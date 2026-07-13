// combined_node — companion + relay + extended telemetry / bot commands
//
// This file is a compile-time EXTENSION of examples/companion_radio. It adds
// out-of-line definitions for the bot-command methods that companion_radio's
// MyMesh declares (and calls) behind `#ifdef WITH_BOT_COMMANDS`. It is only
// compiled by the `*_combined_node_*` build environments, which also compile
// the unmodified companion_radio sources. Nothing here forks companion_radio:
// pull upstream companion_radio changes and they flow straight through.

#include "MyMesh.h"

#ifdef WITH_BOT_COMMANDS

#include <Arduino.h>
#include <string.h>
#ifdef WITH_COMBINED_EXTRAS
#include "CombinedNode.h"   // bot rate-limiter + stats/neighbour helpers
// WiFi status for `!wifi`: strong definition in main.cpp on _wifi builds,
// weak "no WiFi here" fallback in CombinedNode.cpp everywhere else.
extern "C" bool combinedWifiStatus(char* buf, size_t bufsz);
// `!path` map link via the mesh-observer device API (same weak/strong split).
extern "C" bool combinedPathShortUrl(const char* hashes, const char* origin,
                                     const char* requester_pos, const char* reporter,
                                     const char* requester, char* out, size_t outsz);
#endif
#if defined(NRF52_PLATFORM)
#include <utility/debug.h>  // dbgHeapFree() from the Adafruit nRF52 core
#endif

// Runtime malloc headroom in bytes, or -1 if unknown on this platform. On
// nRF52 this is the linker heap region minus mallinfo().uordblks (the real
// space new/malloc can still claim before colliding with the stack); on ESP32
// it's ESP.getFreeHeap(). Surfaced via !telemetry so we can watch margin under
// load instead of guessing from the static link report.
static int combinedFreeHeap() {
#if defined(ESP32)
  return (int)ESP.getFreeHeap();
#elif defined(NRF52_PLATFORM)
  return dbgHeapFree();
#else
  return -1;
#endif
}

// Match a command word at the start of `s` (case-sensitive, like CommonCLI).
// Returns true when `s` begins with `word`.
static bool cmdIs(const char* s, const char* word) {
  size_t n = strlen(word);
  return strncmp(s, word, n) == 0;
}

// Build the reply text for a recognised command (`cmd` is the text after the
// prefix). Returns true if a command matched and `reply` was filled; false for
// unrecognised commands -- in which case we stay silent (no "unknown" reply).
bool MyMesh::buildBotReply(const char* cmd, mesh::Packet* pkt,
                           uint32_t sender_timestamp, const char* sender_name,
                           bool is_ctl, char* reply, size_t sz) {
  reply[0] = 0;

  // Control commands (!relay/!ble/!wd writes) are ONLY authorized on the
  // bot_control_channel (is_ctl): possession of that private channel's key is
  // the auth, and it works at any hop count so a single channel message can
  // control the whole fleet. DMs never reach here at all -- bot commands in
  // direct messages are ignored entirely (the message still lands in the
  // companion app as normal chat). Deliberately no other auth.
  bool write_ok = is_ctl;

  if (cmdIs(cmd, "ping")) {
    // Report how THIS node heard the request: link quality, hop count, and a
    // rough one-way latency (sender clock vs ours; depends on RTC sync).
    float snr = pkt ? pkt->getSNR() : 0.0f;
    uint8_t hops = pkt ? pkt->getPathHashCount() : 0;
    bool flood = pkt && pkt->isRouteFlood();
    long lat = (long)getRTCClock()->getCurrentTime() - (long)sender_timestamp;
    if (lat < 0) lat = 0;
#ifdef WITH_COMBINED_EXTRAS
    int rssi = (int)(_combined ? _combined->last_rssi : 0);
    if (flood) snprintf(reply, sz, "pong snr:%.1fdB rssi:%d hops:%u lat~%lds", (double)snr, rssi, (unsigned)hops, lat);
    else       snprintf(reply, sz, "pong snr:%.1fdB rssi:%d direct lat~%lds", (double)snr, rssi, lat);
#else
    if (flood) snprintf(reply, sz, "pong snr:%.1fdB hops:%u lat~%lds", (double)snr, (unsigned)hops, lat);
    else       snprintf(reply, sz, "pong snr:%.1fdB direct lat~%lds", (double)snr, lat);
#endif

  } else if (cmdIs(cmd, "path")) {
    // Report the complete route this request took to reach us, formatted as
    //   <name> [<hops>h] <hash>,<hash>,...
    // where <name> is the requester, <hops> is how many repeaters relayed it,
    // and each <hash> is one path-hash entry (getPathHashSize() bytes -- the
    // leading bytes of that repeater's public-key hash) in traversal order,
    // always as raw hex so hops can be matched against key prefixes directly.
    // A direct (0-hop) packet was heard straight from the sender, no relay.
    const char* who = (sender_name && sender_name[0]) ? sender_name : "?";
    uint8_t hops = pkt ? pkt->getPathHashCount() : 0;
    uint8_t hsz  = pkt ? pkt->getPathHashSize() : 1;
    char csv[144];   // hop list alone, reused as the map-API payload
    int cn = 0;
    if (hops == 0) {
      cn = snprintf(csv, sizeof(csv), "direct");
    } else {
      const uint8_t* p = pkt->path;
      for (uint8_t i = 0; i < hops && cn < (int)sizeof(csv) - 1; i++) {
        if (i > 0) cn += snprintf(csv + cn, sizeof(csv) - cn, ",");
        for (uint8_t b = 0; b < hsz && cn < (int)sizeof(csv) - 1; b++)
          cn += snprintf(csv + cn, sizeof(csv) - cn, "%02x", p[b]);
        p += hsz;
      }
    }
    int n = snprintf(reply, sz, "%s [%uh] %s", who, (unsigned)hops, csv);
#ifdef WITH_COMBINED_EXTRAS
    // On WiFi builds with `set obs_url` configured, trade the hop chain for a
    // short map URL (mesh-observer resolves the hashes to coordinates and
    // draws the route). Appended only if the whole link fits; any failure
    // leaves the plain hex reply untouched.
    if (hops > 0 && n < (int)sz - 24) {
      char origin[32] = "";
      if (sensors.node_lat != 0.0 || sensors.node_lon != 0.0)
        snprintf(origin, sizeof(origin), "%.5f,%.5f", sensors.node_lat, sensors.node_lon);
      // Wardrive beacons are "!path <lat,lon>" -- the argument is the
      // surveyor's position, forwarded so the map can anchor both ends.
      char qpos[32] = "";
      double qla, qlo;
      if (sscanf(cmd + 4, " %lf,%lf", &qla, &qlo) == 2 &&
          qla >= -90.0 && qla <= 90.0 && qlo >= -180.0 && qlo <= 180.0)
        snprintf(qpos, sizeof(qpos), "%.5f,%.5f", qla, qlo);
      char url[96];
      if (combinedPathShortUrl(csv, origin, qpos, _prefs.node_name, who, url, sizeof(url)) &&
          n + 1 + (int)strlen(url) < (int)sz)
        n += snprintf(reply + n, sz - n, " %s", url);
    }
#endif

#ifdef WITH_COMBINED_EXTRAS
  } else if (cmdIs(cmd, "stats")) {
    combinedFormatStats(reply, sz);

  } else if (cmdIs(cmd, "neighbors") || cmdIs(cmd, "neighbours")) {
    combinedFormatNeighbours(reply, sz);

  } else if (cmdIs(cmd, "heard")) {
    // When did THIS node last directly hear one station (name or hex prefix)?
    const char* arg = cmd + 5;
    while (*arg == ' ') arg++;
    if (*arg == 0) snprintf(reply, sz, "heard: want <name|hex prefix>");
    else           combinedFormatHeard(arg, reply, sz);

  } else if (cmdIs(cmd, "batt") && _combined) {
    // Battery with direction, not just a snapshot: min/max since boot and the
    // change over the last hour -- "charging fine" vs "dying by Thursday".
    uint16_t bmv = board.getBattMilliVolts();
    char trend[16] = "n/a";
    if (_combined->mv_1h_ago > 0)
      snprintf(trend, sizeof(trend), "%+dmV", (int)bmv - (int)_combined->mv_1h_ago);
    snprintf(reply, sz, "batt:%umV(%u%%) min:%u max:%u 1h:%s",
             (unsigned)bmv, (unsigned)combinedLipoPercent(bmv),
             (unsigned)_combined->mv_min, (unsigned)_combined->mv_max, trend);

  } else if (cmdIs(cmd, "boot") && _combined) {
    // Reset cause + persisted boot counter, readable without unsealing the node.
    snprintf(reply, sz, "boot #%lu: %s",
             (unsigned long)_prefs.boot_count, _combined->boot_reason);

  } else if (cmdIs(cmd, "wifi")) {
    // Did the node rejoin WiFi after the power blip? Read-only status;
    // toggling stays with `@name set wifi on|off`.
    char st[48];
    if (combinedWifiStatus(st, sizeof(st))) snprintf(reply, sz, "wifi: %s", st);
    else                                    snprintf(reply, sz, "wifi: n/a on this build");
#endif

  } else if (cmdIs(cmd, "info")) {
    snprintf(reply, sz, "%s fw %s relay:%s",
             _prefs.node_name, FIRMWARE_VERSION, _prefs.client_repeat ? "on" : "off");

  } else if (cmdIs(cmd, "uptime")) {
    uint32_t s = _ms->getMillis() / 1000;
    snprintf(reply, sz, "up %lud %luh %lum %lus",
             (unsigned long)(s / 86400), (unsigned long)((s / 3600) % 24),
             (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));

  } else if (cmdIs(cmd, "telemetry")) {
    uint32_t s = _ms->getMillis() / 1000;
    uint16_t mv = board.getBattMilliVolts();
    char hbuf[16] = "";
    int heap = combinedFreeHeap();
    if (heap >= 0) snprintf(hbuf, sizeof(hbuf), " heap:%dB", heap);
    snprintf(reply, sz, "batt:%dmV(%d%%) up:%lus relayed:%lu sensors:%d%s",
             (int)mv, (int)combinedLipoPercent(mv), (unsigned long)s,
             (unsigned long)_relay_count, (int)sensors.getNumSettings(), hbuf);

  } else if (cmdIs(cmd, "rf")) {
    // Radio params in one line, for a fleet-wide config audit: catches the one
    // node still on default txpower or the wrong preset.
    snprintf(reply, sz, "rf: %.3fMHz sf%u bw%.0fkHz cr%u tx%ddBm",
             (double)_prefs.freq, (unsigned)_prefs.sf, (double)_prefs.bw,
             (unsigned)_prefs.cr, (int)_prefs.tx_power_dbm);

  } else if (cmdIs(cmd, "time")) {
    // RTC health. Drift compares our clock to the sender's message timestamp,
    // so it includes the packet's flight time -- seconds-level truth only.
    // `!time sync` (control channel) adopts the sender's timestamp, either
    // direction, to pull a wandered RTC back in line.
    const char* arg = cmd + 4;
    while (*arg == ' ') arg++;
    uint32_t now = getRTCClock()->getCurrentTime();
    long drift = (long)now - (long)sender_timestamp;
    if (cmdIs(arg, "sync")) {
      if (!write_ok) {
        snprintf(reply, sz, "time: control-channel only");
      } else {
        getRTCClock()->setCurrentTime(sender_timestamp);
        snprintf(reply, sz, "time synced (was %+lds)", drift);
      }
    } else {
      snprintf(reply, sz, "time:%lu drift:%+lds", (unsigned long)now, drift);
    }

  } else if (cmdIs(cmd, "advert")) {
    // Control command: re-announce this node NOW with a flood advert -- after
    // relocating a node, rotating identity, or onboarding new clients, instead
    // of waiting out the periodic advert timer.
    if (!write_ok) {
      snprintf(reply, sz, "advert: control-channel only");
    } else {
      mesh::Packet* pkt = (_prefs.advert_loc_policy == ADVERT_LOC_NONE)
          ? createSelfAdvert(_prefs.node_name)
          : createSelfAdvert(_prefs.node_name, sensors.node_lat, sensors.node_lon);
      if (pkt) {
        TransportKey scope;
        memcpy(&scope.key, _prefs.default_scope_key, sizeof(scope.key));
        sendFloodScoped(scope, pkt, 0);
        snprintf(reply, sz, "advert sent");
      } else {
        snprintf(reply, sz, "advert: failed");
      }
    }

#ifdef WITH_COMBINED_EXTRAS
  } else if (cmdIs(cmd, "wd") && _combined) {
    // Wardrive mode: while on, the node beacons "!path <lat,lon>" to the
    // control channel every COMBINED_WD_INTERVAL_S. Every fleet node that
    // hears it answers with the route it arrived by (the !path handler), so
    // driving around with this node maps mesh coverage, each reply tagged
    // with the position beaconed. Runtime-only (never persisted): a reboot
    // always returns to quiet, so a forgotten survey can't drain a node.
    const char* arg = cmd + 2;
    while (*arg == ' ') arg++;
    if (*arg == 0) {
      snprintf(reply, sz, "wd %s", _combined->wd_on ? "on" : "off");
    } else if (!write_ok) {
      snprintf(reply, sz, "wd: control-channel only");
    } else if (cmdIs(arg, "on")) {
      if (_prefs.bot_control_channel == 0xFF) {
        snprintf(reply, sz, "wd: control channel not set");
      } else {
        _combined->wd_on = true;
        _combined->next_wd_ms = 0;   // first beacon immediately
        snprintf(reply, sz, "wd on (beacon !path to control every %ds)", COMBINED_WD_INTERVAL_S);
      }
    } else if (cmdIs(arg, "off")) {
      _combined->wd_on = false;
      snprintf(reply, sz, "wd off");
    } else {
      snprintf(reply, sz, "wd %s", _combined->wd_on ? "on" : "off");
    }
#endif

  } else if (cmdIs(cmd, "loc")) {
    // Node location -- sensitive for hidden field nodes, so this ONLY answers
    // on the control channel (possession of that key is the authorization).
    // Anywhere else we stay silent, as if the command doesn't exist; it is
    // also deliberately absent from !help.
    if (!is_ctl) return false;
    if (sensors.node_lat == 0.0 && sensors.node_lon == 0.0) {
      snprintf(reply, sz, "loc: not set");
    } else {
      snprintf(reply, sz, "loc: %.6f,%.6f%s", sensors.node_lat, sensors.node_lon,
               _prefs.gps_enabled ? " (gps)" : "");
    }

  } else if (cmdIs(cmd, "help")) {
    // Control channel ONLY: on the public bot channel we stay silent, so the
    // command list isn't advertised to anyone who stumbles onto the bot. And
    // since only key-holders can see it, it may list everything -- write
    // commands and `!loc` included.
    if (!is_ctl) return false;
#ifdef WITH_COMBINED_EXTRAS
    snprintf(reply, sz, "cmds: ping path info uptime telemetry stats neighbors heard batt boot rf time wifi loc advert wd relay ble help; @name set|reboot");
#else
    snprintf(reply, sz, "cmds: ping path info uptime telemetry rf time advert relay help");
#endif

#if defined(BLE_PIN_CODE)
  } else if (cmdIs(cmd, "ble")) {
    // Control command: toggle BLE advertising to save power on unattended nodes.
    // The reply rides the LoRa mesh, not BLE, so it still arrives after `off`.
    // Persisted in NodePrefs and re-applied at boot (see startInterface).
    const char* arg = cmd + 3;            // skip "ble"
    while (*arg == ' ') arg++;
    if (*arg == 0) {                      // bare `!ble` -> status, readable from anywhere
      snprintf(reply, sz, "BLE %s", _serial->isEnabled() ? "on" : "off");
    } else if (!write_ok) {               // write requires the control channel
      snprintf(reply, sz, "BLE: control-channel only");
    } else if (cmdIs(arg, "on")) {
      _prefs.ble_enabled = 1; savePrefs(); _serial->enable();
      snprintf(reply, sz, "BLE on");
    } else if (cmdIs(arg, "off")) {
      _prefs.ble_enabled = 0; savePrefs(); _serial->disable();
      snprintf(reply, sz, "BLE off");
    } else {
      snprintf(reply, sz, "BLE %s", _serial->isEnabled() ? "on" : "off");
    }
#endif

  } else if (cmdIs(cmd, "relay")) {
    // Control command: enable/disable packet forwarding (client_repeat) at
    // runtime. allowPacketForward() reads _prefs.client_repeat live per packet
    // (MyMesh.cpp), so the change takes effect immediately -- no reboot -- and
    // savePrefs() persists it across boots (NodePrefs offset 62).
    const char* arg = cmd + 5;            // skip "relay"
    while (*arg == ' ') arg++;
    if (*arg == 0) {                      // bare `!relay` -> status, readable from anywhere
      snprintf(reply, sz, "relay %s", _prefs.client_repeat ? "on" : "off");
    } else if (!write_ok) {               // write requires the control channel
      snprintf(reply, sz, "relay: control-channel only");
    } else if (cmdIs(arg, "on")) {
      _prefs.client_repeat = 1; savePrefs();
      snprintf(reply, sz, "relay on");
    } else if (cmdIs(arg, "off")) {
      _prefs.client_repeat = 0; savePrefs();
      snprintf(reply, sz, "relay off");
    } else {
      snprintf(reply, sz, "relay %s", _prefs.client_repeat ? "on" : "off");
    }

  } else {
    return false; // unrecognised command -> stay silent
  }
  return reply[0] != 0;
}

// NOTE: there is deliberately NO direct-message bot handler. Bot commands are
// channel-only; a DM starting with '!' is just a chat message to the app.

#ifdef WITH_COMBINED_EXTRAS
// Incoming channel (group) bot handler. Only answers on the configured
// bot_channel, and replies back to that same channel.
void MyMesh::handleBotChannel(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                              uint32_t timestamp, const char* text) {
  // Channel messages arrive wire-formatted as "<sender_name>: <message>", so
  // the command is NOT at text[0]. Skip past the "name: " prefix, and capture
  // the sender name so we can tag them in the reply.
  const char* msg = text;
  char sender[32] = {0};
  if (text) {
    const char* sep = strstr(text, ": ");
    if (sep) {
      msg = sep + 2;
      int nlen = sep - text;
      if (nlen > (int)sizeof(sender) - 1) nlen = sizeof(sender) - 1;
      memcpy(sender, text, nlen);
      sender[nlen] = 0;
    }
  }
#ifdef BOT_DEBUG
  Serial.printf("[bot] CHAN rx: text='%s' msg='%s' rxidx=%d botchan=%d en=%d\n",
                text ? text : "(null)", msg ? msg : "(null)", findChannelIdx(channel),
                _combined ? _prefs.bot_channel : -1,
                _combined ? _prefs.bot_enabled : -1);
#endif
  if (!_combined || !_prefs.bot_enabled) return;
  // '!' = bot command; '@' = targeted admin ("@<name> set ..."), which the
  // control channel handles below. (The '@' case was previously dropped HERE
  // by the '!'-only check, making handleTargetedSet unreachable -- @name
  // commands never worked over the air until this fix.)
  if (msg == NULL || (msg[0] != BOT_CMD_PREFIX && msg[0] != '@')) return;
  // The bot answers on every channel in bot_channel_mask (multi-channel:
  // `set bot_channel +name` adds one), on bot_path_mask channels (but ONLY
  // to `!path` there -- e.g. a public #bot where the full command surface
  // shouldn't be exposed), AND on the control channel. A message on the
  // control channel is write-authorized (is_ctl): possession of that
  // (private) channel's key is the auth, so one channel message can toggle
  // every listening node -- regardless of hop count.
  int rxidx = findChannelIdx(channel);
  bool on_bot  = rxidx >= 0 && rxidx < 64 && ((_prefs.bot_channel_mask >> rxidx) & 1);
  bool on_path = rxidx >= 0 && rxidx < 64 && ((_prefs.bot_path_mask >> rxidx) & 1);
  bool is_ctl = _prefs.bot_control_channel != 0xFF && rxidx == (int)_prefs.bot_control_channel;
  if (!on_bot && !is_ctl && !on_path) return;                        // not a channel we answer on
  if (!on_bot && !is_ctl) {                                          // path-only channel (public #bot):
    if (!cmdIs(msg + 1, "path")) return;                             // only !path answers there, and a
    if (pkt == NULL || pkt->getPathHashCount() == 0) return;         // 0-hop request maps nothing -- stay silent
  }
  if (is_ctl && msg && msg[0] == '@') {
    // "@<node name> set <var> <value>" -- node-targeted admin, control channel
    // only. Only the named node acts (and rate-limits); everyone else ignores.
    handleTargetedSet(channel, sender, msg + 1);
    return;
  }
  if (!_combined->bot_limiter.allow((uint32_t)(_ms->getMillis() / 1000))) return; // throttled
  char reply[160];
  if (buildBotReply(msg + 1, pkt, timestamp, sender, is_ctl, reply, sizeof(reply))) {
    // Tag the requester so they know the reply is for them on a busy channel.
    char tagged[200];
    // Tag the requester with @name so they spot the reply on a busy channel --
    // unless the reply already leads with their name (e.g. !path), which would
    // otherwise read "@zanchez zanchez ...".
    if (sender[0] && strncmp(reply, sender, strlen(sender)) != 0)
      snprintf(tagged, sizeof(tagged), "@%s %s", sender, reply);
    else
      snprintf(tagged, sizeof(tagged), "%s", reply);
#ifdef BOT_DEBUG
    Serial.printf("[bot] CHAN reply -> '%s'\n", tagged);
#endif
    mesh::GroupChannel ch = channel;  // sendGroupMessage wants a non-const ref
    sendGroupMessage(getRTCClock()->getCurrentTimeUnique(), ch, _prefs.node_name, tagged, strlen(tagged));
  }
}
#endif

#ifdef WITH_COMBINED_EXTRAS
// Runtime WiFi enable/disable hook. WiFi builds (main.cpp, #ifdef WIFI_SSID)
// provide the strong definition; this weak fallback reports "unsupported" on
// BLE/USB builds so `set wifi` degrades gracefully.
extern "C" bool combinedSetWifiControl(bool enable);
extern "C" __attribute__((weak)) bool combinedSetWifiControl(bool) { return false; }

// "@<node name> set <var> <value>" (or "@<node name> reboot") on the control
// channel: per-node admin, vs the bare fleet-wide !relay/!ble. Name match is
// case-insensitive and supports spaces in names (everything before the verb
// is the target). A node that isn't the target stays silent.
void MyMesh::handleTargetedSet(const mesh::GroupChannel& channel, const char* sender, const char* text) {
  size_t nlen = strlen(_prefs.node_name);
  if (nlen == 0 || strncasecmp(text, _prefs.node_name, nlen) != 0) return; // not us
  const char* p = text + nlen;
  while (*p == ' ') p++;
  bool is_reboot = strncmp(p, "reboot", 6) == 0;
  if (!is_reboot && strncmp(p, "set ", 4) != 0) return;   // verbs: `set`, `reboot`
  if (!is_reboot) { p += 4; while (*p == ' ') p++; }

  if (!_combined->bot_limiter.allow((uint32_t)(_ms->getMillis() / 1000))) return;

  char reply[80];
  reply[0] = 0;

  if (is_reboot) {
    // The remote unstick for a sealed node. Deferred (combinedLoop pulls the
    // trigger) so this reply gets on the air before the node drops.
    _combined->reboot_at_ms = futureMillis(5000);
    snprintf(reply, sizeof(reply), "rebooting in 5s");

  } else if (strncmp(p, "relay ", 6) == 0) {
    const char* v = p + 6;
    if (cmdIs(v, "on"))       { _prefs.client_repeat = 1; savePrefs(); snprintf(reply, sizeof(reply), "relay on"); }
    else if (cmdIs(v, "off")) { _prefs.client_repeat = 0; savePrefs(); snprintf(reply, sizeof(reply), "relay off"); }

  } else if (strncmp(p, "ble ", 4) == 0) {
#if defined(BLE_PIN_CODE)
    const char* v = p + 4;
    if (cmdIs(v, "on"))       { _prefs.ble_enabled = 1; savePrefs(); _serial->enable();  snprintf(reply, sizeof(reply), "ble on"); }
    else if (cmdIs(v, "off")) { _prefs.ble_enabled = 0; savePrefs(); _serial->disable(); snprintf(reply, sizeof(reply), "ble off"); }
#else
    snprintf(reply, sizeof(reply), "ble: n/a on this build");
#endif

  } else if (strncmp(p, "txpower ", 8) == 0) {
    int power = atoi(p + 8);
    if (power < -9 || power > MAX_LORA_TX_POWER) {
      snprintf(reply, sizeof(reply), "txpower: out of range (max %d)", MAX_LORA_TX_POWER);
    } else {
      _prefs.tx_power_dbm = (int8_t)power;
      savePrefs();
      radio_driver.setTxPower(_prefs.tx_power_dbm);
      snprintf(reply, sizeof(reply), "txpower %d", power);
    }

  } else if (strncmp(p, "location ", 9) == 0) {
    const char* v = p + 9;
    if (cmdIs(v, "off")) {
      sensors.node_lat = 0.0; sensors.node_lon = 0.0;
      savePrefs();
      snprintf(reply, sizeof(reply), "location cleared");
    } else {
      double lat = 0, lon = 0;
      if (sscanf(v, "%lf,%lf", &lat, &lon) == 2 &&
          lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        sensors.node_lat = lat; sensors.node_lon = lon;
        savePrefs();
        snprintf(reply, sizeof(reply), "location %.6f,%.6f", lat, lon);
      } else {
        snprintf(reply, sizeof(reply), "location: want <lat>,<lon> or off");
      }
    }

  } else if (strncmp(p, "wifi ", 5) == 0) {
    const char* v = p + 5;
    bool want = cmdIs(v, "on");
    if (!want && !cmdIs(v, "off")) {
      snprintf(reply, sizeof(reply), "wifi: want on|off");
    } else if (combinedSetWifiControl(want)) {
      // runtime-only: not persisted, a reboot restores the build default (on)
      snprintf(reply, sizeof(reply), "wifi %s (until reboot)", want ? "on" : "off");
    } else {
      snprintf(reply, sizeof(reply), "wifi: n/a on this build");
    }

  } else if (strncmp(p, "gps ", 4) == 0) {
    const char* v = p + 4;
    bool want = cmdIs(v, "on");
    if (!want && !cmdIs(v, "off")) {
      snprintf(reply, sizeof(reply), "gps: want on|off");
    } else if (sensors.setSettingValue("gps", want ? "1" : "0")) {
      // power knob for deployed solar nodes; mirrors `set gps` over meshcli
      _prefs.gps_enabled = want ? 1 : 0;
      savePrefs();
      snprintf(reply, sizeof(reply), "gps %s", want ? "on" : "off");
    } else {
      snprintf(reply, sizeof(reply), "gps: n/a on this build");
    }

  } else if (strncmp(p, "obs_token ", 10) == 0) {
    // Provision the observer device token over the mesh -- no USB cable per
    // node, which matters with a fleet in sealed enclosures. The value may be
    // "label:token" exactly as it appears in the observer's DEVICE_TOKEN
    // secret; the label identifies the node server-side, only the token part
    // rides the X-Device-Token header, so only that is stored. `-` clears.
    // The message transits encrypted with the control channel's key, and the
    // reply confirms LENGTH only -- the token is never echoed back.
    const char* v = p + 10;
    const char* colon = strchr(v, ':');
    const char* tok = colon ? colon + 1 : v;
    if (combinedSetVar("obs_token", tok)) {
      if (_prefs.obs_token[0])
        snprintf(reply, sizeof(reply), "obs_token set (%d chars)", (int)strlen(_prefs.obs_token));
      else
        snprintf(reply, sizeof(reply), "obs_token cleared");
    } else {
      snprintf(reply, sizeof(reply), "obs_token: too long (max %d)", (int)sizeof(_prefs.obs_token) - 1);
    }

  } else if (strncmp(p, "obs_url ", 8) == 0) {
    const char* v = p + 8;
    if (combinedSetVar("obs_url", v))
      snprintf(reply, sizeof(reply), "obs_url %s", _prefs.obs_url[0] ? _prefs.obs_url : "cleared");
    else
      snprintf(reply, sizeof(reply), "obs_url: want http(s)://... (max %d)", (int)sizeof(_prefs.obs_url) - 1);

  } else if (strncmp(p, "advert_interval ", 16) == 0) {
    int s = atoi(p + 16);
    if (s < 0 || s > 86400) {
      snprintf(reply, sizeof(reply), "advert_interval: 0-86400s");
    } else {
      // runtime-only, like `set wifi`: a reboot restores the build default
      _combined->advert_interval_s = (uint32_t)s;
      if (s > 0) _combined->next_advert_ms = futureMillis((uint32_t)s * 1000UL);
      if (s > 0) snprintf(reply, sizeof(reply), "advert_interval %ds (until reboot)", s);
      else       snprintf(reply, sizeof(reply), "advert_interval off (until reboot)");
    }

  } else {
    snprintf(reply, sizeof(reply), "set: relay|ble|txpower|location|wifi|gps|advert_interval|obs_url|obs_token");
  }

  if (reply[0]) {
    char tagged[160];
    snprintf(tagged, sizeof(tagged), "@%s %s: %s", sender[0] ? sender : "?", _prefs.node_name, reply);
    mesh::GroupChannel ch = channel;
    sendGroupMessage(getRTCClock()->getCurrentTimeUnique(), ch, _prefs.node_name, tagged, strlen(tagged));
  }
}
#endif // WITH_COMBINED_EXTRAS

#endif // WITH_BOT_COMMANDS
