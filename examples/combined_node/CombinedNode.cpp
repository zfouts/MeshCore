// combined_node — robustness extensions (watchdog, low-battery shutdown,
// mobile GPS advertising, heard-neighbour table, relay/RX stats). Compiled only
// by the *_combined_node_* envs (which define -D WITH_COMBINED_EXTRAS); a clean
// compile-time extension of companion_radio. All methods here are members of
// companion's MyMesh. Tunables are compile-time build flags (see CombinedNode.h).

#include "MyMesh.h"

#ifdef WITH_COMBINED_EXTRAS

#include "CombinedNode.h"
#include <Arduino.h>
#include <string.h>
// base64.hpp DEFINES its functions in the header, so it can only be included in
// one translation unit (BaseChatMesh.cpp already does). Forward-declare the one
// we need for `set bot_channel <name>,<base64psk>` and let the linker resolve it.
extern unsigned int decode_base64(const unsigned char* input, unsigned int input_length, unsigned char* output);

#if defined(ESP32)
#include "esp_task_wdt.h"
#endif

// Piecewise-linear single-cell LiPo discharge curve (resting voltage). Points
// run high->low; we clamp the ends and interpolate between brackets.
uint8_t combinedLipoPercent(uint16_t mv) {
  static const struct { uint16_t mv; uint8_t pct; } curve[] = {
    {4200,100},{4150,95},{4110,90},{4080,85},{4020,80},{3980,75},{3950,70},
    {3910,65},{3870,60},{3850,55},{3840,50},{3820,45},{3800,40},{3790,35},
    {3770,30},{3750,25},{3730,20},{3710,15},{3690,10},{3610,5},{3270,0}
  };
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0].mv)     return 100;
  if (mv <= curve[n-1].mv)   return 0;
  for (int i = 1; i < n; i++) {
    if (mv >= curve[i].mv) {  // bracketed by curve[i-1] (higher) and curve[i] (lower)
      uint16_t lo_mv = curve[i].mv,  hi_mv = curve[i-1].mv;
      uint8_t  lo_p  = curve[i].pct, hi_p  = curve[i-1].pct;
      return lo_p + (uint8_t)(((uint32_t)(mv - lo_mv) * (hi_p - lo_p)) / (hi_mv - lo_mv));
    }
  }
  return 0;
}

void MyMesh::combinedBegin() {
#ifdef BOT_DEBUG
  Serial.begin(115200);
  Serial.println("[bot] combinedBegin: BOT_DEBUG console up");
#endif
  if (_combined) return;
  _combined = new CombinedState();
  memset(&_combined->stats, 0, sizeof(_combined->stats));
  memset(_combined->neighbours, 0, sizeof(_combined->neighbours));
  _combined->stats.boot_rtc = getRTCClock()->getCurrentTime();
  _combined->next_advert_ms = 0;
  _combined->low_batt_strikes = 0;
  _combined->wdt_on = false;
  // bot_enabled / bot_channel live in _prefs (already loaded by begin()).

#if defined(ESP32)
  // Hardware watchdog: panic-reboot if loop() stalls for COMBINED_WDT_TIMEOUT_S.
  esp_task_wdt_init(COMBINED_WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  _combined->wdt_on = true;
#elif defined(NRF52_PLATFORM)
  // nRF52 hardware watchdog. SLEEP_Pause makes it count only while the CPU is
  // awake, so the idle wake-on-packet loop can't trip it, while a genuinely
  // hung awake loop still resets the chip after COMBINED_WDT_TIMEOUT_S.
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos)
                  | (WDT_CONFIG_SLEEP_Pause << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV  = (COMBINED_WDT_TIMEOUT_S * 32768) - 1; // 32.768 kHz LFCLK
  NRF_WDT->RREN = (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos);
  NRF_WDT->TASKS_START = 1;
  _combined->wdt_on = true;
#endif
}

void MyMesh::combinedLoop() {
  if (!_combined) return;

#if defined(ESP32)
  if (_combined->wdt_on) esp_task_wdt_reset(); // feed the dog
#elif defined(NRF52_PLATFORM)
  if (_combined->wdt_on) NRF_WDT->RR[0] = WDT_RR_RR_Reload; // feed the dog
#endif

  // Low-battery auto power-off, debounced so a single noisy dip can't trip it.
#if COMBINED_LOW_BATT_MV > 0
  uint16_t mv = board.getBattMilliVolts();
  // Only act on a *plausible* low battery. A reading below the floor means no
  // battery / USB power, where powerOff() (deep sleep) would be catastrophic.
  if (mv > COMBINED_LOW_BATT_FLOOR && mv < COMBINED_LOW_BATT_MV) {
    if (++_combined->low_batt_strikes >= COMBINED_LOW_BATT_STRIKES) {
#if COMBINED_LOWBATT_BEACON
      // Broadcast a final "going dark" message so the mesh knows this was a
      // graceful low-battery shutdown (vs. a crash/theft/dead panel). Blocks
      // until the packet actually transmits, then sleeps.
      combinedLowBattBeacon(mv);
#endif
      // Solar/unattended: shut down with a self-recovery wake (LPCOMP voltage
      // wake on nRF52) so the node revives when the panel recharges at dawn.
      // Falls back to powerOff() on boards without a voltage-wake path.
      board.powerOffUntilCharged(); // does not return on supported boards
    }
  } else {
    _combined->low_batt_strikes = 0;
  }
#endif

  // Periodic zero-hop location advert so the mesh tracks a moving node.
#if COMBINED_ADVERT_INTERVAL_S > 0
  if (millisHasNowPassed(_combined->next_advert_ms)) {
    advert();
    _combined->next_advert_ms = futureMillis(COMBINED_ADVERT_INTERVAL_S * 1000);
  }
#endif

  // Resend an un-ACKed bot reply (bounded) so a single lost packet on a weak
  // link doesn't swallow a !ping/!path answer. processAck() clears pending.ack
  // when the recipient confirms; reusing the original timestamp keeps the ACK
  // hash stable and lets the recipient dedupe the resends.
#if COMBINED_BOT_REPLY_RETRIES > 0
  CombinedPendingReply& pr = _combined->pending;
  if (pr.ack != 0 && millisHasNowPassed(pr.next_ms)) {
    if (pr.attempts_left > 0) {
      pr.attempts_left--;
      pr.attempt++;
      uint32_t ea = 0, tmo = 0;
      sendMessage(pr.to, pr.timestamp, pr.attempt, pr.text, ea, tmo);
      pr.next_ms = futureMillis(tmo > 0 ? tmo : COMBINED_BOT_REPLY_TIMEOUT_MS);
    } else {
      pr.ack = 0;   // gave up; stop tracking
    }
  }
#endif
}

void MyMesh::combinedOnRx(float snr, float rssi) {
  if (!_combined) return;
  _combined->stats.rx_count++;
  _combined->last_snr = snr;   // most recent link quality, surfaced by !ping
  _combined->last_rssi = rssi;
#ifdef BOT_DEBUG
  Serial.printf("[bot] RX raw snr=%.1f rssi=%.0f\n", (double)snr, (double)rssi);
#endif
}

void MyMesh::combinedCountForward(bool allowed) {
  if (!_combined) return;
  if (allowed) _combined->stats.relayed++;
  else         _combined->stats.dropped++;
}

void MyMesh::combinedOnNeighbour(const ContactInfo& contact, uint8_t path_len) {
  if (!_combined || path_len != 0) return; // only directly-heard (0-hop) nodes

  uint32_t now = getRTCClock()->getCurrentTime();
  CombinedNeighbour* slot = NULL;
  CombinedNeighbour* oldest = &_combined->neighbours[0];
  for (int i = 0; i < COMBINED_MAX_NEIGHBOURS; i++) {
    CombinedNeighbour* e = &_combined->neighbours[i];
    if (e->used && memcmp(e->prefix, contact.id.pub_key, 4) == 0) { slot = e; break; }
    if (!e->used && !slot) slot = e;            // first free slot
    if (e->last_heard < oldest->last_heard) oldest = e;
  }
  if (!slot) slot = oldest;                     // table full -> evict oldest

  memcpy(slot->prefix, contact.id.pub_key, 4);
  strncpy(slot->name, contact.name, sizeof(slot->name) - 1);
  slot->name[sizeof(slot->name) - 1] = 0;
  slot->last_heard = now;
  slot->used = true;
}

void MyMesh::combinedFormatStats(char* reply, size_t sz) {
  uint32_t up = _ms->getMillis() / 1000;
  uint16_t mv = board.getBattMilliVolts();
  snprintf(reply, sz, "rx:%lu relay:%lu drop:%lu up:%lus batt:%dmV(%d%%) relay:%s",
           (unsigned long)(_combined ? _combined->stats.rx_count : 0),
           (unsigned long)(_combined ? _combined->stats.relayed : 0),
           (unsigned long)(_combined ? _combined->stats.dropped : 0),
           (unsigned long)up, (int)mv, (int)combinedLipoPercent(mv),
           _prefs.client_repeat ? "on" : "off");
}

#if COMBINED_LOWBATT_BEACON
// Broadcast a one-shot "going dark" message on the configured bot channel right
// before a low-battery shutdown, then BLOCK until it actually transmits.
//
// LoRa TX is async: sendGroupMessage() only queues a packet -- the radio sends
// it later from the dispatcher loop. Since the caller is about to SYSTEMOFF (and
// never return), we must pump the radio here until the queue drains, or the
// beacon would die unsent. We call BaseChatMesh::loop() directly (not our own
// loop()) so we service the radio without re-entering the low-batt check, and we
// feed the watchdog ourselves since combinedLoop() is bypassed.
void MyMesh::combinedLowBattBeacon(uint16_t mv) {
  if (!_combined) return;
  if (_prefs.bot_channel == 0xFF) return;            // no bot channel -> nowhere to send

  ChannelDetails d;
  if (!getChannel(_prefs.bot_channel, d) || !d.name[0]) return; // channel gone

  char text[100];
  uint32_t up = _ms->getMillis() / 1000;
  snprintf(text, sizeof(text), "%s going dark batt:%dmV(%d%%) up:%luh",
           _prefs.node_name, (int)mv, (int)combinedLipoPercent(mv),
           (unsigned long)(up / 3600));

  // A channel message is a flood send; the dispatcher bumps its sent-flood
  // counter only when a packet finishes transmitting. Snapshot it, send, then
  // pump the radio until it ticks (TX completed) or we time out.
  uint32_t sent_before = getNumSentFlood();
  if (!sendGroupMessage(getRTCClock()->getCurrentTimeUnique(), d.channel,
                        _prefs.node_name, text, strlen(text))) return;

  // Drain: BaseChatMesh::loop() services radio TX without re-entering
  // combinedLoop() (avoids recursing the low-batt check); feed the watchdog
  // ourselves since that bypasses combinedLoop's feed. Best-effort: a TX
  // timeout/fail won't tick the counter, so we just sleep after the window.
  uint32_t start = _ms->getMillis();
  while (getNumSentFlood() == sent_before
         && (_ms->getMillis() - start) < COMBINED_LOWBATT_BEACON_DRAIN_MS) {
    BaseChatMesh::loop();
#if defined(ESP32)
    if (_combined->wdt_on) esp_task_wdt_reset();
#elif defined(NRF52_PLATFORM)
    if (_combined->wdt_on) NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif
  }
}
#endif // COMBINED_LOWBATT_BEACON

void MyMesh::combinedFormatNeighbours(char* reply, size_t sz) {
  if (!_combined) { snprintf(reply, sz, "no data"); return; }
  uint32_t now = getRTCClock()->getCurrentTime();
  int count = 0;
  size_t pos = 0;
  pos += snprintf(reply + pos, sz - pos, "heard:");
  for (int i = 0; i < COMBINED_MAX_NEIGHBOURS && pos < sz - 1; i++) {
    CombinedNeighbour* e = &_combined->neighbours[i];
    if (!e->used) continue;
    uint32_t age = (now >= e->last_heard) ? (now - e->last_heard) : 0;
    pos += snprintf(reply + pos, sz - pos, " %s(%lus)", e->name, (unsigned long)age);
    count++;
  }
  if (count == 0) snprintf(reply, sz, "heard: none yet");
}

// Clear the pending bot reply if `ack` matches its expected ACK, so the
// combinedLoop() retry stops. Called from processAck() (bot-reply ACKs are not
// tracked in the app's expected_ack_table).
void MyMesh::combinedNotifyAck(const uint8_t* ack) {
  if (_combined && _combined->pending.ack != 0 &&
      memcmp(ack, &_combined->pending.ack, 4) == 0) {
    _combined->pending.ack = 0;
  }
}

// Resolve a path-hash entry (hsz bytes, a prefix of a node's public key) to a
// friendly name for !path: this node, or a known contact whose identity hash
// matches. Best-effort -- a short (1-2 byte) hash can alias more than one node,
// and repeaters we've never adverted-heard won't be contacts, so many hops will
// still fall back to hex at the call site. Returns true and fills `out` on a hit.
bool MyMesh::resolvePathHash(const uint8_t* hash, uint8_t hsz, char* out, size_t outsz) {
  if (!out || outsz == 0) return false;
  if (self_id.isHashMatch(hash, hsz)) {                 // us
    StrHelper::strncpy(out, _prefs.node_name, outsz);
    return out[0] != 0;
  }
  ContactsIterator it = startContactsIterator();        // a known contact
  ContactInfo c;
  while (it.hasNext(this, c)) {
    if (c.name[0] && c.id.isHashMatch(hash, hsz)) {
      StrHelper::strncpy(out, c.name, outsz);
      return out[0] != 0;
    }
  }
  return false;
}

// --- meshcore-cli custom vars (CMD_GET/SET_CUSTOM_VARS) -----------------------

// Resolve a channel argument to a channel index. Accepts: "off"/"none" (0xFF),
// a numeric index, "<name>,<base64psk>" (joins a private channel into a free
// slot; 16- or 32-byte key), an already-joined channel name (leading '#'
// optional), or "#name" (auto-joins the hashtag channel; key = first 16 bytes
// of sha256("#name")). Returns the index, 0xFF for off, or -1 on failure.
// Shared by `set bot_channel` and `set bot_control_channel`.
int MyMesh::combinedResolveChannelArg(const char* value) {
  if (strcmp(value, "off") == 0 || strcmp(value, "none") == 0) return 0xFF;
  if (value[0] >= '0' && value[0] <= '9') {       // numeric channel index
    int idx = atoi(value);
#ifdef MAX_GROUP_CHANNELS
    if (idx < 0 || idx >= MAX_GROUP_CHANNELS) return -1;  // out of range (also blocks 255 = off-sentinel)
#endif
    return idx;
  }
#ifdef MAX_GROUP_CHANNELS
  // Private channel, one-step: "<name>,<base64psk>" joins a private
  // (random-key) channel into a free slot. Mirrors the hashtag auto-join below
  // but sources the key from the supplied base64 instead of sha256(name).
  // Comma-separated so meshcli passes it as a single token (like `set radio f,bw,sf,cr`).
  const char* comma = strchr(value, ',');
  if (comma && comma > value) {
    char chname[32];
    int nlen = comma - value;
    if (nlen > (int)sizeof(chname) - 1) nlen = sizeof(chname) - 1;
    memcpy(chname, value, nlen);
    chname[nlen] = 0;
    const char* psk_b64 = comma + 1;
    uint8_t key[32];
    int klen = decode_base64((unsigned char*)psk_b64, strlen(psk_b64), key);
    if (klen != 16 && klen != 32) return -1;     // bad/short key
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails d;
      if (!getChannel(i, d) || d.name[0] != 0) continue; // first free slot
      ChannelDetails nc;
      memset(&nc, 0, sizeof(nc));
      strncpy(nc.name, chname, sizeof(nc.name) - 1);
      memcpy(nc.channel.secret, key, klen); // 128- or 256-bit key (channel.secret is 32B); setChannel derives hash
      if (setChannel(i, nc)) {
        saveChannels();
        return i;
      }
      break;
    }
    return -1; // no free slot
  }

  // Otherwise treat it as a channel name (ignoring a leading '#').
  const char* want = (value[0] == '#') ? value + 1 : value;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails d;
    if (!getChannel(i, d)) continue;
    const char* nm = (d.name[0] == '#') ? d.name + 1 : d.name;
    if (nm[0] && strcmp(nm, want) == 0) return i;
  }

  // Not joined yet. For a hashtag channel ("#name") the key is deterministic
  // -- first 16 bytes of sha256("#name") -- so we can auto-join it into a free
  // slot. (Private channels use a random key we can't derive from a name.)
  if (value[0] == '#') {
    char chname[32];
    strncpy(chname, value, sizeof(chname) - 1);
    chname[sizeof(chname) - 1] = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails d;
      if (!getChannel(i, d) || d.name[0] != 0) continue; // first free slot
      ChannelDetails nc;
      memset(&nc, 0, sizeof(nc));
      strncpy(nc.name, chname, sizeof(nc.name) - 1);
      uint8_t full[32];
      mesh::Utils::sha256(full, sizeof(full), (const uint8_t*)chname, (int)strlen(chname));
      memcpy(nc.channel.secret, full, 16); // 128-bit hashtag key; setChannel derives hash
      if (setChannel(i, nc)) {
        saveChannels();
        return i;
      }
      break;
    }
  }
#endif
  return -1; // unknown channel name -> reported as an error to the CLI
}

// `set bot_enable 0|1`, `set bot_channel <idx|off|name|#tag|name,b64psk>`,
// `set bot_control_channel <same syntax>`. Returns true if handled.
bool MyMesh::combinedSetVar(const char* name, const char* value) {
  if (!_combined) return false;

  if (strcmp(name, "bot_enable") == 0) {
    _prefs.bot_enabled = (value[0] == '1') ? 1 : 0;
    savePrefs();
    return true;
  }
  bool is_bot_ch = strcmp(name, "bot_channel") == 0;
  bool is_ctl_ch = strcmp(name, "bot_control_channel") == 0;
  if (is_bot_ch || is_ctl_ch) {
    int idx = combinedResolveChannelArg(value);
    if (idx < 0) return false;
    if (is_bot_ch) _prefs.bot_channel = (uint8_t)idx;
    else           _prefs.bot_control_channel = (uint8_t)idx;
    savePrefs();
    return true;
  }
  return false;
}

// Append our vars to the custom-vars GET reply. `base` is the start of the
// value region; `dp` is the current write cursor; `end` is one-past the last
// writable byte of the caller's frame. Returns the new cursor. The whole
// "bot_enable:..,bot_channel:.." string is built in a local buffer first and
// only appended if it fits, so a long channel name can never overrun `end`
// (a prior sensor loop may already have filled the frame close to its cap).
// Format one "name:<channel>" var into buf+n: the channel's name if joined,
// its index otherwise, or "off" for the 0xFF sentinel.
int MyMesh::combinedFormatChannelVar(char* buf, int n, size_t bufsz, const char* name, uint8_t ch) {
  if (n < 0 || n >= (int)bufsz - 1) return n;   // buffer already full
  if (ch == 0xFF) return n + snprintf(buf + n, bufsz - n, "%s:off", name);
#ifdef MAX_GROUP_CHANNELS
  ChannelDetails d;
  if (getChannel(ch, d) && d.name[0]) return n + snprintf(buf + n, bufsz - n, "%s:%s", name, d.name);
#endif
  return n + snprintf(buf + n, bufsz - n, "%s:%d", name, ch);
}

char* MyMesh::combinedAppendVars(char* base, char* dp, const char* end) {
  if (!_combined) return dp;
  char buf[128];
  int n = 0;
  if (dp > base) buf[n++] = ',';
  n += snprintf(buf + n, sizeof(buf) - n, "bot_enable:%d,", _prefs.bot_enabled ? 1 : 0);
  n = combinedFormatChannelVar(buf, n, sizeof(buf), "bot_channel", _prefs.bot_channel);
  if (n < (int)sizeof(buf) - 1) n += snprintf(buf + n, sizeof(buf) - n, ",");
  n = combinedFormatChannelVar(buf, n, sizeof(buf), "bot_control_channel", _prefs.bot_control_channel);
  if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;   // snprintf clamp (defensive)
  if (n > 0 && n <= (int)(end - dp)) {              // only append if it fully fits
    memcpy(dp, buf, n);
    dp += n;
  }
  return dp;
}

#endif // WITH_COMBINED_EXTRAS
