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
#include "esp_idf_version.h"   // ESP_IDF_VERSION_MAJOR (WDT API changed in IDF5)
#include "esp_system.h"        // esp_reset_reason() for `!boot`
#elif defined(NRF52_PLATFORM)
#include <nrf_sdm.h>           // sd_softdevice_is_enabled (boot-reason read path)
#include <nrf_soc.h>           // sd_power_reset_reason_get/_clr
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
  _combined->advert_interval_s = COMBINED_ADVERT_INTERVAL_S;
  _combined->low_batt_strikes = 0;
  _combined->wdt_on = false;
  // bot_enabled / bot_channel live in _prefs (already loaded by begin()).

  // Migrate the single legacy bot_channel into the multi-channel mask (the
  // bot answers on every channel whose bit is set). Persisted by the
  // savePrefs() below.
  if (_prefs.bot_channel_mask == 0 && _prefs.bot_channel != 0xFF)
    _prefs.bot_channel_mask = 1ULL << _prefs.bot_channel;

  // Why did we boot? Captured once for `!boot`. The hardware register is
  // cumulative, so clear it after reading -- the NEXT boot then reports only
  // its own cause.
#if defined(ESP32)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   strcpy(_combined->boot_reason, "power-on");   break;
    case ESP_RST_SW:        strcpy(_combined->boot_reason, "sw-reset");   break;
    case ESP_RST_PANIC:     strcpy(_combined->boot_reason, "panic");      break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       strcpy(_combined->boot_reason, "watchdog");   break;
    case ESP_RST_BROWNOUT:  strcpy(_combined->boot_reason, "brownout");   break;
    case ESP_RST_DEEPSLEEP: strcpy(_combined->boot_reason, "sleep-wake"); break;
    case ESP_RST_EXT:       strcpy(_combined->boot_reason, "ext-reset");  break;
    default:                strcpy(_combined->boot_reason, "other");      break;
  }
#elif defined(NRF52_PLATFORM)
  // combinedBegin() runs before the BLE interface starts, so the SoftDevice is
  // normally still off and the register is directly readable -- but check, in
  // case the init order ever changes (direct access faults with the SD live).
  uint32_t reas = 0;
  uint8_t sd_en = 0;
  sd_softdevice_is_enabled(&sd_en);
  if (sd_en) { sd_power_reset_reason_get(&reas); sd_power_reset_reason_clr(reas); }
  else       { reas = NRF_POWER->RESETREAS; NRF_POWER->RESETREAS = reas; }
  // A plain power cycle (and nRF52 brownout) leaves no bits set. LPCOMP is the
  // powerOffUntilCharged() dawn wake -- a solar node that slept and recovered.
  if      (reas == 0)                           strcpy(_combined->boot_reason, "power-on");
  else if (reas & POWER_RESETREAS_DOG_Msk)      strcpy(_combined->boot_reason, "watchdog");
  else if (reas & POWER_RESETREAS_LOCKUP_Msk)   strcpy(_combined->boot_reason, "lockup");
  else if (reas & POWER_RESETREAS_SREQ_Msk)     strcpy(_combined->boot_reason, "sw-reset");
  else if (reas & POWER_RESETREAS_LPCOMP_Msk)   strcpy(_combined->boot_reason, "wake-charged");
  else if (reas & POWER_RESETREAS_OFF_Msk)      strcpy(_combined->boot_reason, "gpio-wake");
  else if (reas & POWER_RESETREAS_RESETPIN_Msk) strcpy(_combined->boot_reason, "reset-pin");
  else                                          strcpy(_combined->boot_reason, "other");
#else
  strcpy(_combined->boot_reason, "unknown");
#endif
  _prefs.boot_count++;   // persisted: "rebooted once" vs "brownout-looping"
  savePrefs();

#if defined(ESP32)
  // Hardware watchdog: panic-reboot if loop() stalls for COMBINED_WDT_TIMEOUT_S.
#if ESP_IDF_VERSION_MAJOR >= 5
  // IDF5 (Arduino core 3.x, e.g. ESP32-C6): config-struct API, and the core
  // has usually already init'ed the TWDT -- reconfigure instead of init.
  {
    esp_task_wdt_config_t wdt_cfg = {};
    wdt_cfg.timeout_ms = COMBINED_WDT_TIMEOUT_S * 1000;
    wdt_cfg.idle_core_mask = 0;
    wdt_cfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt_cfg) != ESP_OK) esp_task_wdt_init(&wdt_cfg);
  }
#else
  esp_task_wdt_init(COMBINED_WDT_TIMEOUT_S, true);
#endif
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

  // Deferred `@name reboot` (control channel): armed with a delay so the
  // "rebooting" reply gets on the air before the node drops.
  if (_combined->reboot_at_ms != 0 && millisHasNowPassed(_combined->reboot_at_ms)) {
    board.reboot();  // does not return
  }

  // Battery envelope (min/max since boot) plus a ~1h-old reference for the
  // `!batt` trend. Sampled once a minute, not every loop -- ADC reads aren't
  // free. The reference starts as the boot sample, so during the first hour
  // the "1h" delta is really "since boot".
  if (millisHasNowPassed(_combined->next_batt_ms)) {
    _combined->next_batt_ms = futureMillis(60 * 1000UL);
    uint16_t bmv = board.getBattMilliVolts();
    if (bmv > 0) {
      if (_combined->mv_min == 0 || bmv < _combined->mv_min) _combined->mv_min = bmv;
      if (bmv > _combined->mv_max) _combined->mv_max = bmv;
      if (_combined->mv_1h_ago == 0 || millisHasNowPassed(_combined->next_hour_ms)) {
        _combined->mv_1h_ago = bmv;
        _combined->next_hour_ms = futureMillis(3600 * 1000UL);
      }
    }
  }

  // Periodic zero-hop location advert so the mesh tracks a moving node.
  // Cadence is runtime-adjustable (`@name set advert_interval <s>`, 0 = off);
  // a reboot restores the build default (COMBINED_ADVERT_INTERVAL_S).
  if (_combined->advert_interval_s > 0 && millisHasNowPassed(_combined->next_advert_ms)) {
    advert();
    _combined->next_advert_ms = futureMillis(_combined->advert_interval_s * 1000UL);
  }

  // Wardrive beacon (`!wd on`): ask the fleet, via the control channel, to
  // report the route this node's message took -- each reply is a coverage
  // datapoint for the position we broadcast.
  if (_combined->wd_on && millisHasNowPassed(_combined->next_wd_ms)) {
    _combined->next_wd_ms = futureMillis(COMBINED_WD_INTERVAL_S * 1000);
#ifdef MAX_GROUP_CHANNELS
    ChannelDetails d;
    if (_prefs.bot_control_channel != 0xFF &&
        getChannel(_prefs.bot_control_channel, d) && d.name[0]) {
      char txt[48];
      if (sensors.node_lat != 0.0 || sensors.node_lon != 0.0)
        snprintf(txt, sizeof(txt), "!path %.6f,%.6f", sensors.node_lat, sensors.node_lon);
      else
        strcpy(txt, "!path");
      sendGroupMessage(getRTCClock()->getCurrentTimeUnique(), d.channel,
                       _prefs.node_name, txt, strlen(txt));
    }
#endif
  }

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

// `!heard <arg>`: point query into the neighbour table -- when did THIS node
// last directly hear one specific station? Matches a hex pubkey-prefix first
// (2-8 hex chars, so `!path` hop hashes can be pasted as-is), then falls back
// to a case-insensitive name substring.
void MyMesh::combinedFormatHeard(const char* arg, char* reply, size_t sz) {
  if (!_combined) { snprintf(reply, sz, "no data"); return; }

  uint8_t hx[4];
  int hxn = 0;
  size_t alen = strlen(arg);
  if (alen >= 2 && alen <= 8 && (alen & 1) == 0) {
    while (hxn < (int)(alen / 2)) {
      unsigned b;
      if (sscanf(arg + hxn * 2, "%2x", &b) != 1) { hxn = 0; break; }
      hx[hxn++] = (uint8_t)b;
    }
  }

  uint32_t now = getRTCClock()->getCurrentTime();
  for (int i = 0; i < COMBINED_MAX_NEIGHBOURS; i++) {
    CombinedNeighbour* e = &_combined->neighbours[i];
    if (!e->used) continue;
    bool hit = (hxn > 0 && memcmp(e->prefix, hx, hxn) == 0);
    if (!hit) {
      size_t nl = strlen(e->name);
      for (size_t o = 0; !hit && alen <= nl && o + alen <= nl; o++)
        hit = strncasecmp(e->name + o, arg, alen) == 0;
    }
    if (hit) {
      uint32_t age = (now >= e->last_heard) ? (now - e->last_heard) : 0;
      snprintf(reply, sz, "heard %s (%02x%02x%02x%02x) %lus ago", e->name,
               e->prefix[0], e->prefix[1], e->prefix[2], e->prefix[3],
               (unsigned long)age);
      return;
    }
  }
  snprintf(reply, sz, "not heard: %s", arg);
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
    // Idempotent: if a channel with this name already exists, re-key it in
    // place instead of joining a duplicate. A duplicate is worse than useless:
    // incoming packets resolve to the FIRST hash match, so a bot bound to the
    // second copy never sees its own channel's messages.
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails d;
      if (!getChannel(i, d) || strcmp(d.name, chname) != 0) continue;
      memset(d.channel.secret, 0, sizeof(d.channel.secret));
      memcpy(d.channel.secret, key, klen);
      if (!setChannel(i, d)) return -1;
      saveChannels();
      return i;
    }
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

// Runtime-WiFi hooks. Strong definitions live in main.cpp on ESP32 builds with
// WITH_RUNTIME_WIFI; these weak fallbacks make `set wifi_ssid` an error (and
// hide the wifi status var) on builds without WiFi hardware (nRF52 etc).
extern "C" bool combinedApplyWifi(const char* ssid, const char* pwd);
extern "C" __attribute__((weak)) bool combinedApplyWifi(const char*, const char*) { return false; }
extern "C" bool combinedWifiStatus(char* buf, size_t bufsz);
extern "C" __attribute__((weak)) bool combinedWifiStatus(char*, size_t) { return false; }
// `!path` map-link hook: POST the hop chain to the mesh-observer device API
// and get back a short map URL to append to the reply. Strong definition in
// main.cpp on WITH_RUNTIME_WIFI builds; this fallback quietly skips the link
// (the raw-hex reply always goes out either way).
extern "C" bool combinedPathShortUrl(const char* hashes, const char* origin,
                                     const char* requester_pos, const char* reporter,
                                     const char* requester, char* out, size_t outsz);
extern "C" __attribute__((weak)) bool combinedPathShortUrl(const char*, const char*, const char*,
                                                           const char*, const char*, char*, size_t) {
  return false;
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
  bool is_ssid = strcmp(name, "wifi_ssid") == 0;
  if (is_ssid || strcmp(name, "wifi_pwd") == 0) {
    // `-` clears (meshcli can't send an empty value); an SSID/password of a
    // literal single dash is not supported
    const char* v = (strcmp(value, "-") == 0) ? "" : value;
    char* dest = is_ssid ? _prefs.wifi_ssid : _prefs.wifi_pwd;
    size_t destsz = is_ssid ? sizeof(_prefs.wifi_ssid) : sizeof(_prefs.wifi_pwd);
    char old[sizeof(_prefs.wifi_pwd)];
    StrHelper::strzcpy(old, dest, destsz);
    StrHelper::strzcpy(dest, v, destsz);
    if (!combinedApplyWifi(_prefs.wifi_ssid, _prefs.wifi_pwd)) {  // no WiFi on this build
      StrHelper::strzcpy(dest, old, destsz);
      return false;
    }
    savePrefs();
    return true;
  }
  bool is_url = strcmp(name, "obs_url") == 0;
  if (is_url || strcmp(name, "obs_token") == 0) {
    // mesh-observer device API for `!path` map links; `-` clears (see wifi vars).
    // Oversize values are REJECTED, not truncated -- a silently clipped token
    // would 403 on every post and be miserable to debug.
    const char* v = (strcmp(value, "-") == 0) ? "" : value;
    if (is_url) {
      if (v[0] && strncmp(v, "http", 4) != 0) return false;
      if (strlen(v) >= sizeof(_prefs.obs_url)) return false;
      StrHelper::strzcpy(_prefs.obs_url, v, sizeof(_prefs.obs_url));
      size_t l = strlen(_prefs.obs_url);          // store without a trailing '/':
      while (l > 0 && _prefs.obs_url[l - 1] == '/') _prefs.obs_url[--l] = 0; // firmware appends the path
    } else {
      if (strlen(v) >= sizeof(_prefs.obs_token)) return false;
      StrHelper::strzcpy(_prefs.obs_token, v, sizeof(_prefs.obs_token));
    }
    savePrefs();
    return true;
  }
  bool is_bot_ch  = strcmp(name, "bot_channel") == 0;
  bool is_path_ch = strcmp(name, "bot_path_channel") == 0;
  bool is_ctl_ch  = strcmp(name, "bot_control_channel") == 0;
  if (is_bot_ch || is_path_ch || is_ctl_ch) {
    uint64_t* mask = is_bot_ch ? &_prefs.bot_channel_mask
                   : is_path_ch ? &_prefs.bot_path_mask : NULL;
    if (mask && (value[0] == '+' || (value[0] == '-' && value[1]))) {
      // `set bot_channel +#bot` / `-#bot` (same for bot_path_channel):
      // add/remove ONE channel from the set. A bare value replaces the whole
      // set. The legacy bot_channel index stays meaningful as the PRIMARY
      // channel -- it is where the low-battery "going dark" beacon goes.
      bool add = value[0] == '+';
      int idx = combinedResolveChannelArg(value + 1);
      if (idx < 0 || idx == 0xFF || idx >= 64) return false;
      if (add) {
        *mask |= (1ULL << idx);
        if (is_bot_ch && _prefs.bot_channel == 0xFF) _prefs.bot_channel = (uint8_t)idx;
      } else {
        *mask &= ~(1ULL << idx);
        if (is_bot_ch && _prefs.bot_channel == idx) { // removed the primary:
          _prefs.bot_channel = 0xFF;                  // promote the lowest remaining
          for (int i = 0; i < 64; i++)
            if ((_prefs.bot_channel_mask >> i) & 1) { _prefs.bot_channel = (uint8_t)i; break; }
        }
      }
      savePrefs();
      return true;
    }
    int idx = combinedResolveChannelArg(value);
    if (idx < 0) return false;
    if (is_bot_ch) {
      _prefs.bot_channel = (uint8_t)idx;
      _prefs.bot_channel_mask = (idx == 0xFF) ? 0 : (1ULL << idx);
    } else if (is_path_ch) {
      _prefs.bot_path_mask = (idx == 0xFF) ? 0 : (1ULL << idx);
    } else {
      _prefs.bot_control_channel = (uint8_t)idx;
    }
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

// Append "name:<ch>+<ch>+..." for every channel in `mask` ('+'-joined -- ','
// and ':' both belong to the reply framing), or "name:off" for an empty mask.
int MyMesh::combinedFormatChannelMask(char* buf, int n, size_t bufsz, const char* name, uint64_t mask) {
  if (n < 0 || n >= (int)bufsz - 1) return n;
  n += snprintf(buf + n, bufsz - n, "%s:", name);
  if (mask == 0) return n + snprintf(buf + n, bufsz - n, "off");
  bool first = true;
  for (int i = 0; i < 64 && n < (int)bufsz - 1; i++) {
    if (!((mask >> i) & 1)) continue;
    if (!first) n += snprintf(buf + n, bufsz - n, "+");
    first = false;
#ifdef MAX_GROUP_CHANNELS
    ChannelDetails d;
    if (getChannel(i, d) && d.name[0]) { n += snprintf(buf + n, bufsz - n, "%s", d.name); continue; }
#endif
    n += snprintf(buf + n, bufsz - n, "%d", i);
  }
  return n;
}

char* MyMesh::combinedAppendVars(char* base, char* dp, const char* end) {
  if (!_combined) return dp;
  char buf[320];
  int n = 0;
  if (dp > base) buf[n++] = ',';
  n += snprintf(buf + n, sizeof(buf) - n, "bot_enable:%d,", _prefs.bot_enabled ? 1 : 0);
  n = combinedFormatChannelMask(buf, n, sizeof(buf), "bot_channel", _prefs.bot_channel_mask);
  if (n < (int)sizeof(buf) - 1) n += snprintf(buf + n, sizeof(buf) - n, ",");
  n = combinedFormatChannelMask(buf, n, sizeof(buf), "bot_path_channel", _prefs.bot_path_mask);
  if (n < (int)sizeof(buf) - 1) n += snprintf(buf + n, sizeof(buf) - n, ",");
  n = combinedFormatChannelVar(buf, n, sizeof(buf), "bot_control_channel", _prefs.bot_control_channel);
  char wifi_state[40];
  if (n < (int)sizeof(buf) - 1 && combinedWifiStatus(wifi_state, sizeof(wifi_state))) {
    // ssid + connection state only -- the password is never echoed back
    n += snprintf(buf + n, sizeof(buf) - n, ",wifi_ssid:%s,wifi:%s",
                  _prefs.wifi_ssid[0] ? _prefs.wifi_ssid : "off", wifi_state);
    // observer URL only -- the device token is never echoed back. A ':'
    // inside the value would break the reply's name:value,... framing (the
    // stored value is fine -- set splits on the first ':' only), so echo
    // with ';' in place of ':' ("https;//obs.example.com" loses nothing).
    char obs_echo[sizeof(_prefs.obs_url)];
    StrHelper::strzcpy(obs_echo, _prefs.obs_url[0] ? _prefs.obs_url : "off", sizeof(obs_echo));
    for (char* c = obs_echo; *c; c++) if (*c == ':') *c = ';';
    n += snprintf(buf + n, sizeof(buf) - n, ",obs_url:%s", obs_echo);
  }
  if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;   // snprintf clamp (defensive)
  if (n > 0 && n <= (int)(end - dp)) {              // only append if it fully fits
    memcpy(dp, buf, n);
    dp += n;
  }
  return dp;
}

#endif // WITH_COMBINED_EXTRAS
