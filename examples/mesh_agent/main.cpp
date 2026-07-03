#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#if defined(ESP32) && defined(WITH_WIFI)
  #include <WiFi.h>
  #include <PubSubClient.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/BaseChatMesh.h>
#include <RTClib.h>
#include <target.h>

/* ============================ MeshCore MQTT Agent ===========================
 *
 * Thin mesh <-> MQTT bridge. The node stays dumb: it joins WiFi (or hosts its
 * own AP if it can't), connects to an MQTT broker, publishes every advert it
 * hears (with RF metadata + a capture-time stamp), emits a periodic heartbeat,
 * and subscribes to a command topic. All analysis / dashboards / scheduling
 * live server-side, on whatever consumes the MQTT stream.
 *
 * Configure WiFi + MQTT from the built-in web UI (http://meshagent.local/ or the
 * AP at 192.168.4.1) or the serial CLI.
 * ========================================================================== */

#ifndef LORA_FREQ
  #define LORA_FREQ   915.0
#endif
#ifndef LORA_BW
  #define LORA_BW     250
#endif
#ifndef LORA_SF
  #define LORA_SF     10
#endif
#ifndef LORA_CR
  #define LORA_CR      5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER  20
#endif

#ifndef AGENT_DEFAULT_BASE
  #define AGENT_DEFAULT_BASE  "meshcore"
#endif
#ifndef AGENT_DEFAULT_MQTT_PORT
  #define AGENT_DEFAULT_MQTT_PORT  1883
#endif
#ifndef AGENT_HEARTBEAT_SECS
  #define AGENT_HEARTBEAT_SECS  30
#endif
#ifndef AGENT_AP_PASS
  #define AGENT_AP_PASS  "meshagent"   // >=8 => WPA2, "" => open
#endif
#ifndef AGENT_STA_TIMEOUT_MS
  #define AGENT_STA_TIMEOUT_MS  20000
#endif
#ifndef AGENT_JSON_MAX
  #define AGENT_JSON_MAX  320
#endif

#define AGENT_CFG_FILE   "/agent_cfg"
#define AGENT_CFG_MAGIC  0x4D413031   // "MA01"

#define SEND_TIMEOUT_BASE_MILLIS          500
#define FLOOD_SEND_TIMEOUT_FACTOR         16.0f
#define DIRECT_SEND_PERHOP_FACTOR         6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS   250

struct AgentCfg {
  uint32_t magic;
  uint8_t  wifi_enabled;
  uint8_t  mqtt_enabled;
  uint16_t mqtt_port;
  char     ssid[33];
  char     pass[64];
  char     mqtt_host[64];
  char     mqtt_user[33];
  char     mqtt_pass[64];
  char     base_topic[48];
};

#if defined(ESP32) && defined(WITH_WIFI)
  static WiFiClient   wifi_client;
  static PubSubClient mqtt(wifi_client);
#endif

static const char* advTypeName(uint8_t t) {
  switch (t) {
    case ADV_TYPE_REPEATER: return "Repeater";
    case ADV_TYPE_CHAT:     return "Companion";
    case ADV_TYPE_ROOM:     return "Room";
    case ADV_TYPE_SENSOR:   return "Sensor";
    default:                return "Node";
  }
}
static void hexEncode(const uint8_t* s, int n, char* o) {
  static const char* H = "0123456789abcdef";
  for (int i = 0; i < n; i++) { o[i*2] = H[s[i] >> 4]; o[i*2+1] = H[s[i] & 0xF]; }
  o[n*2] = 0;
}

class MeshAgent : public BaseChatMesh {
  FILESYSTEM* _fs;
  char _nodeid[9];                    // hex of first 4 pubkey bytes
  AgentCfg _cfg;

  uint32_t _rx_count, _pub_count, _cmd_count;

#if defined(ESP32) && defined(WITH_WIFI)
  unsigned long _next_wifi_check, _sta_since, _next_mqtt_try, _next_beat;
  bool _ap_active;
#endif
  char command[200];

  // -------- config --------
  void cfgDefaults() {
    memset(&_cfg, 0, sizeof(_cfg));
    _cfg.magic = AGENT_CFG_MAGIC;
    _cfg.mqtt_port = AGENT_DEFAULT_MQTT_PORT;
    strncpy(_cfg.base_topic, AGENT_DEFAULT_BASE, sizeof(_cfg.base_topic) - 1);
  }
  void loadCfg() {
    cfgDefaults();
    if (_fs->exists(AGENT_CFG_FILE)) {
    #if defined(RP2040_PLATFORM)
      File f = _fs->open(AGENT_CFG_FILE, "r");
    #else
      File f = _fs->open(AGENT_CFG_FILE);
    #endif
      if (f) {
        AgentCfg tmp; size_t n = f.read((uint8_t*)&tmp, sizeof(tmp)); f.close();
        if (n == sizeof(tmp) && tmp.magic == AGENT_CFG_MAGIC) _cfg = tmp;
      }
    }
    if (_cfg.base_topic[0] == 0) strncpy(_cfg.base_topic, AGENT_DEFAULT_BASE, sizeof(_cfg.base_topic) - 1);
  }
  void saveCfg() {
  #if defined(ESP32)
    File f = _fs->open(AGENT_CFG_FILE, "w", true);
  #elif defined(NRF52_PLATFORM)
    _fs->remove(AGENT_CFG_FILE); File f = _fs->open(AGENT_CFG_FILE, FILE_O_WRITE);
  #else
    File f = _fs->open(AGENT_CFG_FILE, "w");
  #endif
    if (f) { f.write((const uint8_t*)&_cfg, sizeof(_cfg)); f.close(); }
  }

#if defined(ESP32) && defined(WITH_WIFI)
  void topic(char* dst, size_t sz, const char* leaf) {
    snprintf(dst, sz, "%s/%s/%s", _cfg.base_topic, _nodeid, leaf);
  }
  void publishBeat() {
    if (!mqtt.connected()) return;
    char t[80]; topic(t, sizeof(t), "status");
    char j[200];
    snprintf(j, sizeof(j),
      "{\"id\":\"%s\",\"up\":%lu,\"heap\":%u,\"rx\":%u,\"pub\":%u,\"ip\":\"%s\"}",
      _nodeid, millis()/1000UL, (unsigned)ESP.getFreeHeap(), _rx_count, _pub_count,
      WiFi.localIP().toString().c_str());
    mqtt.publish(t, j);
  }
  void mqttConnect() {
    char cid[32]; snprintf(cid, sizeof(cid), "meshagent-%s", _nodeid);
    bool ok = _cfg.mqtt_user[0] ? mqtt.connect(cid, _cfg.mqtt_user, _cfg.mqtt_pass)
                                : mqtt.connect(cid);
    if (ok) {
      char t[80];
      topic(t, sizeof(t), "cmd/#"); mqtt.subscribe(t);
      snprintf(t, sizeof(t), "%s/all/cmd/#", _cfg.base_topic); mqtt.subscribe(t);
      Serial.printf("[mqtt] connected, subscribed %s/%s/cmd/#\n", _cfg.base_topic, _nodeid);
      publishBeat();
    }
  }
#endif

  void handleCommand(char* cmd) {
  #if defined(ESP32) && defined(WITH_WIFI)
    if (memcmp(cmd, "wifi ", 5) == 0) {          // wifi <ssid> <pass>
      char* sp = strchr(cmd + 5, ' ');
      if (sp) { *sp = 0; setWifi(cmd + 5, sp + 1); } else setWifi(cmd + 5, "");
      Serial.println("OK wifi set, connecting"); return;
    }
    if (memcmp(cmd, "mqtt ", 5) == 0) {          // mqtt <host> [port]
      char* sp = strchr(cmd + 5, ' '); int port = 0;
      if (sp) { *sp = 0; port = atoi(sp + 1); }
      setMqtt(cmd + 5, port, "", ""); Serial.println("OK mqtt set"); return;
    }
    if (memcmp(cmd, "base ", 5) == 0) { setBaseTopic(cmd + 5); Serial.println("OK base set"); return; }
  #endif
    if (strcmp(cmd, "stats") == 0) {
    #if defined(ESP32) && defined(WITH_WIFI)
      Serial.printf("id=%s rx=%u pub=%u cmd=%u wifi=%s mqtt=%s\n", _nodeid, _rx_count, _pub_count, _cmd_count,
        WiFi.status()==WL_CONNECTED?"up":(_ap_active?"AP":"down"), mqtt.connected()?"up":"down");
    #else
      Serial.printf("id=%s rx=%u pub=%u\n", _nodeid, _rx_count, _pub_count);
    #endif
    } else if (strcmp(cmd, "help") == 0) {
      Serial.println("cmds: wifi <ssid> <pass>  mqtt <host> [port]  base <topic>  stats");
    } else if (cmd[0]) {
      Serial.println("? (try 'help')");
    }
  }

protected:
  // Publish every advert we hear; never relay or reply.
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                    const uint8_t* app_data, size_t app_data_len) override {
    _rx_count++;
  #if defined(ESP32) && defined(WITH_WIFI)
    if (mqtt.connected()) {
      float snr = packet->getSNR();
      float rssi = radio_driver.getLastRSSI();
      uint8_t hops = packet->path_len & 0x3F;
      AdvertDataParser p(app_data, app_data_len);
      char pk[13]; hexEncode(id.pub_key, 6, pk);
      char nm[40]; { const char* s = p.hasName() ? p.getName() : ""; size_t j=0;
        for (size_t i=0; s[i] && j<sizeof(nm)-2; i++){ char c=s[i];
          if(c=='"'||c=='\\'){nm[j++]='\\'; if(j<sizeof(nm)-1)nm[j++]=c;} else if((uint8_t)c>=0x20)nm[j++]=c; } nm[j]=0; }
      char j[AGENT_JSON_MAX];
      int k = snprintf(j, sizeof(j),
        "{\"id\":\"%s\",\"ms\":%lu,\"t\":%u,\"type\":\"%s\",\"pk\":\"%s\",\"name\":\"%s\",\"snr\":%.1f,\"rssi\":%.0f,\"hops\":%u",
        _nodeid, millis(), (unsigned)getRTCClock()->getCurrentTime(), advTypeName(p.getType()),
        pk, nm, snr, rssi, hops);
      if (p.hasLatLon() && k>0 && k<(int)sizeof(j)-40)
        k += snprintf(j+k, sizeof(j)-k, ",\"lat\":%.6f,\"lon\":%.6f", p.getLat(), p.getLon());
      if (k>0 && k<(int)sizeof(j)-2) strcpy(j+k, "}");
      char tp[80]; topic(tp, sizeof(tp), "rx");
      if (mqtt.publish(tp, j)) _pub_count++;
    }
  #endif
    BaseChatMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);
  }

  bool allowPacketForward(const mesh::Packet*) override { return false; }
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
  void onContactPathUpdated(const ContactInfo&) override {}
  ContactInfo* processAck(const uint8_t*) override { return NULL; }
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  void onSendTimeout() override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t a) const override {
    return SEND_TIMEOUT_BASE_MILLIS + (FLOOD_SEND_TIMEOUT_FACTOR * a);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t a, uint8_t pl) const override {
    uint8_t h = pl & 63;
    return SEND_TIMEOUT_BASE_MILLIS + ((a*DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS)*(h+1));
  }

public:
  MeshAgent(mesh::Radio& radio, StdRNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables) {
    _nodeid[0] = 0; _rx_count = _pub_count = _cmd_count = 0; command[0] = 0;
    cfgDefaults();
  #if defined(ESP32) && defined(WITH_WIFI)
    _next_wifi_check = _sta_since = _next_mqtt_try = _next_beat = 0; _ap_active = false;
  #endif
  }

  float  getFreqPref()    const { return LORA_FREQ; }
  int8_t getTxPowerPref() const { return LORA_TX_POWER; }

  void begin(FILESYSTEM& fs) {
    _fs = &fs;
    BaseChatMesh::begin();
    loadCfg();

  #if defined(NRF52_PLATFORM)
    IdentityStore store(fs, "");
  #else
    IdentityStore store(fs, "/identity");
  #endif
  #if defined(RP2040_PLATFORM)
    store.begin();
  #endif
    if (!store.load("_main", self_id)) {
      self_id = mesh::LocalIdentity(getRNG());
      store.save("_main", self_id);
    }
    hexEncode(self_id.pub_key, 4, _nodeid);

  #if defined(ESP32) && defined(WITH_WIFI)
    if (wifiEnabled()) { WiFi.mode(WIFI_STA); WiFi.begin(_cfg.ssid, _cfg.pass); }
    mqtt.setBufferSize(AGENT_JSON_MAX + 64);
    if (_cfg.mqtt_host[0]) mqtt.setServer(_cfg.mqtt_host, _cfg.mqtt_port);
  #endif
  }

  void showWelcome() {
    Serial.println("===== MeshCore MQTT Agent =====");
    Serial.printf("node id: %s   base topic: %s\n", _nodeid, _cfg.base_topic);
  #if defined(ESP32) && defined(WITH_WIFI)
    Serial.printf("wifi: %s   mqtt: %s:%u\n", wifiEnabled()?_cfg.ssid:"(unset)",
                  _cfg.mqtt_host[0]?_cfg.mqtt_host:"(unset)", _cfg.mqtt_port);
    Serial.println("configure at the web UI (AP 'MeshAgent-xxxx' if no wifi). 'help' for CLI.");
  #endif
  }

  void onMqttMessage(char* tp, uint8_t* payload, unsigned len) {
    _cmd_count++;
    char buf[160]; unsigned n = len < sizeof(buf)-1 ? len : sizeof(buf)-1;
    memcpy(buf, payload, n); buf[n] = 0;
    Serial.printf("[cmd] %s -> %s\n", tp, buf);
    // (placeholder: server commands handled here later)
  }

  // ---- web/CLI accessors ----
  const char* nodeId()   const { return _nodeid; }
  const char* baseTopic()const { return _cfg.base_topic; }
  uint32_t rxCount()     const { return _rx_count; }
  uint32_t pubCount()    const { return _pub_count; }
  uint32_t cmdCount()    const { return _cmd_count; }
#if defined(ESP32) && defined(WITH_WIFI)
  const char* ssid()     const { return _cfg.ssid; }
  const char* mqttHost() const { return _cfg.mqtt_host; }
  uint16_t mqttPort()    const { return _cfg.mqtt_port; }
  const char* mqttUser() const { return _cfg.mqtt_user; }
  bool wifiEnabled()     const { return _cfg.wifi_enabled && _cfg.ssid[0]; }
  bool mqttEnabled()     const { return _cfg.mqtt_host[0]; }
  bool apActive()        const { return _ap_active; }
  bool netUp()           const { return (WiFi.status()==WL_CONNECTED) || _ap_active; }
  bool mqttUp()          { return mqtt.connected(); }

  void setWifi(const char* ssid, const char* pass) {
    strncpy(_cfg.ssid, ssid, sizeof(_cfg.ssid)-1); _cfg.ssid[sizeof(_cfg.ssid)-1]=0;
    if (pass && pass[0]) { strncpy(_cfg.pass, pass, sizeof(_cfg.pass)-1); _cfg.pass[sizeof(_cfg.pass)-1]=0; }
    _cfg.wifi_enabled = 1; saveCfg();
    _ap_active = false; _sta_since = 0;
    WiFi.disconnect(); WiFi.mode(WIFI_STA); WiFi.begin(_cfg.ssid, _cfg.pass);
  }
  void setMqtt(const char* host, int port, const char* user, const char* pass) {
    strncpy(_cfg.mqtt_host, host, sizeof(_cfg.mqtt_host)-1); _cfg.mqtt_host[sizeof(_cfg.mqtt_host)-1]=0;
    if (port > 0) _cfg.mqtt_port = (uint16_t)port;
    strncpy(_cfg.mqtt_user, user, sizeof(_cfg.mqtt_user)-1); _cfg.mqtt_user[sizeof(_cfg.mqtt_user)-1]=0;
    if (pass && pass[0]) { strncpy(_cfg.mqtt_pass, pass, sizeof(_cfg.mqtt_pass)-1); _cfg.mqtt_pass[sizeof(_cfg.mqtt_pass)-1]=0; }
    saveCfg();
    if (_cfg.mqtt_host[0]) mqtt.setServer(_cfg.mqtt_host, _cfg.mqtt_port);
    mqtt.disconnect(); _next_mqtt_try = 0;
  }
  void setBaseTopic(const char* t) {
    strncpy(_cfg.base_topic, t, sizeof(_cfg.base_topic)-1); _cfg.base_topic[sizeof(_cfg.base_topic)-1]=0;
    saveCfg();
  }

  void startAP() {
    char ss[28]; snprintf(ss, sizeof(ss), "MeshAgent-%s", _nodeid);
    WiFi.mode(WIFI_AP_STA);
    const char* pw = AGENT_AP_PASS;
    if (pw && strlen(pw) >= 8) WiFi.softAP(ss, pw); else WiFi.softAP(ss);
    _ap_active = true;
    Serial.printf("[wifi] AP up: %s http://%s/\n", ss, WiFi.softAPIP().toString().c_str());
  }
  void netLoop() {
    unsigned long now = millis();
    if (now >= _next_wifi_check) {
      _next_wifi_check = now + 3000;
      if (WiFi.status() == WL_CONNECTED) _sta_since = 0;
      else if (!_ap_active) {
        if (wifiEnabled()) {
          if (_sta_since == 0) { _sta_since = now; WiFi.mode(WIFI_STA); WiFi.begin(_cfg.ssid, _cfg.pass); }
          else if (now - _sta_since > AGENT_STA_TIMEOUT_MS) startAP();
        } else startAP();
      }
    }
    if (_cfg.mqtt_host[0] && WiFi.status() == WL_CONNECTED) {
      if (!mqtt.connected() && now >= _next_mqtt_try) { _next_mqtt_try = now + 5000; mqttConnect(); }
      if (mqtt.connected()) {
        mqtt.loop();
        if (now >= _next_beat) { _next_beat = now + (unsigned long)AGENT_HEARTBEAT_SECS*1000UL; publishBeat(); }
      }
    }
  }
#endif

  void loop() {
    BaseChatMesh::loop();
  #if defined(ESP32) && defined(WITH_WIFI)
    netLoop();
  #endif
    int len = strlen(command);
    while (Serial.available() && len < (int)sizeof(command) - 1) {
      char c = Serial.read();
      if (c != '\n' && c != '\r') { command[len++] = c; command[len] = 0; }
      else if (len > 0) { handleCommand(command); command[0] = 0; len = 0; }
    }
  }
};

StdRNG fast_rng;
SimpleMeshTables tables;
MeshAgent the_mesh(radio_driver, fast_rng, rtc_clock, tables);

#if defined(ESP32) && defined(WITH_WIFI)
  #include <ESPmDNS.h>
  #include "AgentWeb.h"
  AgentWeb web(the_mesh);
  static bool web_started = false;
  void mqtt_cb(char* topic, uint8_t* payload, unsigned int len) { the_mesh.onMqttMessage(topic, payload, len); }
#endif

static void halt() { while (1) ; }

void setup() {
  Serial.begin(115200);
  board.begin();
  if (!radio_init()) { halt(); }
  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM)
  InternalFS.begin();  the_mesh.begin(InternalFS);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();    the_mesh.begin(LittleFS);
#elif defined(ESP32)
  SPIFFS.begin(true);  the_mesh.begin(SPIFFS);
#else
  #error "need to define filesystem"
#endif

  radio_driver.setParams(the_mesh.getFreqPref(), LORA_BW, LORA_SF, LORA_CR);
  radio_driver.setTxPower(the_mesh.getTxPowerPref());

#if defined(ESP32) && defined(WITH_WIFI)
  mqtt.setCallback(mqtt_cb);
#endif
  the_mesh.showWelcome();
}

void loop() {
  the_mesh.loop();
  rtc_clock.tick();

#if defined(ESP32) && defined(WITH_WIFI)
  if (the_mesh.netUp()) {
    if (!web_started) {
      web.begin();
      if (MDNS.begin("meshagent")) MDNS.addService("http", "tcp", HTTP_PORT);
      IPAddress ip = (WiFi.status()==WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
      Serial.printf("[web] http://%s/ (or http://meshagent.local/)\n", ip.toString().c_str());
      web_started = true;
    }
    web.loop();
  }
#endif
}
