#pragma once

// ---------------------------------------------------------------------------
// ObserverProbe -- schedule + policy for the observer node's active telemetry
// prober. Compiled only into the `*_observer_node_*` build envs, which define
// WITH_OBSERVER_PROBE and leave WITH_BOT_COMMANDS / WITH_RELAY_POLICY OFF.
//
// The node walks its known-repeater roster and, twice a day per repeater,
// performs a *blank-password* login followed by a REQ_TYPE_GET_TELEMETRY_DATA
// request -- exactly the exchange the phone app drives via CMD_SEND_TELEMETRY_REQ,
// just scheduled instead of manual. It reuses MyMesh's existing sendLogin() /
// sendRequest() / onContactResponse() machinery; this class holds only the
// per-repeater schedule and the accept/backoff policy. No mesh access here.
//
// Policy notes (deliberate, do not "fix" without understanding):
//  - BLANK PASSWORD ONLY. We send one empty-string login. A repeater whose
//    operator set a guest/admin password simply rejects it (handleLoginReq
//    returns 0) and we back off hard. We never try a second credential -- this
//    is a designed-in, operator-gated telemetry read, not credential guessing.
//  - Guest login yields base telemetry only (battery + MCU temp). That is all
//    we want: a network health/coverage map.
//  - Single probe in flight, PROBE_MIN_GAP_S between attempts: the prober is an
//    active TX participant and flood logins cost the whole mesh airtime.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <string.h>

// Twice per day per repeater.
#ifndef OBS_PROBE_PERIOD_S
#define OBS_PROBE_PERIOD_S      (12 * 3600UL)
#endif
// Minimum gap between consecutive probe attempts (airtime throttle).
#ifndef OBS_PROBE_MIN_GAP_S
#define OBS_PROBE_MIN_GAP_S     45UL
#endif
// How long to wait for a login / telemetry reply before giving up.
#ifndef OBS_PROBE_TIMEOUT_S
#define OBS_PROBE_TIMEOUT_S     30UL
#endif
// After a refusal (password set) or repeated silence, wait this long before
// re-attempting -- keeps us off secured/dead nodes without abandoning them.
#ifndef OBS_PROBE_FAIL_BACKOFF_S
#define OBS_PROBE_FAIL_BACKOFF_S (24 * 3600UL)
#endif
// Max distinct repeaters we track schedule state for.
#ifndef OBS_PROBE_MAX_TRACKED
#define OBS_PROBE_MAX_TRACKED   128
#endif

struct ObserverProbeSlot {
  uint8_t  key[4];         // pubkey prefix (matches ContactInfo id.pub_key[0..3])
  uint32_t last_attempt;   // epoch secs, 0 = never
  uint32_t last_ok;        // epoch secs of last good telemetry
  uint16_t last_batt_mv;   // most recent battery reading
  int16_t  last_temp_dc;   // most recent temperature, deci-degrees C (0x7FFF = none)
  uint8_t  consec_fail;    // login rejected / timed out in a row
  bool     used;
};

class ObserverProbe {
public:
  ObserverProbe() { memset(_slots, 0, sizeof(_slots)); }

  // Is this repeater due for a probe right now (respecting period + backoff)?
  bool isDue(const uint8_t* pubkey, uint32_t now_epoch) const {
    const ObserverProbeSlot* s = find(pubkey);
    if (!s || s->last_attempt == 0) return true;      // never probed
    uint32_t wait = (s->consec_fail > 0) ? OBS_PROBE_FAIL_BACKOFF_S : OBS_PROBE_PERIOD_S;
    return (now_epoch - s->last_attempt) >= wait;
  }

  // Record that we just launched a probe (login sent).
  void markAttempt(const uint8_t* pubkey, uint32_t now_epoch) {
    ObserverProbeSlot* s = getOrAdd(pubkey);
    if (s) s->last_attempt = now_epoch;
  }

  // Telemetry came back: clear failure streak, stash the readings.
  void markOk(const uint8_t* pubkey, uint32_t now_epoch, uint16_t batt_mv, int16_t temp_dc) {
    ObserverProbeSlot* s = getOrAdd(pubkey);
    if (!s) return;
    s->last_ok = now_epoch;
    s->last_batt_mv = batt_mv;
    s->last_temp_dc = temp_dc;
    s->consec_fail = 0;
  }

  // Login rejected (password set) or no reply -> back off.
  void markFail(const uint8_t* pubkey) {
    ObserverProbeSlot* s = getOrAdd(pubkey);
    if (s && s->consec_fail < 255) s->consec_fail++;
  }

private:
  ObserverProbeSlot _slots[OBS_PROBE_MAX_TRACKED];

  const ObserverProbeSlot* find(const uint8_t* k) const {
    for (int i = 0; i < OBS_PROBE_MAX_TRACKED; i++)
      if (_slots[i].used && memcmp(_slots[i].key, k, 4) == 0) return &_slots[i];
    return NULL;
  }
  ObserverProbeSlot* getOrAdd(const uint8_t* k) {
    int free_idx = -1, oldest_idx = -1; uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < OBS_PROBE_MAX_TRACKED; i++) {
      if (_slots[i].used) {
        if (memcmp(_slots[i].key, k, 4) == 0) return &_slots[i];
        if (_slots[i].last_attempt < oldest) { oldest = _slots[i].last_attempt; oldest_idx = i; }
      } else if (free_idx < 0) free_idx = i;
    }
    int use = (free_idx >= 0) ? free_idx : oldest_idx;   // evict least-recently-probed
    if (use < 0) return NULL;
    ObserverProbeSlot* s = &_slots[use];
    memset(s, 0, sizeof(*s));
    memcpy(s->key, k, 4);
    s->last_temp_dc = 0x7FFF;
    s->used = true;
    return s;
  }
};
