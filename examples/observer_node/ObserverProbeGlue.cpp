// ---------------------------------------------------------------------------
// ObserverProbeGlue.cpp -- ties the ObserverProbe schedule to MyMesh's mesh I/O.
//
// Part of the observer_node tree (build_src_filter globs *.cpp). The whole body
// is gated on WITH_OBSERVER_PROBE, which every observer_node env defines.
//
// Flow (single probe in flight):
//   observerProbeLoop()        picks a due repeater -> sendLogin(contact, "")
//   observerProbeOnLogin(ok)   login OK  -> sendRequest(GET_TELEMETRY_DATA)
//                              login FAIL -> markFail (operator set a password)
//   observerProbeOnTelemetry() parse CayenneLPP (battery + MCU temp) -> publish
//   observerPublishPath()      fired from onContactPathUpdated (path learned by
//                              the flood login) -> publish hop chain for the map
//
// Publishing is decoupled via weak extern "C" hooks; the _wifi observer build
// overrides them in main.cpp to push MQTT. Non-wifi builds keep the no-ops.
// ---------------------------------------------------------------------------

#ifdef WITH_OBSERVER_PROBE

#include "MyMesh.h"   // pulls in Mesh.h + ADV_TYPE_* / ContactInfo / REQ_TYPE_*

// CayenneLPP data-type ids (subset we need to walk the telemetry reply).
#define LPP_TEMPERATURE   103   // 2 bytes, signed, 0.1 C
#define LPP_VOLTAGE       116   // 2 bytes, unsigned, 0.01 V
#define TEMP_NONE         0x7FFF

// Publish sinks. Weak no-ops here; main.cpp (observer _wifi build) provides the
// strong MQTT implementations. pk_hex is the 8-char pubkey prefix.
extern "C" __attribute__((weak))
void observerProbePublish(const char* pk_hex, uint16_t batt_mv, int16_t temp_dc, uint32_t ok_epoch) { }
extern "C" __attribute__((weak))
void observerProbePublishPath(const char* pk_hex, const char* hops_hex) { }

static void pkHex(const uint8_t* pub_key, char out[9]) {
  for (int i = 0; i < 4; i++) snprintf(out + i * 2, 3, "%02x", pub_key[i]);
}

// Byte length of a CayenneLPP value for the types a repeater can emit. Returns
// -1 for anything unknown, which stops the walk (no OOB read) -- we keep
// whatever we parsed up to that point. Battery is always added first by the
// repeater, so it is captured even if a later unknown sensor truncates us.
static int lppValueLen(uint8_t type) {
  switch (type) {
    case 0: case 1: case 102: case 104: case 120: case 142: return 1;
    case 2: case 3: case 101: case 103: case 115: case 116:
    case 117: case 121: case 125: case 128:                 return 2;
    case 118: case 134:                                     return 4;
    case 113:                                               return 6;
    case 136:                                               return 9;   // GPS
    default:                                                return -1;
  }
}

void MyMesh::observerProbeLoop() {
  uint32_t now_ms = millis();

  if (_probe_state != PROBE_IDLE) {                 // one probe in flight
    if ((int32_t)(now_ms - _probe_deadline) >= 0) { // login/telemetry timed out
      _probe.markFail(_probe_key);
      _probe_state = PROBE_IDLE;
    }
    return;
  }
  if ((int32_t)(now_ms - _probe_next_scan) < 0) return;   // airtime throttle
  _probe_next_scan = now_ms + OBS_PROBE_MIN_GAP_S * 1000UL;

  uint32_t now_epoch = getRTCClock()->getCurrentTime();
  if (now_epoch < 1600000000UL) return;   // clock not set yet: twice-daily schedule
                                          // is meaningless, and epoch 0 would alias
                                          // the "never probed" sentinel -> hold off.
  int nc = getNumContacts();
  if (nc <= 0) return;

  for (int n = 0; n < nc; n++) {                    // round-robin from cursor
    int idx = (_probe_cursor + n) % nc;
    ContactInfo c;
    if (!getContactByIdx((uint32_t)idx, c)) continue;
    if (c.type != ADV_TYPE_REPEATER) continue;
    if (!_probe.isDue(c.id.pub_key, now_epoch)) continue;

    uint32_t est;
    int r = sendLogin(c, "", est);                  // BLANK password, single try
    if (r == MSG_SEND_FAILED) { _probe.markFail(c.id.pub_key); continue; }

    memcpy(&pending_login, c.id.pub_key, 4);        // match in onContactResponse()
    memcpy(_probe_key, c.id.pub_key, 4);
    _probe.markAttempt(c.id.pub_key, now_epoch);
    _probe_state = PROBE_WAIT_LOGIN;
    _probe_deadline = now_ms + OBS_PROBE_TIMEOUT_S * 1000UL;
    _probe_cursor = idx + 1;
    return;                                         // launched; wait for reply
  }
}

void MyMesh::observerProbeOnLogin(const ContactInfo& contact, bool ok) {
  if (_probe_state != PROBE_WAIT_LOGIN) return;
  if (memcmp(_probe_key, contact.id.pub_key, 4) != 0) return;

  if (!ok) {                                        // password set -> back off
    _probe.markFail(_probe_key);
    _probe_state = PROBE_IDLE;
    return;
  }
  uint32_t tag, est;
  int r = sendRequest(contact, REQ_TYPE_GET_TELEMETRY_DATA, tag, est);
  if (r == MSG_SEND_FAILED) {
    _probe.markFail(_probe_key);
    _probe_state = PROBE_IDLE;
    return;
  }
  pending_telemetry = tag;                          // match in onContactResponse()
  _probe_state = PROBE_WAIT_TELEM;
  _probe_deadline = millis() + OBS_PROBE_TIMEOUT_S * 1000UL;
}

void MyMesh::observerProbeOnTelemetry(const ContactInfo& contact, const uint8_t* lpp, uint8_t lpp_len) {
  if (_probe_state != PROBE_WAIT_TELEM) return;
  if (memcmp(_probe_key, contact.id.pub_key, 4) != 0) return;
  _probe_state = PROBE_IDLE;

  uint16_t batt_mv = 0;
  int16_t  temp_dc = TEMP_NONE;

  int i = 0;                                        // walk [chan][type][value..]
  while (i + 2 <= lpp_len) {
    uint8_t type = lpp[i + 1];
    int vlen = lppValueLen(type);
    if (vlen < 0 || i + 2 + vlen > lpp_len) break;  // unknown/truncated -> stop
    const uint8_t* v = &lpp[i + 2];
    if (type == LPP_VOLTAGE)          batt_mv = (uint16_t)(((v[0] << 8) | v[1]) * 10); // 0.01V -> mV
    else if (type == LPP_TEMPERATURE) temp_dc = (int16_t)((v[0] << 8) | v[1]);         // already 0.1C
    i += 2 + vlen;
  }

  uint32_t now_epoch = getRTCClock()->getCurrentTime();
  _probe.markOk(contact.id.pub_key, now_epoch, batt_mv, temp_dc);

  char pk[9]; pkHex(contact.id.pub_key, pk);
  observerProbePublish(pk, batt_mv, temp_dc, now_epoch);
}

// Called from MyMesh::onContactPathUpdated (guarded). The flood login taught us
// the hop chain to this repeater -- publish it so the map/topology stays fresh.
void MyMesh::observerPublishPath(const ContactInfo& contact) {
  if (contact.type != ADV_TYPE_REPEATER) return;
  if (contact.out_path_len == OUT_PATH_UNKNOWN || contact.out_path_len == 0) return;
  if (contact.out_path_len > MAX_PATH_SIZE) return;

  char hops[MAX_PATH_SIZE * 2 + 1];
  for (int i = 0; i < contact.out_path_len; i++)
    snprintf(hops + i * 2, 3, "%02x", contact.out_path[i]);

  char pk[9]; pkHex(contact.id.pub_key, pk);
  observerProbePublishPath(pk, hops);
}

#endif // WITH_OBSERVER_PROBE
