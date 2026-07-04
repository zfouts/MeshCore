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
                           bool is_dm, bool is_ctl, char* reply, size_t sz) {
  reply[0] = 0;

  // Control commands (!relay/!ble writes) are authorized two ways:
  //  - a DM heard directly (0 path hashes = straight from the sender's radio),
  //    so someone physically in range messaging the node can change its state;
  //  - any message on the bot_control_channel (is_ctl) -- possession of that
  //    private channel's key is the auth, and it works at any hop count so a
  //    single channel message can control the whole fleet.
  // Deliberately no other auth. Left out of !help.
  bool from_direct = pkt && pkt->getPathHashCount() == 0;
  bool write_ok = is_ctl || (is_dm && from_direct);

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
    //   <name> [#<hops>] <hash>,<hash>,...
    // where <name> is the requester, <hops> is how many repeaters relayed it,
    // and each <hash> is one path-hash entry (getPathHashSize() bytes -- the
    // leading bytes of that repeater's public-key hash) in traversal order.
    // A direct (0-hop) packet was heard straight from the sender, no relay.
    // Each hop is shown as a known name where we recognise the repeater
    // (this node or a contact), else the raw hex hash.
    const char* who = (sender_name && sender_name[0]) ? sender_name : "?";
    uint8_t hops = pkt ? pkt->getPathHashCount() : 0;
    uint8_t hsz  = pkt ? pkt->getPathHashSize() : 1;
    int n = snprintf(reply, sz, "%s [#%u]", who, (unsigned)hops);
    if (hops == 0) {
      n += snprintf(reply + n, sz - n, " direct");
    } else {
      const uint8_t* p = pkt->path;
      for (uint8_t i = 0; i < hops && n < (int)sz - 1; i++) {
        n += snprintf(reply + n, sz - n, i == 0 ? " " : ",");
#ifdef WITH_COMBINED_EXTRAS
        char nm[24];
        if (resolvePathHash(p, hsz, nm, sizeof(nm))) {
          n += snprintf(reply + n, sz - n, "%s", nm);
        } else
#endif
        for (uint8_t b = 0; b < hsz && n < (int)sz - 1; b++)
          n += snprintf(reply + n, sz - n, "%02x", p[b]);
        p += hsz;
      }
    }

#ifdef WITH_COMBINED_EXTRAS
  } else if (cmdIs(cmd, "stats")) {
    combinedFormatStats(reply, sz);

  } else if (cmdIs(cmd, "neighbors") || cmdIs(cmd, "neighbours")) {
    combinedFormatNeighbours(reply, sz);
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

  } else if (cmdIs(cmd, "help")) {
    // List the read-only commands this build compiled. The control commands
    // (!relay/!ble on|off, DM+direct-only) are intentionally NOT listed.
#ifdef WITH_COMBINED_EXTRAS
    snprintf(reply, sz, "cmds: ping path info uptime telemetry stats neighbors help");
#else
    snprintf(reply, sz, "cmds: ping path info uptime telemetry help");
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
    } else if (!write_ok) {               // write requires a direct DM or the control channel
      snprintf(reply, sz, is_dm ? "BLE: direct-only (you are %u hops away)" : "BLE: DM-only",
               (unsigned)(pkt ? pkt->getPathHashCount() : 0));
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
    } else if (!write_ok) {               // write requires a direct DM or the control channel
      snprintf(reply, sz, is_dm ? "relay: direct-only (you are %u hops away)" : "relay: DM-only",
               (unsigned)(pkt ? pkt->getPathHashCount() : 0));
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

// Incoming direct-message bot handler. The message is still delivered to the
// companion app regardless; the auto-reply is additive.
bool MyMesh::handleBotCommand(const ContactInfo& from, mesh::Packet* pkt,
                              uint32_t sender_timestamp, const char* text) {
#ifdef BOT_DEBUG
  Serial.printf("[bot] DM rx: '%s'\n", text ? text : "(null)");
#endif
  if (text == NULL || text[0] != BOT_CMD_PREFIX) return false;
#ifdef WITH_COMBINED_EXTRAS
  if (_combined && !_prefs.bot_enabled) return false;                 // bot disabled
  if (_combined && !_combined->bot_limiter.allow((uint32_t)(_ms->getMillis() / 1000))) return true; // throttled
#endif
  char reply[160];
  if (buildBotReply(text + 1, pkt, sender_timestamp, from.name, true, false, reply, sizeof(reply)))
    sendBotReply(from, reply);
  return true;
}

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
  if (msg == NULL || msg[0] != BOT_CMD_PREFIX) return;
  // The bot answers on the configured bot_channel AND on the control channel.
  // A message on the control channel is write-authorized (is_ctl): possession
  // of that (private) channel's key is the auth, so one channel message can
  // toggle every listening node -- regardless of hop count.
  int rxidx = findChannelIdx(channel);
  bool on_bot = _prefs.bot_channel != 0xFF && rxidx == (int)_prefs.bot_channel;
  bool is_ctl = _prefs.bot_control_channel != 0xFF && rxidx == (int)_prefs.bot_control_channel;
  if (!on_bot && !is_ctl) return;                                    // not a channel we answer on
  if (!_combined->bot_limiter.allow((uint32_t)(_ms->getMillis() / 1000))) return; // throttled
  char reply[160];
  if (buildBotReply(msg + 1, pkt, timestamp, sender, false, is_ctl, reply, sizeof(reply))) {
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

// Send a plain-text reply back to a contact, reusing BaseChatMesh's message
// path (handles flood/direct routing and encryption). When the send expects an
// ACK we record it in the single pending-reply slot so combinedLoop() can resend
// if the reply is lost on a weak link (see COMBINED_BOT_REPLY_RETRIES). Falls
// back to fire-and-forget when extras are compiled out or no ACK is expected.
void MyMesh::sendBotReply(const ContactInfo& to, const char* text) {
  uint32_t expected_ack = 0, est_timeout = 0;
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  sendMessage(to, ts, 0, text, expected_ack, est_timeout);
#ifdef WITH_COMBINED_EXTRAS
  if (_combined && COMBINED_BOT_REPLY_RETRIES > 0 && expected_ack != 0) {
    CombinedPendingReply& pr = _combined->pending;
    pr.ack = expected_ack;
    pr.timestamp = ts;              // reuse on resend so the ACK hash stays stable
    pr.attempt = 0;
    pr.attempts_left = COMBINED_BOT_REPLY_RETRIES;
    pr.next_ms = futureMillis(est_timeout > 0 ? est_timeout : COMBINED_BOT_REPLY_TIMEOUT_MS);
    pr.to = to;
    StrHelper::strncpy(pr.text, text, sizeof(pr.text));
  }
#endif
}

#endif // WITH_BOT_COMMANDS
