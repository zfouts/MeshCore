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
            // SNTP: a power-on reset seeds the clock to a 2024 constant
            // (ESP32RTCClock::begin), which fails TLS cert validation until
            // someone runs `clock sync`. Unattended nodes need real time on
            // their own, so sync from NTP as soon as the network is up.
            // Async + idempotent; harmless if UDP 123 egress is blocked.
            configTime(0, 0, "pool.ntp.org");
        }
    });
  }
#endif

#if defined(ESP32) && defined(WITH_RUNTIME_WIFI)
  static bool wifi_started = false;         // event hooks are one-time
  static bool wifi_server_started = false;  // TCP server is one-time, after WiFi.begin
  static bool wifi_has_creds = false;

  // Strong definition (weak fallback in ObserverNode.cpp errors out on builds
  // without WiFi hardware). (Re)applies credentials at runtime -- called at
  // boot from saved prefs and from `set wifi_ssid` / `set wifi_pwd`. An empty
  // ssid turns WiFi off.
  extern "C" bool observerApplyWifi(const char* ssid, const char* pwd) {
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

  extern "C" bool observerWifiStatus(char* buf, size_t bufsz) {
    if (!wifi_has_creds)                    snprintf(buf, bufsz, "off");
    else if (!wifi_admin_enabled)           snprintf(buf, bufsz, "admin-off");
    else if (WiFi.status() == WL_CONNECTED) snprintf(buf, bufsz, "%s", WiFi.localIP().toString().c_str());
    else                                    snprintf(buf, bufsz, "connecting");
    return true;
  }

  // `@name set wifi on|off` via control channel. Runtime-only by design: a
  // reboot restores WiFi, so a bad toggle can't permanently strand a node.
  extern "C" bool observerSetWifiControl(bool enable) {
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
  // hex reply. Strong definition; weak fallback in ObserverNode.cpp.
  extern "C" bool observerPathShortUrl(const char* hashes, const char* origin,
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

  // ---- MQTT telemetry publisher (`set mqtt_host <host[:port]>`) -----------
  // Publishes to any MQTT 3.1.1 broker over the node's own OUTBOUND socket,
  // so the single-client TCP companion console stays free. esp_mqtt (bundled
  // with the ESP32 core) runs in its own FreeRTOS task and reconnects by
  // itself -- nothing here can stall the mesh loop, unlike the blocking
  // !path HTTP call.
  //
  // TLS: `set mqtt_host wss://host[:port]` (MQTT over TLS WebSockets, default
  // 443 -- the recommended transport; TLS terminates at an HTTPS ingress in
  // front of the broker) or `mqtts://host[:port]` (native TLS, default 8883).
  // Either way the server cert is verified against the Let's Encrypt
  // production roots PINNED in MqttCaCerts.h -- LE-issued certs work with no
  // per-device cert provisioning, anything else is rejected (opt-out:
  // mqtt_tls_insecure). Plain mqtt:// is deprecated -- lab/bench only.
  //
  // The ONE inbound path is the send bridge: <prefix>/send/<channel_idx>
  // payloads are posted as channel text into a channel this node already
  // holds keys for -- as this node, rate-limited, nothing else. No DM path,
  // no CLI/admin path: broker access lets you SPEAK in the node's channels,
  // not command the node. Everything else remains publish-only.
  #include <mqtt_client.h>
  #include "MqttCaCerts.h"
  #include "RateLimiter.h"
  #ifndef OBS_MQTT_INTERVAL_S
  #define OBS_MQTT_INTERVAL_S 60
  #endif
  // Send-bridge limits: mesh airtime is shared, so inbound MQTT is throttled.
  // Msgs over budget stay queued (depth 4); overflow beyond that is dropped.
  #ifndef MQTT_SEND_MAX_PER_MIN
  #define MQTT_SEND_MAX_PER_MIN 6
  #endif
  #define MQTT_SEND_QUEUE_DEPTH 4
  // Per-user fleet send bridge: an observer ALSO subscribes to
  // meshcore/<user>/all/send/+, so one publish reaches all of THAT user's
  // nodes (still inside the user's ACL namespace). Meant for a user's
  // observers on DISJOINT meshes (one bridge per mesh): two subscribers on the
  // SAME mesh each transmit their own copy -- packet dedup can't collapse them
  // (different timestamps) and the channel sees doubles. Same-mesh sends
  // should use the per-node send topic instead. -D MQTT_SHARED_SEND_ENABLE=0
  // disables the shared subscription.
  #ifndef MQTT_SHARED_SEND_ENABLE
  #define MQTT_SHARED_SEND_ENABLE 1
  #endif

  static esp_mqtt_client_handle_t mqtt_client = NULL;
  static bool mqtt_connected = false;
  // Topics are namespaced per user for multi-user safety: meshcore/<user>/<node>
  // (user = the MQTT login, so it lines up with a mosquitto `meshcore/%u/#`
  // ACL). mqtt_prefix is that per-node prefix; mqtt_shared_prefix is the
  // per-user fleet prefix meshcore/<user>/all. Buffers sized for the worst
  // case: "meshcore/" + user[32] + "/" + node[31].
  static char mqtt_prefix[80];
  static char mqtt_shared_prefix[48];   // meshcore/<user>/all ("" = disabled)
  static unsigned long next_mqtt_pub = 0;
  // Last connect failure, surfaced via `get mqtt` -- a TLS/refused/DNS error
  // otherwise looks like "connecting" forever. Written from the esp_mqtt task,
  // read from the main task: int-sized fields, torn reads are harmless.
  static int mqtt_err_type = 0;      // esp_mqtt_error_type_t
  static int mqtt_err_tls = 0;       // esp_tls_last_esp_err
  static int mqtt_err_sock = 0;      // transport sock errno
  static int mqtt_err_rc = 0;        // broker CONNACK return code
  // Reconnect policy. Ported from agessaman/MeshCore observer-firmware
  // (src/helpers/MQTTConnectionPolicy.h + MQTTBridge::optimizeMqttClientConfig).
  //
  // esp_mqtt's own auto-reconnect is DISABLED. It retries on a flat 10s timer
  // forever, and every retry is a fresh TLS handshake needing ~40KB of internal
  // DRAM -- so an outage became a heap-thrash loop, which the old watchdog
  // "solved" by calling ESP.restart() every 5 minutes. That reboot loop WAS the
  // flapping: the node lost mesh state and re-adverted on every cycle, and a
  // broker-side fault looked like a firmware fault. There is no reboot here now.
  //
  // Instead, three things, all of them from the reference implementation:
  //   1. an exponential ladder, so a dead broker is retried rarely, not hourly*720;
  //   2. a stability requirement before the ladder resets -- CONNACK alone proves
  //      only that the handshake worked, so a link that cannot survive one
  //      keepalive round-trip keeps its earned rung instead of dropping back to
  //      10s and hammering TLS (this is what stops flap -> reset -> flap);
  //   3. a circuit breaker, so a slot that is genuinely down stops costing heap.
  static unsigned long mqtt_connected_at = 0;    // millis of CONNACK; 0 = down or already reset
  static unsigned long mqtt_last_attempt = 0;
  static uint8_t mqtt_backoff = 0;               // ladder index; 5 = saturated at the top rung
  static uint8_t mqtt_top_fails = 0;             // consecutive failures at the top rung
  static bool mqtt_breaker = false;              // tripped: routine retries stopped
  #define MQTT_STABLE_RESET_MS   120000UL   // hold this long before the ladder resets
  #define MQTT_BREAKER_PROBE_MS 1800000UL   // tripped -> probe once every 30 min
  #define MQTT_MAX_FAILS_AT_TOP 3
  static const unsigned long MQTT_BACKOFF_MS[] = {
    10000UL, 30000UL, 60000UL, 120000UL, 300000UL
  };
  static unsigned long mqttBackoffMs() {
    return MQTT_BACKOFF_MS[mqtt_backoff < 5 ? mqtt_backoff : 4];
  }
  // Cloudflare -- and most HTTPS ingresses -- close an idle WebSocket at ~100s,
  // not configurable, so keepalive has to sit safely under that. The old 30s
  // gave 2.5x more chances for one late PINGRESP to tear down the session and
  // force a full re-handshake.
  #ifdef BOARD_HAS_PSRAM
  #define OBS_MQTT_KEEPALIVE_S 45
  #else
  #define OBS_MQTT_KEEPALIVE_S 75
  #endif

  // Send bridge: the esp_mqtt task parses <prefix>/send/<channel> messages
  // into this queue; mqttLoop() (main task) drains and transmits. Cross-task
  // via FreeRTOS queue -- the mesh stack may only be touched from the main
  // loop, which is also why the channel is carried as a STRING: resolving a
  // name to a slot needs mesh channel state, so it happens at drain time.
  struct MqttSendMsg { char channel[32]; char text[MAX_TEXT_LEN + 1]; };
  static QueueHandle_t mqtt_send_q = NULL;
  static RateLimiter mqtt_send_limit(MQTT_SEND_MAX_PER_MIN, 60);

  // If `topic` is "<pfx>/send/<channel>", return the <channel> suffix (into
  // *suffix_len), else NULL. `topic` is esp_mqtt's non-NUL-terminated buffer.
  static const char* sendTopicSuffix(const char* topic, int topic_len, const char* pfx, int* suffix_len) {
    int plen = (int)strlen(pfx);
    if (plen == 0 || topic_len <= plen + 6) return NULL;
    if (strncmp(topic, pfx, plen) != 0 || strncmp(topic + plen, "/send/", 6) != 0) return NULL;
    *suffix_len = topic_len - plen - 6;
    return topic + plen + 6;
  }

  // Topic hygiene on a free-form name segment: no wildcards / level separators.
  static void mqttSanitizeSeg(char* s) {
    for (; *s; s++) if (*s == '#' || *s == '+' || *s == '/' || *s == ' ') *s = '-';
  }

  static void mqttBuildPrefix() {
    auto p = the_mesh.getNodePrefs();
    char user[sizeof(p->mqtt_user)]; StrHelper::strzcpy(user, p->mqtt_user, sizeof(user));
    mqttSanitizeSeg(user);
    if (p->mqtt_topic[0]) {
      // explicit override wins, used verbatim
      snprintf(mqtt_prefix, sizeof(mqtt_prefix), "%s", p->mqtt_topic);
    } else if (p->mqtt_iata[0]) {
      // Collector layout: meshcore/<iata>/<node>. CoreScope and agessaman-style
      // ingests parse segment 1 as the observer's REGION (and filter/blacklist on
      // it), not as a login -- so when an IATA is set it takes the segment.
      // NOTE this is deliberately mutually exclusive with the mqtt_user layout
      // below: a mosquitto `topic ... meshcore/%u/#` ACL stops matching once
      // segment 1 is a region, so the two namespaces cannot both be live.
      char iata[sizeof(p->mqtt_iata)]; StrHelper::strzcpy(iata, p->mqtt_iata, sizeof(iata));
      mqttSanitizeSeg(iata);
      snprintf(mqtt_prefix, sizeof(mqtt_prefix), "meshcore/%s/%s", iata, p->node_name);
      mqttSanitizeSeg(mqtt_prefix + 9 + strlen(iata) + 1);   // hygiene on the <node> part only
    } else if (user[0]) {
      // per-user namespace: meshcore/<user>/<node> (ACL-aligned)
      snprintf(mqtt_prefix, sizeof(mqtt_prefix), "meshcore/%s/%s", user, p->node_name);
      mqttSanitizeSeg(mqtt_prefix + 9 + strlen(user) + 1);   // hygiene on the <node> part only
    } else {
      // anonymous (no login): fall back to meshcore/<node>
      snprintf(mqtt_prefix, sizeof(mqtt_prefix), "meshcore/%s", p->node_name);
      mqttSanitizeSeg(mqtt_prefix + 9);
    }
    size_t l = strlen(mqtt_prefix);
    while (l > 0 && mqtt_prefix[l - 1] == '/') mqtt_prefix[--l] = 0;

    // Per-user fleet prefix: meshcore/<user>/all (only when a user is set).
    if (MQTT_SHARED_SEND_ENABLE && user[0])
      snprintf(mqtt_shared_prefix, sizeof(mqtt_shared_prefix), "meshcore/%s/all", user);
    else
      mqtt_shared_prefix[0] = 0;
  }

  static void mqttOnEvent(void* arg, esp_event_base_t base, int32_t event_id, void* event_data) {
    if (event_id == MQTT_EVENT_CONNECTED) {
      mqtt_connected = true;
      char t[96];
      snprintf(t, sizeof(t), "%s/status", mqtt_prefix);
      // enqueue(), not publish(): this runs in the esp_mqtt task's event
      // dispatch, where a blocking write stalls the event loop (and with it the
      // keepalive PINGs) for up to network.timeout_ms.
      esp_mqtt_client_enqueue(mqtt_client, t, "online", 0, 1, true, true);  // pairs with the LWT below
      snprintf(t, sizeof(t), "%s/send/+", mqtt_prefix);
      esp_mqtt_client_subscribe(mqtt_client, t, 0);                   // MQTT->mesh send bridge
      if (mqtt_shared_prefix[0] && strcmp(mqtt_shared_prefix, mqtt_prefix) != 0) {
        snprintf(t, sizeof(t), "%s/send/+", mqtt_shared_prefix);      // per-user fleet send bridge
        esp_mqtt_client_subscribe(mqtt_client, t, 0);
      }
      next_mqtt_pub = 0;   // first telemetry right away
      mqtt_err_type = mqtt_err_tls = mqtt_err_sock = mqtt_err_rc = 0;
      mqtt_connected_at = millis();
      // The backoff ladder is deliberately NOT reset here. mqttLoop() resets it
      // only once the link has held MQTT_STABLE_RESET_MS.
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
      mqtt_connected = false;   // mqttLoop() schedules the retry (auto-reconnect is off)
      mqtt_connected_at = 0;
    } else if (event_id == MQTT_EVENT_ERROR) {
      esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
      if (ev->error_handle) {
        mqtt_err_type = ev->error_handle->error_type;
        mqtt_err_tls  = ev->error_handle->esp_tls_last_esp_err;
        mqtt_err_sock = ev->error_handle->esp_transport_sock_errno;
        mqtt_err_rc   = ev->error_handle->connect_return_code;
      }
    } else if (event_id == MQTT_EVENT_DATA) {
      // Send bridge, esp_mqtt task context: parse + queue ONLY, no mesh calls.
      esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
      if (ev->current_data_offset != 0) return;   // fragmented = way over text size
      if (ev->topic == NULL || ev->data_len <= 0 || mqtt_send_q == NULL) return;
      int clen = 0;
      const char* chan = sendTopicSuffix(ev->topic, ev->topic_len, mqtt_prefix, &clen);
      if (!chan && mqtt_shared_prefix[0])
        chan = sendTopicSuffix(ev->topic, ev->topic_len, mqtt_shared_prefix, &clen);
      if (!chan || clen <= 0 || clen >= (int)sizeof(MqttSendMsg::channel)) return;
      MqttSendMsg m;
      memcpy(m.channel, chan, clen);
      m.channel[clen] = 0;
      int n = ev->data_len < (int)sizeof(m.text) - 1 ? ev->data_len : (int)sizeof(m.text) - 1;
      memcpy(m.text, ev->data, n);
      m.text[n] = 0;
      xQueueSend(mqtt_send_q, &m, 0);             // queue full -> message dropped
    }
  }

  // (Re)start the client from prefs -- called at boot and on every
  // `set mqtt_*`. Strong def; weak fallback in ObserverNode.cpp errors the
  // vars out on builds without WiFi.
  // Tear down and rebuild the esp_mqtt client. Used both by a config change
  // and by the reconnect escalation -- a full re-init is what clears a
  // half-open socket or a wedged mbedtls context, and is the strongest recovery
  // available now that ESP.restart() is gone.
  static bool mqttStartClient() {
    if (mqtt_client) {                 // config change: rebuild from scratch
      esp_mqtt_client_stop(mqtt_client);
      esp_mqtt_client_destroy(mqtt_client);
      mqtt_client = NULL;
      mqtt_connected = false;
    }
    auto p = the_mesh.getNodePrefs();
    if (!p->mqtt_host[0]) return true;   // MQTT off
    mqttBuildPrefix();
    if (mqtt_send_q == NULL)             // created once, survives client rebuilds
      mqtt_send_q = xQueueCreate(MQTT_SEND_QUEUE_DEPTH, sizeof(MqttSendMsg));
    // esp_mqtt copies the config strings at init, but keep them static anyway.
    // Schemes: mqtt:// mqtts:// (native TCP) plus ws:// wss:// (MQTT over
    // WebSockets — lets the broker live behind a single 443 ingress /
    // Cloudflare; arduino-esp32 builds ship CONFIG_MQTT_TRANSPORT_WEBSOCKET).
    // No URI path support: `set mqtt_host wss://host[:port]` only.
    static char uri[96], lwt[104];
    const char* host = p->mqtt_host;
    const char* scheme = "mqtt";
    const char* defport = ":1883";
    bool tls = false;
    if      (strncmp(host, "mqtts://", 8) == 0) { tls = true; host += 8; scheme = "mqtts"; defport = ":8883"; }
    else if (strncmp(host, "mqtt://",  7) == 0) {             host += 7; }
    else if (strncmp(host, "wss://",   6) == 0) { tls = true; host += 6; scheme = "wss";   defport = ":443"; }
    else if (strncmp(host, "ws://",    5) == 0) {             host += 5; scheme = "ws";    defport = ":80"; }
    snprintf(uri, sizeof(uri), "%s://%s%s", scheme, host,
             strchr(host, ':') ? "" : defport);
    snprintf(lwt, sizeof(lwt), "%s/status", mqtt_prefix);
    esp_mqtt_client_config_t cfg = {};
  #if ESP_IDF_VERSION_MAJOR >= 5
    cfg.broker.address.uri = uri;
    if (tls) {
      // Insecure mode (`set mqtt_tls_insecure on`): attach no CA, so esp-tls
      // does NOT verify the server cert -- for brokers behind a private CA or
      // an IP/hostname the pinned LE roots can't vouch for. Opt-in per node;
      // the default still pins the Let's Encrypt roots.
      if (p->mqtt_tls_insecure) cfg.broker.verification.skip_cert_common_name_check = true;
      else                      cfg.broker.verification.certificate = OBS_MQTT_CA_PEM;
    }
    if (p->mqtt_user[0]) cfg.credentials.username = p->mqtt_user;
    if (p->mqtt_pwd[0])  cfg.credentials.authentication.password = p->mqtt_pwd;
    cfg.session.last_will.topic = lwt;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.retain = true;
    cfg.session.keepalive = OBS_MQTT_KEEPALIVE_S;
    cfg.network.disable_auto_reconnect = true;   // mqttLoop() drives reconnection
    // esp-mqtt resends an unacked QoS-1 PUBLISH every 1s by default, so a single
    // slow PUBACK floods subscribers with byte-identical copies of one message.
    // 15s allows exactly one retry inside the 30s outbox expiry.
    cfg.session.message_retransmit_timeout = 15000;
    // WSS layers the WebSocket transport on top of esp-tls; mbedtls handshake
    // frames are deep and the 6144 default is thin for the pair.
    cfg.task.stack_size = 8192;
    // Set both explicitly: out_size defaults to 0, which tracks buffer.size, and
    // a later change to one silently moves the other. Largest publish is the
    // ~560B msg payload plus a ~104B topic and header.
    cfg.buffer.size = 1024;
    cfg.buffer.out_size = 1024;
  #else
    cfg.uri = uri;
    if (tls) {
      if (p->mqtt_tls_insecure) cfg.skip_cert_common_name_check = true;
      else                      cfg.cert_pem = OBS_MQTT_CA_PEM;
    }
    if (p->mqtt_user[0]) cfg.username = p->mqtt_user;
    if (p->mqtt_pwd[0])  cfg.password = p->mqtt_pwd;
    cfg.lwt_topic = lwt;
    cfg.lwt_msg = "offline";
    cfg.lwt_retain = 1;
    cfg.keepalive = OBS_MQTT_KEEPALIVE_S;
    cfg.disable_auto_reconnect = true;
    cfg.message_retransmit_timeout = 15000;
    cfg.task_stack = 8192;
    cfg.buffer_size = 1024;
    cfg.out_buffer_size = 1024;
  #endif
    mqtt_client = esp_mqtt_client_init(&cfg);
    if (mqtt_client == NULL) return true;  // vars accepted; client start retried on next set
    esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqttOnEvent, NULL);
    esp_mqtt_client_start(mqtt_client);
    return true;
  }

  // Called at boot and on every `set mqtt_*`. Reconfiguring a slot clears the
  // circuit breaker and the ladder (same rule as the reference implementation):
  // the operator just changed something, so the previous failures no longer
  // describe the current config.
  extern "C" bool observerApplyMqtt() {
    mqtt_backoff = 0;
    mqtt_top_fails = 0;
    mqtt_breaker = false;
    mqtt_last_attempt = millis();
    return mqttStartClient();
  }

  extern "C" bool observerMqttStatus(char* buf, size_t bufsz) {
    auto p = the_mesh.getNodePrefs();
    if (!p->mqtt_host[0])    snprintf(buf, bufsz, "off");
    else if (mqtt_connected) snprintf(buf, bufsz, "connected");
    // r=backoff rung t=esp_mqtt_error_type tls=esp-tls err sock=errno rc=CONNACK.
    // sock/rc are appended ONLY when non-zero: this string is packed into the
    // 176-byte custom-vars reply frame, where every byte spent here evicts a
    // trailing var (that cap is why the status buffer was widened 24->48 in the
    // first place). Omitting the two fields that read 0 in the common transport
    // failure makes this SHORTER than the old fixed-width form even with the
    // rung added -- "connecting r2 t1 tls0x8006" vs "connecting t1 tls0x8006 sock0 rc0".
    else if (mqtt_err_type) {
      int n = snprintf(buf, bufsz, "%s r%d t%d tls0x%x",
                       mqtt_breaker ? "breaker" : "connecting", mqtt_backoff,
                       mqtt_err_type, (unsigned)mqtt_err_tls);
      if (n > 0 && mqtt_err_sock && (size_t)n < bufsz)
        n += snprintf(buf + n, bufsz - n, " sock%d", mqtt_err_sock);
      if (n > 0 && mqtt_err_rc && (size_t)n < bufsz)
        snprintf(buf + n, bufsz - n, " rc%d", mqtt_err_rc);
    }
    else                     snprintf(buf, bufsz, "%s r%d",
                                      mqtt_breaker ? "breaker" : "connecting", mqtt_backoff);
    return true;
  }

  // Chat text is arbitrary: escape it before it goes inside a JSON string.
  static int mqttJsonEscape(const char* in, char* out, size_t outsz) {
    size_t o = 0;
    for (const char* p = in ? in : ""; *p && o + 2 < outsz; p++) {
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\')  { out[o++] = '\\'; out[o++] = (char)c; }
      else if (c == '\n')         { out[o++] = '\\'; out[o++] = 'n'; }
      else if (c == '\r')         { out[o++] = '\\'; out[o++] = 'r'; }
      else if (c == '\t')         { out[o++] = '\\'; out[o++] = 't'; }
      else if (c >= 0x20)         { out[o++] = (char)c; }
      // other control chars dropped
    }
    out[o] = 0;
    return (int)o;
  }

  // Mirror every readable received message to the broker:
  //   <prefix>/msg/dm       {"from":"<contact>","text":"...","snr":-7.5}
  //   <prefix>/msg/channel  {"channel":"#bot","text":"<sender>: <msg>","snr":-7.5}
  // This INCLUDES private channels the node holds keys for (control channel
  // too) -- decrypted mesh traffic lands on the broker, the operator's
  // explicit choice. Enqueued to the esp_mqtt task at QoS 0, so the radio
  // path that just delivered the message is never blocked.
  // `hops` = hex hop-hash chain the message flooded over to reach us (NULL/empty
  // if heard direct); `hops_n` = hop count (0 = direct neighbour). Same ingress
  // path data as adverts, but sampled at message time -- feeds the topology graph.
  extern "C" void observerMqttMessage(const char* kind, const char* channel,
                                      const char* from, const char* text, float snr,
                                      const char* hops, int hops_n, uint32_t sender_ts) {
    if (!mqtt_client || !mqtt_connected) return;
    char esc_text[360], esc_who[48];
    mqttJsonEscape(text, esc_text, sizeof(esc_text));
    mqttJsonEscape(channel ? channel : (from ? from : "?"), esc_who, sizeof(esc_who));
    char topic[104], payload[560];
    snprintf(topic, sizeof(topic), "%s/msg/%s", mqtt_prefix, kind);
    int n = snprintf(payload, sizeof(payload), "{\"%s\":\"%s\",\"text\":\"%s\",\"snr\":%.1f,\"hops_n\":%d",
                     channel ? "channel" : "from", esc_who, esc_text, (double)snr, hops_n);
    if (hops && hops[0])
      n += snprintf(payload + n, sizeof(payload) - n, ",\"hops\":\"%s\"", hops);
    // sender_ts = the message's own embedded clock; rx_ts = our NTP receive time;
    // skew_s = rx_ts - sender_ts (same fields/semantics as observerMqttAdvert).
    uint32_t rx_ts = rtc_clock.getCurrentTime();
    long skew = (long)rx_ts - (long)sender_ts;
    n += snprintf(payload + n, sizeof(payload) - n,
                  ",\"sender_ts\":%lu,\"rx_ts\":%lu,\"skew_s\":%ld",
                  (unsigned long)sender_ts, (unsigned long)rx_ts, skew);
    n += snprintf(payload + n, sizeof(payload) - n, "}");
    if (n > 0 && n < (int)sizeof(payload))
      esp_mqtt_client_enqueue(mqtt_client, topic, payload, n, 0, false, true);
  }

  // Advert dump (`set advert_dump on`): publish each heard advert to
  // <prefix>/advert. `adv_ts` is the advertising node's OWN clock (from the
  // advert); `rx_ts` is our NTP-disciplined receive time; `skew_s` = rx_ts -
  // adv_ts is the node's clock error (huge positive -> node stuck in the past;
  // negative/future -> node ahead). `hash` (payload+type, path-independent) lets
  // a consumer group relay copies of one advert vs distinct re-stamped adverts.
  // `raw` is the full packet as hex for byte-level decode. Built for diagnosing
  // which nodes advertise a bad timestamp. Enqueued (non-blocking, RX-safe).
  extern "C" void observerMqttAdvert(const uint8_t* pk4, const uint8_t* hash8, uint32_t adv_ts,
                                     const char* type, const char* name, float snr, int hops_n,
                                     const uint8_t* raw, int raw_len) {
    if (!mqtt_client || !mqtt_connected) return;
    uint32_t rx_ts = rtc_clock.getCurrentTime();
    long skew = (long)rx_ts - (long)adv_ts;
    char pk[9];  for (int b = 0; b < 4; b++) snprintf(pk + b * 2, 3, "%02x", pk4[b]);
    char hh[17]; for (int b = 0; b < 8; b++) snprintf(hh + b * 2, 3, "%02x", hash8[b]);
    char rawhex[MAX_TRANS_UNIT * 2 + 1]; int ho = 0;
    for (int b = 0; b < raw_len && ho + 2 < (int)sizeof(rawhex); b++)
      ho += snprintf(rawhex + ho, sizeof(rawhex) - ho, "%02x", raw[b]);
    rawhex[ho] = 0;
    char esc_name[64]; mqttJsonEscape(name ? name : "", esc_name, sizeof(esc_name));
    char topic[112]; snprintf(topic, sizeof(topic), "%s/advert", mqtt_prefix);
    char payload[768];
    int n = snprintf(payload, sizeof(payload),
      "{\"pubkey\":\"%s\",\"hash\":\"%s\",\"adv_ts\":%lu,\"rx_ts\":%lu,\"skew_s\":%ld,"
      "\"type\":\"%s\",\"name\":\"%s\",\"snr\":%.1f,\"hops_n\":%d,\"raw\":\"%s\"}",
      pk, hh, (unsigned long)adv_ts, (unsigned long)rx_ts, skew, type, esc_name,
      (double)snr, hops_n, rawhex);
    if (n > 0 && n < (int)sizeof(payload))
      esp_mqtt_client_enqueue(mqtt_client, topic, payload, n, 0, false, true);
  }

  #ifndef OBS_MQTT_CONTACTS_INTERVAL_S
  #define OBS_MQTT_CONTACTS_INTERVAL_S 300
  #endif
  static unsigned long next_mqtt_contacts = 0;

  // Walk the whole contact table and publish each known node RETAINED under
  // <prefix>/contact/<8-hex pubkey prefix>: name, type, coordinates (when the
  // node adverts a position), and when we last heard its advert. Retained so
  // the broker always holds the current roster/map even across broker or node
  // restarts, and re-published on a slow timer so positions track movement.
  static void mqttPublishContacts() {
    static const char* TYPE_STR[] = {"none", "chat", "repeater", "room", "sensor"};
    int nc = the_mesh.getNumContacts();
    for (int i = 0; i < nc; i++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx((uint32_t)i, c)) continue;
      if (c.name[0] == 0 && c.last_advert_timestamp == 0) continue;  // empty slot
      char pk[9];
      for (int b = 0; b < 4; b++) snprintf(pk + b * 2, 3, "%02x", c.id.pub_key[b]);
      char topic[104], payload[224], esc_name[64];
      snprintf(topic, sizeof(topic), "%s/contact/%s", mqtt_prefix, pk);
      mqttJsonEscape(c.name, esc_name, sizeof(esc_name));
      const char* ts = (c.type <= ADV_TYPE_SENSOR) ? TYPE_STR[c.type] : "unknown";
      int n = snprintf(payload, sizeof(payload),
        "{\"pubkey\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"heard\":%lu",
        pk, esc_name, ts, (unsigned long)c.last_advert_timestamp);
      if (c.gps_lat != 0 || c.gps_lon != 0)   // 0,0 = no adverted position
        n += snprintf(payload + n, sizeof(payload) - n, ",\"lat\":%.6f,\"lon\":%.6f",
                      c.gps_lat / 1e6, c.gps_lon / 1e6);
      n += snprintf(payload + n, sizeof(payload) - n, "}");
      if (n > 0 && n < (int)sizeof(payload))
        esp_mqtt_client_enqueue(mqtt_client, topic, payload, n, 0, true, true);
    }
  }

  // Advert ingress paths: for each node whose advert we've heard, publish the
  // hop chain the advert flooded over to reach us -> meshcore/<node>/heard/<pk>.
  // This is the receive-side topology source (every adverting node, passively),
  // complementing the probe's outbound repeater paths. path_len is ENCODED
  // (count in low 6 bits, hash-size-1 in the top 2), so decode the byte length.
  // "snr" is the advert as heard HERE: for hops_n=0 that's the link to the
  // origin node; for multi-hop it's the last relay's transmission.
  #ifndef OBS_MQTT_HEARD_INTERVAL_S
  #define OBS_MQTT_HEARD_INTERVAL_S 180
  #endif
  static unsigned long next_mqtt_heard = 0;
  static void mqttPublishHeard() {
    AdvertPath heard[16];
    int n = the_mesh.getRecentlyHeard(heard, 16);
    for (int i = 0; i < n; i++) {
      const AdvertPath& h = heard[i];
      char pk[9];
      for (int b = 0; b < 4; b++) snprintf(pk + b * 2, 3, "%02x", h.pubkey_prefix[b]);
      int hop_n = h.path_len & 63;
      int blen  = hop_n * ((h.path_len >> 6) + 1);   // count * hash-size
      char hops[MAX_PATH_SIZE * 2 + 1]; hops[0] = 0;
      if (blen > 0 && blen <= MAX_PATH_SIZE)
        for (int b = 0; b < blen; b++) snprintf(hops + b * 2, 3, "%02x", h.path[b]);
      char topic[104], payload[256], esc_name[64];
      snprintf(topic, sizeof(topic), "%s/heard/%s", mqtt_prefix, pk);
      mqttJsonEscape(h.name, esc_name, sizeof(esc_name));
      int nn = snprintf(payload, sizeof(payload),
        "{\"pubkey\":\"%s\",\"name\":\"%s\",\"hops_n\":%d,\"ts\":%lu,\"snr\":%.1f",
        pk, esc_name, hop_n, (unsigned long)h.recv_timestamp, (double)h.snr_x4 / 4.0);
      if (hops[0]) nn += snprintf(payload + nn, sizeof(payload) - nn, ",\"hops\":\"%s\"", hops);
      nn += snprintf(payload + nn, sizeof(payload) - nn, "}");
      if (nn > 0 && nn < (int)sizeof(payload))
        esp_mqtt_client_enqueue(mqtt_client, topic, payload, nn, 0, true, true);  // retained
    }
  }

  // Every received frame, as raw hex -> <prefix>/packets, in the shape CoreScope's
  // ingestor expects: {"raw":"<hex>","SNR":<f>,"RSSI":<f>} (their key casing, not
  // ours). It decodes the frame itself, so this is deliberately NOT our decoded
  // advert/contact/msg data -- those topics stay as they are for the bot framework
  // and are simply ignored by collectors that don't know them.
  //
  // Called from logRxRaw() in the mesh loop (main task), once per frame HEARD --
  // including frames we drop. That is a much higher publish rate than anything
  // else here, which is why it is opt-in (`set mqtt_packets on`) and enqueued
  // rather than published: enqueue() never blocks the mesh loop on the socket.
  extern "C" void observerMqttRawPacket(float snr, float rssi, const uint8_t* raw, int len) {
    auto p = the_mesh.getNodePrefs();
    if (!p->mqtt_packets || !mqtt_client || !mqtt_connected) return;
    if (len <= 0 || len > MAX_TRANS_UNIT) return;
    // MAX_TRANS_UNIT(255)*2 hex + the JSON envelope; stays under the 1024 buffer.
    char payload[MAX_TRANS_UNIT * 2 + 64];
    int n = snprintf(payload, sizeof(payload), "{\"raw\":\"");
    for (int i = 0; i < len; i++) n += snprintf(payload + n, sizeof(payload) - n, "%02x", raw[i]);
    n += snprintf(payload + n, sizeof(payload) - n, "\",\"SNR\":%.1f,\"RSSI\":%.0f}",
                  (double)snr, (double)rssi);
    if (n <= 0 || n >= (int)sizeof(payload)) return;
    char topic[104];
    snprintf(topic, sizeof(topic), "%s/packets", mqtt_prefix);
    esp_mqtt_client_enqueue(mqtt_client, topic, payload, n, 0, false, true);
  }

  static void mqttLoop() {
    if (!mqtt_client) return;
    if (!mqtt_connected) {
      // Reconnect driver (auto-reconnect is off; see the ladder notes above).
      // This MUST also cover the never-yet-connected case: with esp_mqtt's own
      // retry disabled, a node whose broker is down at boot would otherwise sit
      // at "connecting" forever. The old watchdog could gate on a prior success
      // because its escalation was a reboot; the ladder has no such hazard, and
      // a genuinely bad mqtt_host simply climbs to the top rung and trips the
      // breaker, which is the wanted outcome.
      // Gated on WiFi only: a WiFi outage is WiFi's problem, not MQTT's, and
      // must not burn rungs.
      if (WiFi.status() == WL_CONNECTED) {
        unsigned long now = millis();
        if (mqtt_breaker) {
          // Tripped: no routine retries. One full re-init probe every 30 min;
          // if it connects and holds, the stable-reset below clears the breaker.
          if (now - mqtt_last_attempt >= MQTT_BREAKER_PROBE_MS) {
            mqtt_last_attempt = now;
            mqttStartClient();
          }
        } else if (now - mqtt_last_attempt >= mqttBackoffMs()) {
          mqtt_last_attempt = now;
          if (mqtt_backoff < 5) {
            mqtt_backoff++;                        // climb: 10 -> 30 -> 60 -> 120 -> 300s
            esp_mqtt_client_reconnect(mqtt_client);
          } else if (++mqtt_top_fails >= MQTT_MAX_FAILS_AT_TOP) {
            mqtt_breaker = true;                   // ~15 min at the top rung -> give up
          } else {
            mqttStartClient();                     // at the top rung, escalate to a re-init
          }
        }
      }
      return;
    }
    // Connected. The ladder resets only once the link has PROVEN stable for
    // longer than one keepalive round-trip -- CONNACK alone does not count.
    if (mqtt_connected_at && (millis() - mqtt_connected_at) >= MQTT_STABLE_RESET_MS) {
      mqtt_backoff = 0;
      mqtt_top_fails = 0;
      mqtt_breaker = false;
      mqtt_connected_at = 0;   // reset once per connection
    }
    // Drain the send bridge (main-task context, so mesh calls are safe here).
    // Peek-then-receive: an over-budget message STAYS queued for a later pass
    // instead of being dequeued and lost.
    MqttSendMsg sm;
    while (mqtt_send_q && xQueuePeek(mqtt_send_q, &sm, 0) == pdTRUE) {
      if (!mqtt_send_limit.allow(millis() / 1000)) break;
      xQueueReceive(mqtt_send_q, &sm, 0);
      the_mesh.sendMqttChannelText(sm.channel, sm.text);
    }
    if (next_mqtt_pub == 0 || (long)(millis() - next_mqtt_pub) >= 0) {
      next_mqtt_pub = millis() + OBS_MQTT_INTERVAL_S * 1000UL;
      char t[104], payload[288];
      snprintf(t, sizeof(t), "%s/telemetry", mqtt_prefix);
      int n = the_mesh.observerFormatMqttTelemetry(payload, sizeof(payload));
      // enqueue(), not publish(): publish() blocks the caller on the socket for
      // up to network.timeout_ms, and this runs in the MAIN task -- a stalled
      // uplink would stall the mesh loop with it.
      if (n > 0 && n < (int)sizeof(payload))
        esp_mqtt_client_enqueue(mqtt_client, t, payload, n, 0, false, true);
      // All attached sensor readings (temperature/humidity/pressure/GPS/...),
      // same cadence. Empty "{}" on a node with no sensors -- still published
      // so a subscriber can tell "no sensors" from "node offline".
      char sensors_json[320];
      int sn = the_mesh.observerFormatSensorsJson(sensors_json, sizeof(sensors_json));
      if (sn > 0 && sn < (int)sizeof(sensors_json)) {
        snprintf(t, sizeof(t), "%s/sensors", mqtt_prefix);
        esp_mqtt_client_enqueue(mqtt_client, t, sensors_json, sn, 0, false, true);
      }
    }
    if (next_mqtt_contacts == 0 || (long)(millis() - next_mqtt_contacts) >= 0) {
      next_mqtt_contacts = millis() + OBS_MQTT_CONTACTS_INTERVAL_S * 1000UL;
      mqttPublishContacts();
    }
    if (next_mqtt_heard == 0 || (long)(millis() - next_mqtt_heard) >= 0) {
      next_mqtt_heard = millis() + OBS_MQTT_HEARD_INTERVAL_S * 1000UL;
      mqttPublishHeard();
    }
  }

  #ifdef WITH_OBSERVER_PROBE
  // Strong sinks for the observer prober (weak no-ops in ObserverProbeGlue.cpp).
  // Push-based: called from the probe's response hooks, so a repeater's health
  // and hop chain publish the moment they arrive. Retained + enqueue (non-block,
  // RX-context safe) so the broker always holds each repeater's latest state.
  extern "C" void observerProbePublish(const char* pk_hex, uint16_t batt_mv, int16_t temp_dc, uint32_t ok_epoch) {
    if (!mqtt_client || !mqtt_connected) return;
    char t[112], payload[160];
    snprintf(t, sizeof(t), "%s/repeater/%s/telemetry", mqtt_prefix, pk_hex);
    int n = snprintf(payload, sizeof(payload), "{\"batt_mv\":%u,\"ts\":%lu",
                     (unsigned)batt_mv, (unsigned long)ok_epoch);
    if (temp_dc != 0x7FFF)   // 0x7FFF sentinel = repeater reported no temperature
      n += snprintf(payload + n, sizeof(payload) - n, ",\"temp_c\":%.1f", (double)temp_dc / 10.0);
    n += snprintf(payload + n, sizeof(payload) - n, "}");
    if (n > 0 && n < (int)sizeof(payload))
      esp_mqtt_client_enqueue(mqtt_client, t, payload, n, 0, true, true);
  }
  extern "C" void observerProbePublishPath(const char* pk_hex, const char* hops_hex) {
    if (!mqtt_client || !mqtt_connected) return;
    char t[112], payload[160];
    snprintf(t, sizeof(t), "%s/repeater/%s/path", mqtt_prefix, pk_hex);
    int n = snprintf(payload, sizeof(payload), "{\"hops\":\"%s\"}", hops_hex);
    if (n > 0 && n < (int)sizeof(payload))
      esp_mqtt_client_enqueue(mqtt_client, t, payload, n, 0, true, true);
  }
  #endif
#elif defined(ESP32) && defined(WIFI_SSID)
  // Strong definition of the control-channel WiFi toggle (weak fallback in
  // BotCommands.cpp returns false on non-WiFi builds). Runtime-only by design:
  // a reboot restores WiFi, so a bad toggle can't permanently strand a node.
  extern "C" bool observerSetWifiControl(bool enable) {
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
#ifdef OBS_BOOT_TRACE
  delay(3000); Serial.println("[boot] serial up");
#endif

  board.begin();
#ifdef OBS_BOOT_TRACE
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

#ifdef OBS_BOOT_TRACE
  Serial.println("[boot] radio_init...");
#endif
  if (!radio_init()) {
#ifdef OBS_BOOT_TRACE
    Serial.println("[boot] RADIO INIT FAILED (SX1262 not responding) -- halting");
#endif
    halt();
  }
#ifdef OBS_BOOT_TRACE
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
#ifdef OBS_BOOT_TRACE
  Serial.println("[boot] spiffs...");
#endif
  SPIFFS.begin(true);
#ifdef OBS_BOOT_TRACE
  Serial.println("[boot] store...");
#endif
  store.begin();
#ifdef OBS_BOOT_TRACE
  Serial.println("[boot] mesh.begin...");
#endif
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#ifdef OBS_BOOT_TRACE
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
#ifdef OBS_BOOT_TRACE
    Serial.println("[boot] wifi.begin...");
#endif
    observerApplyWifi(the_mesh.getNodePrefs()->wifi_ssid, the_mesh.getNodePrefs()->wifi_pwd);
#ifdef OBS_BOOT_TRACE
    Serial.println("[boot] wifi started, tcp listening");
#endif
  }
  // MQTT publisher, if a broker was provisioned (`set mqtt_host`). Safe to
  // start before WiFi associates -- esp_mqtt retries in its own task.
  if (the_mesh.getNodePrefs()->mqtt_host[0]) {
    observerApplyMqtt();
  }
#elif defined(WIFI_SSID)
#ifdef OBS_BOOT_TRACE
  Serial.println("[boot] wifi.begin...");
#endif
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  wifiRegisterEvents();

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  serial_interface.begin(TCP_PORT);
#ifdef OBS_BOOT_TRACE
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
  #ifdef WITH_RUNTIME_WIFI
  mqttLoop();   // periodic telemetry publish (no-op unless broker connected)
  #endif
#endif
}
