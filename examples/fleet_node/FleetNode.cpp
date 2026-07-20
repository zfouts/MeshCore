// fleet_node — robustness extensions (watchdog, low-battery shutdown, boot
// reason, deferred reboot, periodic advert) plus the control-room's runtime
// config helpers. Compiled only by the *_fleet_node_* envs (which define
// -D WITH_FLEET_EXTRAS); a clean compile-time extension of companion_radio.
// All methods here are members of companion's MyMesh. Tunables are compile-time
// build flags (see FleetNode.h).

#include "MyMesh.h"

#ifdef WITH_FLEET_EXTRAS

#include "FleetNode.h"
#include <Arduino.h>
#include <string.h>
#include <helpers/sensors/LPPDataHelpers.h>   // LPPReader + LPP_* for `@name sensors`
// base64.hpp DEFINES its functions in the header, so it can only be included in
// one translation unit (BaseChatMesh.cpp already does). Forward-declare the one
// we need for joining a private control channel and let the linker resolve it.
extern unsigned int decode_base64(const unsigned char* input, unsigned int input_length, unsigned char* output);

#if defined(ESP32)
#include "esp_task_wdt.h"
#include "esp_idf_version.h"   // ESP_IDF_VERSION_MAJOR (WDT API changed in IDF5)
#include "esp_system.h"        // esp_reset_reason() for the boot reason
#elif defined(NRF52_PLATFORM)
#include <nrf_sdm.h>           // sd_softdevice_is_enabled (boot-reason read path)
#include <nrf_soc.h>           // sd_power_reset_reason_get/_clr
#endif

// Piecewise-linear single-cell LiPo discharge curve (resting voltage). Points
// run high->low; we clamp the ends and interpolate between brackets.
uint8_t fleetLipoPercent(uint16_t mv) {
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

void MyMesh::fleetBegin() {
#ifdef BOT_DEBUG
  Serial.begin(115200);
  Serial.println("[bot] fleetBegin: BOT_DEBUG console up");
#endif
  if (_fleet) return;
  _fleet = new FleetState();
  _fleet->next_advert_ms = 0;
  _fleet->advert_interval_s = FLEET_ADVERT_INTERVAL_S;
  _fleet->low_batt_strikes = 0;
  _fleet->wdt_on = false;
  // bot_enabled / bot_control_channel live in _prefs (already loaded by begin()).

  // Why did we boot? Captured once. The hardware register is cumulative, so
  // clear it after reading -- the NEXT boot then reports only its own cause.
#if defined(ESP32)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   strcpy(_fleet->boot_reason, "power-on");   break;
    case ESP_RST_SW:        strcpy(_fleet->boot_reason, "sw-reset");   break;
    case ESP_RST_PANIC:     strcpy(_fleet->boot_reason, "panic");      break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       strcpy(_fleet->boot_reason, "watchdog");   break;
    case ESP_RST_BROWNOUT:  strcpy(_fleet->boot_reason, "brownout");   break;
    case ESP_RST_DEEPSLEEP: strcpy(_fleet->boot_reason, "sleep-wake"); break;
    case ESP_RST_EXT:       strcpy(_fleet->boot_reason, "ext-reset");  break;
    default:                strcpy(_fleet->boot_reason, "other");      break;
  }
#elif defined(NRF52_PLATFORM)
  // fleetBegin() runs before the BLE interface starts, so the SoftDevice is
  // normally still off and the register is directly readable -- but check, in
  // case the init order ever changes (direct access faults with the SD live).
  uint32_t reas = 0;
  uint8_t sd_en = 0;
  sd_softdevice_is_enabled(&sd_en);
  if (sd_en) { sd_power_reset_reason_get(&reas); sd_power_reset_reason_clr(reas); }
  else       { reas = NRF_POWER->RESETREAS; NRF_POWER->RESETREAS = reas; }
  // A plain power cycle (and nRF52 brownout) leaves no bits set. LPCOMP is the
  // powerOffUntilCharged() dawn wake -- a solar node that slept and recovered.
  if      (reas == 0)                           strcpy(_fleet->boot_reason, "power-on");
  else if (reas & POWER_RESETREAS_DOG_Msk)      strcpy(_fleet->boot_reason, "watchdog");
  else if (reas & POWER_RESETREAS_LOCKUP_Msk)   strcpy(_fleet->boot_reason, "lockup");
  else if (reas & POWER_RESETREAS_SREQ_Msk)     strcpy(_fleet->boot_reason, "sw-reset");
  else if (reas & POWER_RESETREAS_LPCOMP_Msk)   strcpy(_fleet->boot_reason, "wake-charged");
  else if (reas & POWER_RESETREAS_OFF_Msk)      strcpy(_fleet->boot_reason, "gpio-wake");
  else if (reas & POWER_RESETREAS_RESETPIN_Msk) strcpy(_fleet->boot_reason, "reset-pin");
  else                                          strcpy(_fleet->boot_reason, "other");
#else
  strcpy(_fleet->boot_reason, "unknown");
#endif
  _prefs.boot_count++;   // persisted: "rebooted once" vs "brownout-looping"
  savePrefs();

#if defined(ESP32)
  // Hardware watchdog: panic-reboot if loop() stalls for FLEET_WDT_TIMEOUT_S.
#if ESP_IDF_VERSION_MAJOR >= 5
  // IDF5 (Arduino core 3.x, e.g. ESP32-C6): config-struct API, and the core
  // has usually already init'ed the TWDT -- reconfigure instead of init.
  {
    esp_task_wdt_config_t wdt_cfg = {};
    wdt_cfg.timeout_ms = FLEET_WDT_TIMEOUT_S * 1000;
    wdt_cfg.idle_core_mask = 0;
    wdt_cfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt_cfg) != ESP_OK) esp_task_wdt_init(&wdt_cfg);
  }
#else
  esp_task_wdt_init(FLEET_WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
  _fleet->wdt_on = true;
#elif defined(NRF52_PLATFORM)
  // nRF52 hardware watchdog. SLEEP_Pause makes it count only while the CPU is
  // awake, so the idle wake-on-packet loop can't trip it, while a genuinely
  // hung awake loop still resets the chip after FLEET_WDT_TIMEOUT_S.
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos)
                  | (WDT_CONFIG_SLEEP_Pause << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV  = (FLEET_WDT_TIMEOUT_S * 32768) - 1; // 32.768 kHz LFCLK
  NRF_WDT->RREN = (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos);
  NRF_WDT->TASKS_START = 1;
  _fleet->wdt_on = true;
#endif
}

void MyMesh::fleetLoop() {
  if (!_fleet) return;

#if defined(ESP32)
  if (_fleet->wdt_on) esp_task_wdt_reset(); // feed the dog
#elif defined(NRF52_PLATFORM)
  if (_fleet->wdt_on) NRF_WDT->RR[0] = WDT_RR_RR_Reload; // feed the dog
#endif

  // Low-battery auto power-off, debounced so a single noisy dip can't trip it.
#if FLEET_LOW_BATT_MV > 0
  uint16_t mv = board.getBattMilliVolts();
  // Only act on a *plausible* low battery. A reading below the floor means no
  // battery / USB power, where powerOff() (deep sleep) would be catastrophic.
  if (mv > FLEET_LOW_BATT_FLOOR && mv < FLEET_LOW_BATT_MV) {
    if (++_fleet->low_batt_strikes >= FLEET_LOW_BATT_STRIKES) {
#if FLEET_LOWBATT_BEACON
      // Broadcast a final "going dark" message so the mesh knows this was a
      // graceful low-battery shutdown (vs. a crash/theft/dead panel). Blocks
      // until the packet actually transmits, then sleeps.
      fleetLowBattBeacon(mv);
#endif
      // Solar/unattended: shut down with a self-recovery wake (LPCOMP voltage
      // wake on nRF52) so the node revives when the panel recharges at dawn.
      // Falls back to powerOff() on boards without a voltage-wake path.
      board.powerOffUntilCharged(); // does not return on supported boards
    }
  } else {
    _fleet->low_batt_strikes = 0;
  }
#endif

  // Deferred `@name reboot` (control channel): armed with a delay so the
  // "rebooting" reply gets on the air before the node drops.
  if (_fleet->reboot_at_ms != 0 && millisHasNowPassed(_fleet->reboot_at_ms)) {
    board.reboot();  // does not return
  }

  // Periodic zero-hop location advert so the mesh tracks a moving node.
  // Cadence is runtime-adjustable (`@name set advert_interval <s>`, 0 = off);
  // a reboot restores the build default (FLEET_ADVERT_INTERVAL_S).
  if (_fleet->advert_interval_s > 0 && millisHasNowPassed(_fleet->next_advert_ms)) {
    advert();
    _fleet->next_advert_ms = futureMillis(_fleet->advert_interval_s * 1000UL);
  }
}

#if FLEET_LOWBATT_BEACON
// Broadcast a one-shot "going dark" message on the control channel right before
// a low-battery shutdown, then BLOCK until it actually transmits.
//
// LoRa TX is async: sendGroupMessage() only queues a packet -- the radio sends
// it later from the dispatcher loop. Since the caller is about to SYSTEMOFF (and
// never return), we must pump the radio here until the queue drains, or the
// beacon would die unsent. We call BaseChatMesh::loop() directly (not our own
// loop()) so we service the radio without re-entering the low-batt check, and we
// feed the watchdog ourselves since fleetLoop() is bypassed.
void MyMesh::fleetLowBattBeacon(uint16_t mv) {
  if (!_fleet) return;
  if (_prefs.bot_control_channel == 0xFF) return;    // no control channel -> nowhere to send

  ChannelDetails d;
  if (!getChannel(_prefs.bot_control_channel, d) || !d.name[0]) return; // channel gone

  char text[100];
  uint32_t up = _ms->getMillis() / 1000;
  snprintf(text, sizeof(text), "%s going dark batt:%dmV(%d%%) up:%luh",
           _prefs.node_name, (int)mv, (int)fleetLipoPercent(mv),
           (unsigned long)(up / 3600));

  // A channel message is a flood send; the dispatcher bumps its sent-flood
  // counter only when a packet finishes transmitting. Snapshot it, send, then
  // pump the radio until it ticks (TX completed) or we time out.
  uint32_t sent_before = getNumSentFlood();
  if (!sendGroupMessage(getRTCClock()->getCurrentTimeUnique(), d.channel,
                        _prefs.node_name, text, strlen(text))) return;

  uint32_t start = _ms->getMillis();
  while (getNumSentFlood() == sent_before
         && (_ms->getMillis() - start) < FLEET_LOWBATT_BEACON_DRAIN_MS) {
    BaseChatMesh::loop();
#if defined(ESP32)
    if (_fleet->wdt_on) esp_task_wdt_reset();
#elif defined(NRF52_PLATFORM)
    if (_fleet->wdt_on) NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif
  }
}
#endif // FLEET_LOWBATT_BEACON

// --- meshcore-cli custom vars (CMD_GET/SET_CUSTOM_VARS) -----------------------

// Resolve a channel argument to a channel index. Accepts: "off"/"none" (0xFF),
// a numeric index, "<name>,<base64psk>" (joins a private channel into a free
// slot; 16- or 32-byte key), an already-joined channel name (leading '#'
// optional), or "#name" (auto-joins the hashtag channel; key = first 16 bytes
// of sha256("#name")). Returns the index, 0xFF for off, or -1 on failure.
// Used by `set bot_control_channel`.
int MyMesh::fleetResolveChannelArg(const char* value) {
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
  // (random-key) channel into a free slot. Comma-separated so meshcli passes it
  // as a single token (like `set radio f,bw,sf,cr`).
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

// `set bot_enable 0|1`, `set bot_control_channel <idx|off|name|#tag|name,b64psk>`.
// Returns true if handled.
bool MyMesh::fleetSetVar(const char* name, const char* value) {
  if (!_fleet) return false;

  if (strcmp(name, "bot_enable") == 0) {
    _prefs.bot_enabled = (value[0] == '1') ? 1 : 0;
    savePrefs();
    return true;
  }
  if (strcmp(name, "bot_control_channel") == 0) {
    int idx = fleetResolveChannelArg(value);
    if (idx < 0) return false;
    _prefs.bot_control_channel = (uint8_t)idx;
    savePrefs();
    return true;
  }
  return false;
}

// Format one "name:<channel>" var into buf+n: the channel's name if joined,
// its index otherwise, or "off" for the 0xFF sentinel.
int MyMesh::fleetFormatChannelVar(char* buf, int n, size_t bufsz, const char* name, uint8_t ch) {
  if (n < 0 || n >= (int)bufsz - 1) return n;   // buffer already full
  if (ch == 0xFF) return n + snprintf(buf + n, bufsz - n, "%s:off", name);
#ifdef MAX_GROUP_CHANNELS
  ChannelDetails d;
  if (getChannel(ch, d) && d.name[0]) return n + snprintf(buf + n, bufsz - n, "%s:%s", name, d.name);
#endif
  return n + snprintf(buf + n, bufsz - n, "%s:%d", name, ch);
}

// Append ONE "name:value" var into the custom-vars reply frame if the whole
// thing fits in the remaining space -- otherwise skip it and keep going.
// Returns the advanced cursor.
static char* appendVarKV(char* dp, const char* end, bool* first, const char* kv) {
  int sep = *first ? 0 : 1;                     // leading ',' between vars
  int len = (int)strlen(kv);
  if (dp + sep + len > end) return dp;          // doesn't fit -> skip, try the next
  if (sep) *dp++ = ',';
  memcpy(dp, kv, len);
  *first = false;
  return dp + len;
}

// Compact one-line human-readable sensor summary for `@name sensors`. Same
// source as req_telemetry: fill the Cayenne-LPP buffer via querySensors(), then
// walk it back out to typed values and emit "k:v" pairs (temp/hum/press/voltage/
// gps as available). "no sensors" when nothing but the self voltage is present.
int MyMesh::fleetFormatSensorsText(char* out, size_t sz) {
  telemetry.reset();
  telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
  sensors.querySensors(0xFF, telemetry);   // 0xFF = all permitted, local read

  LPPReader r(telemetry.getBuffer(), telemetry.getSize());
  int n = 0, count = 0;
  uint8_t ch, type;
  auto kv = [&](const char* key, double val, const char* unit) {
    if (n < (int)sz - 24)
      n += snprintf(out + n, sz - n, "%s%s:%.1f%s", count ? " " : "", key, val, unit);
    count++;
  };
  while (r.readHeader(ch, type)) {
    float a, b, c;
    switch (type) {
      case LPP_TEMPERATURE:        if (r.readTemperature(a))      kv("temp", a, "C"); break;
      case LPP_RELATIVE_HUMIDITY:  if (r.readRelativeHumidity(a)) kv("hum", a, "%"); break;
      case LPP_BAROMETRIC_PRESSURE:if (r.readPressure(a))         kv("press", a, "hPa"); break;
      case LPP_VOLTAGE:            if (r.readVoltage(a))          kv("v", a, "V"); break;
      case LPP_GPS:                if (r.readGPS(a, b, c)) {      // keep lat/lon together
        if (n < (int)sz - 40) n += snprintf(out + n, sz - n, "%sgps:%.5f,%.5f", count ? " " : "", a, b);
        count++; } break;
      default: r.skipData(type); break;   // unknown/unhandled -> advance past it
    }
  }
  if (count == 0) n = snprintf(out, sz, "no sensors");
  return n;
}

char* MyMesh::fleetAppendVars(char* base, char* dp, const char* end) {
  if (!_fleet) return dp;
  bool first = (dp == base);                    // no leading ',' if we're first
  char kv[80];

  snprintf(kv, sizeof(kv), "bot_enable:%d", _prefs.bot_enabled ? 1 : 0);
  dp = appendVarKV(dp, end, &first, kv);

  int m = fleetFormatChannelVar(kv, 0, sizeof(kv), "bot_control_channel", _prefs.bot_control_channel);
  if (m > 0) dp = appendVarKV(dp, end, &first, kv);

  return dp;
}

#endif // WITH_FLEET_EXTRAS
