#pragma once

// Runtime WiFi station credentials for wifi_repeater.
//
// Instead of baking SSID/password into the firmware (which would put secrets in
// the repo), the creds live in a small file on the node's own filesystem and are
// configured over the CLI:
//   wifi                 -> show ssid / enabled / link state / IP
//   wifi ssid <name>     -> set SSID            (persists)
//   wifi pass <secret>   -> set password        (persists, never echoed back)
//   wifi on | off        -> enable/disable WiFi (persists)
//   wifi reconnect       -> (re)connect with the stored creds now
//
// First-time setup is over USB serial; once it has joined, the same commands are
// reachable over the TCP console. Nothing secret is compiled in or committed.

#if defined(ESP32) && defined(WITH_WIFI)

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>

#ifndef WIFI_CFG_FILE
  #define WIFI_CFG_FILE "/wifi_cfg"
#endif

class WifiConfig {
  char    _ssid[33];
  char    _pass[64];
  uint8_t _enabled;

public:
  WifiConfig() : _enabled(0) { _ssid[0] = 0; _pass[0] = 0; }

  bool        enabled() const { return _enabled && _ssid[0]; }
  const char* ssid()    const { return _ssid; }

  void load(FILESYSTEM* fs) {
    File f = fs->open(WIFI_CFG_FILE);
    if (!f) return;
    f.read((uint8_t*)&_enabled, 1);
    f.read((uint8_t*)_ssid, sizeof(_ssid));
    f.read((uint8_t*)_pass, sizeof(_pass));
    _ssid[sizeof(_ssid) - 1] = 0;
    _pass[sizeof(_pass) - 1] = 0;
    f.close();
  }

  void save(FILESYSTEM* fs) {
    File f = fs->open(WIFI_CFG_FILE, "w", true);   // ESP32 SPIFFS create-on-write
    if (!f) return;
    f.write((uint8_t*)&_enabled, 1);
    f.write((uint8_t*)_ssid, sizeof(_ssid));
    f.write((uint8_t*)_pass, sizeof(_pass));
    f.close();
  }

  // Connect now using the stored creds (no-op if disabled / no SSID).
  void applyConnect() {
    if (!enabled()) return;
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _pass);
  }

  // Handle the text after the "wifi" keyword. Writes a short status/result into
  // `reply`. Persists on any change. Returns nothing -- always consumes the cmd.
  void handleCommand(const char* arg, char* reply, size_t sz, FILESYSTEM* fs) {
    while (*arg == ' ') arg++;

    if (*arg == 0) {                               // bare "wifi" -> status
      bool up = (WiFi.status() == WL_CONNECTED);
      snprintf(reply, sz, "wifi ssid:%s en:%u link:%s ip:%s",
               _ssid[0] ? _ssid : "(unset)", (unsigned)_enabled,
               up ? "connected" : "down",
               up ? WiFi.localIP().toString().c_str() : "-");
      return;
    }
    if (memcmp(arg, "ssid ", 5) == 0) {
      strncpy(_ssid, arg + 5, sizeof(_ssid) - 1); _ssid[sizeof(_ssid) - 1] = 0;
      _enabled = 1; save(fs);
      snprintf(reply, sz, "OK - ssid set (wifi reconnect to apply)");
    } else if (memcmp(arg, "pass ", 5) == 0) {
      strncpy(_pass, arg + 5, sizeof(_pass) - 1); _pass[sizeof(_pass) - 1] = 0;
      save(fs);
      snprintf(reply, sz, "OK - pass set (wifi reconnect to apply)");
    } else if (strcmp(arg, "on") == 0) {
      _enabled = 1; save(fs);
      snprintf(reply, sz, "OK - wifi enabled");
    } else if (strcmp(arg, "off") == 0) {
      _enabled = 0; save(fs); WiFi.disconnect();
      snprintf(reply, sz, "OK - wifi disabled");
    } else if (strcmp(arg, "reconnect") == 0 || strcmp(arg, "connect") == 0) {
      if (!enabled()) { snprintf(reply, sz, "Err - set ssid first"); return; }
      WiFi.disconnect(); applyConnect();
      snprintf(reply, sz, "OK - connecting to %s", _ssid);
    } else {
      snprintf(reply, sz, "Err - wifi [ssid <x>|pass <x>|on|off|reconnect]");
    }
  }
};

// Defined in main.cpp; lets MyMesh::handleCommand route "wifi ..." here.
extern WifiConfig* g_wifi;

#endif // ESP32 && WITH_WIFI
