#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
  #if defined(EXTRAFS)
    #include <CustomLFS.h>
    CustomLFS ExtraFS(0xD4000, 0x19000, 128);
    DataStore store(InternalFS, ExtraFS, rtc_clock);
  #else
    DataStore store(InternalFS, rtc_clock);
  #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

#ifdef ESP32
  #ifdef WITH_RUNTIME_WIFI
    // BLE/USB primary + a WiFi TCP console provisioned at runtime via
    // `set wifi_ssid <ssid>` / `set wifi_pwd <pwd>` (meshcli), muxed into the
    // single companion endpoint. Compile-time WIFI_SSID is just the first-boot
    // default (see MyMesh constructor).
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface wifi_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
    #ifdef BLE_PIN_CODE
      #include <helpers/esp32/SerialBLEInterface.h>
      SerialBLEInterface primary_interface;
    #elif defined(SERIAL_RX)
      #include <helpers/ArduinoSerialInterface.h>
      ArduinoSerialInterface primary_interface;
      HardwareSerial companion_serial(1);
    #else
      #include <helpers/ArduinoSerialInterface.h>
      ArduinoSerialInterface primary_interface;
    #endif
    #include "SerialMux.h"
    SerialMux serial_interface(primary_interface, wifi_interface);
  #elif defined(WIFI_SSID)
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
  #elif defined(BLE_PIN_CODE)
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #elif defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(RP2040_PLATFORM)
  //#ifdef WIFI_SSID
  //  #include <helpers/rp2040/SerialWifiInterface.h>
  //  SerialWifiInterface serial_interface;
  //  #ifndef TCP_PORT
  //    #define TCP_PORT 5000
  //  #endif
  // #elif defined(BLE_PIN_CODE)
  //   #include <helpers/rp2040/SerialBLEInterface.h>
  //   SerialBLEInterface serial_interface;
  #if defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(NRF52_PLATFORM)
  #ifdef BLE_PIN_CODE
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(STM32_PLATFORM)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface serial_interface;
#else
  #error "need to define a serial interface"
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &serial_interface);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#if defined(ESP32) && (defined(WITH_RUNTIME_WIFI) || defined(WIFI_SSID))
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
  static bool wifi_admin_enabled = true;   // `@name set wifi off` via control channel

  static void wifiRegisterEvents() {
    WiFi.setAutoReconnect(true);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            // reason codes: 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT (usually a
            // wrong password), 201 NO_AP_FOUND, 202 AUTH_FAIL, 203 ASSOC_FAIL
            WIFI_DEBUG_PRINTLN("WiFi disconnected (reason %u). Flagging for reconnect...",
                               (unsigned)info.wifi_sta_disconnected.reason);
            wifi_needs_reconnect = true;
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            WIFI_DEBUG_PRINTLN("WiFi connected, ip=%s", WiFi.localIP().toString().c_str());
            wifi_needs_reconnect = false;
        }
    });
  }
#endif

#if defined(ESP32) && defined(WITH_RUNTIME_WIFI)
  static bool wifi_started = false;         // event hooks are one-time
  static bool wifi_server_started = false;  // TCP server is one-time, after WiFi.begin
  static bool wifi_has_creds = false;

  // Strong definition (weak fallback in CombinedNode.cpp errors out on builds
  // without WiFi hardware). (Re)applies credentials at runtime -- called at
  // boot from saved prefs and from `set wifi_ssid` / `set wifi_pwd`. An empty
  // ssid turns WiFi off.
  extern "C" bool combinedApplyWifi(const char* ssid, const char* pwd) {
    if (ssid[0] == 0) {
      wifi_has_creds = false;
      wifi_needs_reconnect = false;
      wifi_interface.disable();          // mux stops polling the TCP console
      WiFi.disconnect(true);             // also powers down the WiFi radio
      if (wifi_started) board.setInhibitSleep(false);
      return true;
    }
    if (!wifi_started) {
      wifi_started = true;
      wifiRegisterEvents();
    }
    board.setInhibitSleep(true);         // prevent sleep when WiFi is active
    wifi_has_creds = true;
    wifi_admin_enabled = true;
    wifi_needs_reconnect = false;
    WiFi.disconnect();
    WiFi.begin(ssid, pwd[0] ? pwd : NULL);   // NULL = open network
    // The TCP server may only start once WiFi.begin has brought up the network
    // stack -- opening the socket before that crashes and reboots the node
    // (hardware-bitten on XIAO S3), which is also why it can't live in
    // wifiRegisterEvents() above.
    if (!wifi_server_started) {
      wifi_server_started = true;
      wifi_interface.begin(TCP_PORT);
    }
    wifi_interface.enable();
    return true;
  }

  extern "C" bool combinedWifiStatus(char* buf, size_t bufsz) {
    if (!wifi_has_creds)                    snprintf(buf, bufsz, "off");
    else if (!wifi_admin_enabled)           snprintf(buf, bufsz, "admin-off");
    else if (WiFi.status() == WL_CONNECTED) snprintf(buf, bufsz, "%s", WiFi.localIP().toString().c_str());
    else                                    snprintf(buf, bufsz, "connecting");
    return true;
  }

  // `@name set wifi on|off` via control channel. Runtime-only by design: a
  // reboot restores WiFi, so a bad toggle can't permanently strand a node.
  extern "C" bool combinedSetWifiControl(bool enable) {
    if (!wifi_has_creds) return false;   // nothing provisioned to toggle
    wifi_admin_enabled = enable;
    if (enable) {
      auto p = the_mesh.getNodePrefs();
      WiFi.begin(p->wifi_ssid, p->wifi_pwd[0] ? p->wifi_pwd : NULL);
    } else {
      wifi_needs_reconnect = false;
      WiFi.disconnect(true);   // also powers down the WiFi radio
    }
    return true;
  }

  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>

  // JSON-safe copy of a node/sender name: drop the two characters that could
  // break out of a snprintf-built JSON string.
  static void jsonSanitize(const char* in, char* out, size_t outsz) {
    size_t o = 0;
    for (const char* p = in ? in : ""; *p && o < outsz - 1; p++)
      if (*p != '"' && *p != '\\') out[o++] = *p;
    out[o] = 0;
  }

  // `!path` map link: POST the hop chain to the mesh-observer device API
  // (`set obs_url` / `set obs_token`) and return the short URL it mints.
  // BLOCKING for up to ~5s including a TLS handshake -- the radio is not
  // serviced meanwhile. Bot replies are rate-limited and !path is a human
  // command, so the stall is accepted; everything fails soft to the plain
  // hex reply. Strong definition; weak fallback in CombinedNode.cpp.
  extern "C" bool combinedPathShortUrl(const char* hashes, const char* origin,
                                       const char* requester_pos, const char* reporter,
                                       const char* requester, char* out, size_t outsz) {
    auto p = the_mesh.getNodePrefs();
    if (!p->obs_url[0] || WiFi.status() != WL_CONNECTED) return false;

    char url[112];
    snprintf(url, sizeof(url), "%s/api/device/path", p->obs_url);
    char rep[33], req[33];
    jsonSanitize(reporter, rep, sizeof(rep));
    jsonSanitize(requester, req, sizeof(req));
    char body[288];
    int n = snprintf(body, sizeof(body), "{\"h\":\"%s\"", hashes);
    if (origin && origin[0])         n += snprintf(body + n, sizeof(body) - n, ",\"o\":\"%s\"", origin);
    if (requester_pos && requester_pos[0]) n += snprintf(body + n, sizeof(body) - n, ",\"q\":\"%s\"", requester_pos);
    if (rep[0])                      n += snprintf(body + n, sizeof(body) - n, ",\"n\":\"%s\"", rep);
    if (req[0])                      n += snprintf(body + n, sizeof(body) - n, ",\"r\":\"%s\"", req);
    if (n >= (int)sizeof(body) - 2) return false;   // truncated -> don't send garbage
    n += snprintf(body + n, sizeof(body) - n, "}");

    HTTPClient http;
    WiFiClientSecure tls;
    bool begun;
    if (strncmp(p->obs_url, "https:", 6) == 0) {
      tls.setInsecure();   // no CA bundle on-device; the device token is the auth
      begun = http.begin(tls, url);
    } else {
      begun = http.begin(url);
    }
    if (!begun) return false;
    http.setConnectTimeout(2500);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    if (p->obs_token[0]) http.addHeader("X-Device-Token", p->obs_token);

    bool found = false;
    int code = http.POST((uint8_t*)body, n);
    if (code == 200) {
      String resp = http.getString();
      int i = resp.indexOf("\"url\":\"");
      if (i >= 0) {
        int e = resp.indexOf('"', i + 7);
        int len = e - (i + 7);
        if (e > i && len > 0 && len < (int)outsz) {
          memcpy(out, resp.c_str() + i + 7, len);
          out[len] = 0;
          found = true;
        }
      }
    }
    http.end();
    return found;
  }
#elif defined(ESP32) && defined(WIFI_SSID)
  // Strong definition of the control-channel WiFi toggle (weak fallback in
  // BotCommands.cpp returns false on non-WiFi builds). Runtime-only by design:
  // a reboot restores WiFi, so a bad toggle can't permanently strand a node.
  extern "C" bool combinedSetWifiControl(bool enable) {
    wifi_admin_enabled = enable;
    if (enable) {
      WiFi.begin(WIFI_SSID, WIFI_PWD);
    } else {
      WiFi.disconnect(true);   // also powers down the WiFi radio
    }
    return true;
  }
#endif

void setup() {
  Serial.begin(115200);
#ifdef COMBINED_BOOT_TRACE
  delay(3000); Serial.println("[boot] serial up");
#endif

  board.begin();
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] board.begin ok");
#endif

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] radio_init...");
#endif
  if (!radio_init()) {
#ifdef COMBINED_BOOT_TRACE
    Serial.println("[boot] RADIO INIT FAILED (SX1262 not responding) -- halting");
#endif
    halt();
  }
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] radio ok");
#endif

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef BLE_PIN_CODE
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

  //#ifdef WIFI_SSID
  //  WiFi.begin(WIFI_SSID, WIFI_PWD);
  //  serial_interface.begin(TCP_PORT);
  // #elif defined(BLE_PIN_CODE)
  //   char dev_name[32+16];
  //   sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName());
  //   serial_interface.begin(dev_name, the_mesh.getBLEPin());
  #if defined(SERIAL_RX)
    companion_serial.setPins(SERIAL_RX, SERIAL_TX);
    companion_serial.begin(115200);
    serial_interface.begin(companion_serial);
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
#elif defined(ESP32)
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] spiffs...");
#endif
  SPIFFS.begin(true);
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] store...");
#endif
  store.begin();
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] mesh.begin...");
#endif
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] mesh ok");
#endif

#ifdef WITH_RUNTIME_WIFI
  #ifdef BLE_PIN_CODE
  primary_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  #elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  primary_interface.begin(companion_serial);
  #else
  primary_interface.begin(Serial);
  #endif
  // bring up the WiFi TCP console if credentials were provisioned (saved
  // prefs, or a compile-time WIFI_SSID first-boot default)
  if (the_mesh.getNodePrefs()->wifi_ssid[0]) {
#ifdef COMBINED_BOOT_TRACE
    Serial.println("[boot] wifi.begin...");
#endif
    combinedApplyWifi(the_mesh.getNodePrefs()->wifi_ssid, the_mesh.getNodePrefs()->wifi_pwd);
#ifdef COMBINED_BOOT_TRACE
    Serial.println("[boot] wifi started, tcp listening");
#endif
  }
#elif defined(WIFI_SSID)
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] wifi.begin...");
#endif
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  wifiRegisterEvents();

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  serial_interface.begin(TCP_PORT);
#ifdef COMBINED_BOOT_TRACE
  Serial.println("[boot] wifi started, tcp listening");
#endif
#elif defined(BLE_PIN_CODE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#else
  #error "need to define filesystem"
#endif

  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#endif
  }

#if defined(ESP32) && (defined(WITH_RUNTIME_WIFI) || defined(WIFI_SSID))
  #ifdef WITH_RUNTIME_WIFI
  bool wifi_provisioned = wifi_has_creds;
  #else
  bool wifi_provisioned = true;
  #endif
  // Safely attempt to reconnect every 10 seconds if flagged
  if (wifi_provisioned && wifi_admin_enabled && wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > 10000)) {
    WIFI_DEBUG_PRINTLN("Attempting manual WiFi reconnect...");
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }
#endif
}
