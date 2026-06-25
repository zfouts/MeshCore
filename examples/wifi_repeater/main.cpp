#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

// WiFi bring-up + read-only HTTP status server (ESP32 only). Guarded by WITH_WIFI
// so the same example still builds as a plain repeater when the flag is absent.
// WiFi credentials are NOT compiled in -- they are stored on the node and set
// over the USB serial CLI (see WifiConfig.h / the "wifi" command). HTTP exposes
// an info page at / and Prometheus metrics at /metrics (see WebStatus.h).
#if defined(ESP32) && defined(WITH_WIFI)
  #include <WiFi.h>
  #include <ESPmDNS.h>
  #include "WifiConfig.h"
  #include "WebStatus.h"
  static WifiConfig  wifi_cfg;
  WifiConfig*        g_wifi = &wifi_cfg;     // reached by MyMesh::handleCommand
  static bool wifi_needs_reconnect = false;
  static bool mdns_started = false;
  static unsigned long last_wifi_reconnect_attempt = 0;
  static unsigned long last_noise_sample = 0;

  // mDNS hostname from the node name: lowercase, spaces/underscores -> '-', keep
  // only [a-z0-9-]. e.g. "Heltec WiFi Repeater" -> "heltec-wifi-repeater".
  static String mdnsHostname(const char* name) {
    String h;
    for (const char* p = name; *p && h.length() < 63; p++) {
      char c = *p;
      if (c == ' ' || c == '_')                               h += '-';
      else if (c >= 'A' && c <= 'Z')                          h += (char)(c - 'A' + 'a');
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') h += c;
      // anything else is dropped
    }
    if (h.length() == 0) h = "mesh-repeater";
    return h;
  }
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

#if defined(ESP32) && defined(WITH_WIFI)
  static WebStatus web_status(the_mesh);     // defined after the_mesh (needs the ref)
#endif

void halt() {
  while (1) ;
}

static char command[160];

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

#if defined(ESP32) && defined(WITH_WIFI)
  // Keep the radio awake while WiFi is up (sleep would drop the link/socket),
  // load the stored creds, connect if configured, and start the LAN console.
  // With no creds yet, WiFi stays down -- configure it over USB serial with
  // `wifi ssid <x>` / `wifi pass <x>` / `wifi reconnect`.
  board.setInhibitSleep(true);
  // Bring up STA mode unconditionally so the network/TCP stack is initialized
  // even before we have credentials. Without this, starting the console's
  // WiFiServer below would hang when no creds are stored (the stack is only
  // implicitly initialized by WiFi.begin()).
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifi_needs_reconnect = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifi_needs_reconnect = false;
      Serial.print("WiFi IP: "); Serial.println(WiFi.localIP());
    }
  });
  wifi_cfg.load(fs);
  wifi_cfg.applyConnect();
  web_status.begin();
#endif

  board.onBootComplete();
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(ESP32) && defined(WITH_WIFI)
  web_status.loop();   // service HTTP clients (/ and /metrics)
#endif

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#if defined(ESP32) && defined(WITH_WIFI)
  // Start mDNS once WiFi is up, named after the node (heard at <name>.local).
  if (!mdns_started && WiFi.status() == WL_CONNECTED) {
    String hn = mdnsHostname(the_mesh.getNodePrefs()->node_name);
    if (MDNS.begin(hn.c_str())) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      Serial.print("mDNS: http://"); Serial.print(hn); Serial.println(".local");
      mdns_started = true;
    }
  }
  // Sample the operating-channel noise floor ~1/sec. getCurrentRSSI() reads the
  // radio's instantaneous RSSI without retuning, so it never leaves the mesh
  // frequency -- non-disruptive to relaying.
  if (millis() - last_noise_sample > 1000) {
    last_noise_sample = millis();
    the_mesh.recordNoiseSample(radio_driver.getCurrentRSSI());
  }

  // Best-effort manual reconnect if a configured station drops.
  if (wifi_cfg.enabled() && wifi_needs_reconnect &&
      (millis() - last_wifi_reconnect_attempt > 10000)) {
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }
  // Periodic WiFi heartbeat to serial: the GOT_IP print is one-shot and easy to
  // miss, so re-announce status every 10s. Surfaces the IP (for `nc <ip> 23`), a
  // stuck/failed join (wrong PSK, or a 5GHz-only SSID -- ESP32 is 2.4GHz only),
  // or that WiFi simply hasn't been configured yet.
  static unsigned long last_wifi_report = 0;
  if (millis() - last_wifi_report > 10000) {
    last_wifi_report = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi connected -- http://"); Serial.print(WiFi.localIP());
      Serial.print(":"); Serial.print(HTTP_PORT);
      Serial.print("/  metrics: http://"); Serial.print(WiFi.localIP());
      Serial.print(":"); Serial.print(HTTP_PORT); Serial.println("/metrics");
    } else if (wifi_cfg.enabled()) {
      Serial.print("WiFi not connected (status="); Serial.print(WiFi.status());
      Serial.print(", ssid="); Serial.print(wifi_cfg.ssid()); Serial.println(")");
    } else {
      Serial.println("WiFi not configured -- set over serial: wifi ssid <x> / wifi pass <x> / wifi reconnect");
    }
  }
  // No power-save sleep on WiFi builds: sleeping drops the link and TCP console.
#else
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
#endif
}
