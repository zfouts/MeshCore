// fleet_node — the control room: a private "@<node> <verb>" command surface.
//
// This file is a compile-time EXTENSION of examples/companion_radio. It defines
// the control-channel handler that companion_radio's MyMesh declares behind
// `#ifdef WITH_BOT_COMMANDS`. Compiled only by the `*_fleet_node_*` envs.
//
// The ENTIRE command surface is the control room: commands addressed as
// "@<node name> <verb>" on the channel indexed by bot_control_channel.
// Possession of that private channel's key is the whole auth story -- there are
// no public `!` commands, and DMs never reach the bot at all.

#include "MyMesh.h"

#ifdef WITH_BOT_COMMANDS

#include <Arduino.h>
#include <string.h>
#include <helpers/AdvertDataHelpers.h>   // ADV_TYPE_* for `@name advert`
#ifdef WITH_FLEET_EXTRAS
#include "FleetNode.h"   // bot rate-limiter
#endif

// Match a command word at the start of `s` (case-sensitive, like CommonCLI).
static bool cmdIs(const char* s, const char* word) {
  size_t n = strlen(word);
  return strncmp(s, word, n) == 0;
}

// NOTE: there is deliberately NO direct-message handler. The control room is
// channel-only; a DM is just a chat message to the companion app.

#ifdef WITH_FLEET_EXTRAS
// Incoming channel (group) handler. Only the control channel is live: every
// command is "@<node name> <verb>", authorized by possession of that private
// channel's key, and a single message can drive the whole fleet (only the
// named node acts). Everything else on any channel is ignored.
void MyMesh::handleBotChannel(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                              uint32_t timestamp, const char* text) {
  (void)pkt; (void)timestamp;
  // Channel messages arrive wire-formatted as "<sender_name>: <message>", so
  // split off the "name: " prefix; keep the sender to tag the reply.
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
  if (!_fleet || !_prefs.bot_enabled) return;
  if (msg == NULL || msg[0] != '@') return;             // control room speaks only '@name ...'

  // Only the control channel is authorized. Its private key is the auth, so one
  // message controls every listening node regardless of hop count.
  int rxidx = findChannelIdx(channel);
  if (_prefs.bot_control_channel == 0xFF || rxidx != (int)_prefs.bot_control_channel) return;

  handleTargetedCmd(channel, sender, msg + 1);
}

// "@<node name> <verb>" on the control channel. Name match is case-insensitive
// and tolerates spaces in the name (everything before the verb is the target);
// a node that isn't the target stays silent. Rate-limited per node. Reply is
// tagged "@<sender> <node>: <reply>".
void MyMesh::handleTargetedCmd(const mesh::GroupChannel& channel, const char* sender, const char* text) {
  size_t nlen = strlen(_prefs.node_name);
  if (nlen == 0 || strncasecmp(text, _prefs.node_name, nlen) != 0) return; // not us
  const char* p = text + nlen;
  while (*p == ' ') p++;

  if (!_fleet->bot_limiter.allow((uint32_t)(_ms->getMillis() / 1000))) return;

  char reply[140];
  reply[0] = 0;

  if (strncmp(p, "relay ", 6) == 0) {
    // Toggle packet forwarding (client_repeat). allowPacketForward() reads it
    // live per packet, so it takes effect immediately; savePrefs() persists it.
    const char* v = p + 6;
    if (cmdIs(v, "on"))       { _prefs.client_repeat = 1; savePrefs(); snprintf(reply, sizeof(reply), "relay on"); }
    else if (cmdIs(v, "off")) { _prefs.client_repeat = 0; savePrefs(); snprintf(reply, sizeof(reply), "relay off"); }

  } else if (strncmp(p, "ble ", 4) == 0) {
    // The reply rides the LoRa mesh, not BLE, so it still arrives after `off`.
    // Persisted and re-applied at boot (see startInterface).
#if defined(BLE_PIN_CODE)
    const char* v = p + 4;
    if (cmdIs(v, "on"))       { _prefs.ble_enabled = 1; savePrefs(); _serial->enable();  snprintf(reply, sizeof(reply), "ble on"); }
    else if (cmdIs(v, "off")) { _prefs.ble_enabled = 0; savePrefs(); _serial->disable(); snprintf(reply, sizeof(reply), "ble off"); }
#else
    snprintf(reply, sizeof(reply), "ble: n/a on this build");
#endif

  } else if (cmdIs(p, "sensors")) {
    fleetFormatSensorsText(reply, sizeof(reply));

  } else if (strncmp(p, "advert ", 7) == 0) {
    // FLOOD self-advert masquerading as the requested adv_type -- lets one
    // physical node present as a repeater or a companion (chat) on demand.
    const char* v = p + 7;
    uint8_t t = 0;
    const char* label = NULL;
    if (cmdIs(v, "repeater"))       { t = ADV_TYPE_REPEATER; label = "repeater"; }
    else if (cmdIs(v, "companion")) { t = ADV_TYPE_CHAT;     label = "companion"; }
    if (label == NULL) {
      snprintf(reply, sizeof(reply), "advert: want repeater|companion");
    } else {
      mesh::Packet* pkt = createAdvertAs(t);
      if (pkt) {
        TransportKey scope;
        memcpy(&scope.key, _prefs.default_scope_key, sizeof(scope.key));
        sendFloodScoped(scope, pkt, 0);
        snprintf(reply, sizeof(reply), "advert sent as %s", label);
      } else {
        snprintf(reply, sizeof(reply), "advert: failed");
      }
    }

  } else if (cmdIs(p, "reboot")) {
    // The remote unstick for a sealed node. Deferred (fleetLoop pulls the
    // trigger) so this reply gets on the air before the node drops.
    _fleet->reboot_at_ms = futureMillis(5000);
    snprintf(reply, sizeof(reply), "rebooting in 5s");

  } else if (cmdIs(p, "info")) {
    snprintf(reply, sizeof(reply), "%s fw %s relay:%s ble:%s",
             _prefs.node_name, FIRMWARE_VERSION,
             _prefs.client_repeat ? "on" : "off",
             _prefs.ble_enabled ? "on" : "off");

  } else if (strncmp(p, "set ", 4) == 0) {
    const char* s = p + 4;
    while (*s == ' ') s++;

    if (strncmp(s, "advert_interval ", 16) == 0) {
      int n = atoi(s + 16);
      if (n < 0 || n > 86400) {
        snprintf(reply, sizeof(reply), "advert_interval: 0-86400s");
      } else {
        // runtime-only: a reboot restores the build default (FLEET_ADVERT_INTERVAL_S)
        _fleet->advert_interval_s = (uint32_t)n;
        if (n > 0) { _fleet->next_advert_ms = futureMillis((uint32_t)n * 1000UL);
                     snprintf(reply, sizeof(reply), "advert_interval %ds (until reboot)", n); }
        else         snprintf(reply, sizeof(reply), "advert_interval off (until reboot)");
      }

    } else if (strncmp(s, "txpower ", 8) == 0) {
      int power = atoi(s + 8);
      if (power < -9 || power > MAX_LORA_TX_POWER) {
        snprintf(reply, sizeof(reply), "txpower: out of range (max %d)", MAX_LORA_TX_POWER);
      } else {
        _prefs.tx_power_dbm = (int8_t)power;
        savePrefs();
        radio_driver.setTxPower(_prefs.tx_power_dbm);
        snprintf(reply, sizeof(reply), "txpower %d", power);
      }

    } else if (strncmp(s, "location ", 9) == 0) {
      const char* v = s + 9;
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

    } else {
      snprintf(reply, sizeof(reply), "set: advert_interval|txpower|location");
    }

  } else {
    return; // unrecognised verb -> stay silent
  }

  if (reply[0]) {
    char tagged[180];
    snprintf(tagged, sizeof(tagged), "@%s %s: %s", sender[0] ? sender : "?", _prefs.node_name, reply);
    mesh::GroupChannel ch = channel;
    sendGroupMessage(getRTCClock()->getCurrentTimeUnique(), ch, _prefs.node_name, tagged, strlen(tagged));
  }
}
#endif // WITH_FLEET_EXTRAS

#endif // WITH_BOT_COMMANDS
