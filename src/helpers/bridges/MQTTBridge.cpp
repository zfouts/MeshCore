#include "MQTTBridge.h"

#ifdef WITH_MQTT_BRIDGE

#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <sys/time.h>   // settimeofday

// First-boot defaults (optional); everything is overridable from the web UI.
#ifndef MQTT_BRIDGE_SSID
  #define MQTT_BRIDGE_SSID  ""
#endif
#ifndef MQTT_BRIDGE_PASS
  #define MQTT_BRIDGE_PASS  ""
#endif
#ifndef MQTT_BRIDGE_HOST
  #define MQTT_BRIDGE_HOST  ""
#endif
#ifndef MQTT_BRIDGE_PORT
  #define MQTT_BRIDGE_PORT  1883
#endif
#ifndef MQTT_BRIDGE_USER
  #define MQTT_BRIDGE_USER  ""
#endif
#ifndef MQTT_BRIDGE_PASSWORD
  #define MQTT_BRIDGE_PASSWORD  ""
#endif
#ifndef MQTT_BRIDGE_TOPIC
  #define MQTT_BRIDGE_TOPIC  "meshcore/bridge"
#endif
#ifndef MQTT_BRIDGE_AP_PASS
  #define MQTT_BRIDGE_AP_PASS  "meshbridge"   // >=8 => WPA2, "" => open
#endif
#ifndef HTTP_PORT
  #define HTTP_PORT 80
#endif

#define MQTT_BRIDGE_CFG_FILE   "/mqtt_bridge_cfg"
#define MQTT_BRIDGE_CFG_MAGIC  0x4D423033   // "MB03"

// Let's Encrypt root (ISRG Root X1) — used to verify TLS broker certs.
static const char ISRG_ROOT_X1[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)CERT";
#define MQTT_STA_TIMEOUT_MS    20000

// magic(2) + checksum(2) + serialized packet (<= MAX_TRANS_UNIT)
static const size_t MQTT_FRAME_MAX = BridgeBase::BRIDGE_MAGIC_SIZE +
                                     BridgeBase::BRIDGE_CHECKSUM_SIZE + MAX_TRANS_UNIT;

// The page is sent in two parts so the STA-mode handler can inject a Bootstrap
// CDN <link> between them (a client on the AP has no internet, so it gets only
// the self-contained fallback CSS below). Bootstrap-friendly class names are
// used so the fallback gives a clean "basic" look and Bootstrap, when loaded,
// overrides it for the "pretty" look. Shared JS is served from /app.js (no CDN).
static const char MQTT_BRIDGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>MeshBridge</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#f4f5f7;color:#222}
 .container{max-width:600px;margin:auto;padding:12px}
 h2{margin:6px 0}.card{background:#fff;border:1px solid #ddd;border-radius:8px;margin:12px 0}
 .card-body{padding:14px}h3{margin:0 0 6px}
 .form-label{display:block;font-size:13px;color:#567;margin:8px 0 3px}
 .form-control,.form-select{width:100%;padding:8px;border:1px solid #ccd;border-radius:6px;box-sizing:border-box}
 .btn{padding:8px 14px;border:0;border-radius:6px;cursor:pointer;font-weight:600;margin-top:8px}
 .btn-primary{background:#2c6cb0;color:#fff}.btn-secondary{background:#667;color:#fff}
 .text-muted{color:#789;font-size:12px}pre{white-space:pre-wrap;word-break:break-word;background:#f0f1f3;padding:8px;border-radius:6px}
</style>)HTML";

static const char MQTT_BRIDGE_BODY[] PROGMEM = R"HTML(</head><body>
<div class=container>
 <h2>🌉 MeshBridge <small id=hdr class=text-muted></small></h2>
 <div class=card><div class=card-body id=stat></div></div>
 <div class=card><div class=card-body><h3>WiFi</h3><form method=post action=/config>
  <label class=form-label>SSID</label><input class=form-control name=ssid id=ssid>
  <label class=form-label>Password (blank = keep)</label><input class=form-control type=password name=wpass>
  <input type=hidden name=action value=wifi><button class="btn btn-primary">Save &amp; connect</button></form>
  <p class=text-muted>No network? Hosts AP "MeshBridge-xxxx" so this page stays reachable.</p></div></div>
 <div class=card><div class=card-body><h3>MQTT</h3><form method=post action=/config>
  <label class=form-label>Broker host</label><input class=form-control name=host id=host>
  <label class=form-label>Port</label><input class=form-control name=port id=port type=number>
  <label class=form-label>Username (optional)</label><input class=form-control name=user id=user>
  <label class=form-label>Password (blank = keep)</label><input class=form-control type=password name=mpass>
  <label class=form-label>Bridge topic (same on both meshes)</label><input class=form-control name=topic id=topic>
  <label class=form-label><input type=checkbox name=tls id=tls value=1> TLS (verify via Let's Encrypt root)</label>
  <input type=hidden name=action value=mqtt><button class="btn btn-primary">Save MQTT</button></form></div></div>
 <div class=card><div class=card-body><h3>Repeater</h3><form method=post action=/config>
  <label class=form-label>Bridge packet source</label>
  <select class=form-select name=pktsrc id=pktsrc><option value=rx>logRx (forward heard packets)</option><option value=tx>logTx (forward retransmits)</option></select>
  <label class=form-label>Bridge secret (XOR isolation; blank = keep)</label><input class=form-control name=secret id=secret>
  <input type=hidden name=action value=repeater><button class="btn btn-primary">Save repeater</button></form></div></div>
 <div class=card><div class=card-body><h3>Console</h3>
  <input class=form-control id=cmd placeholder="set freq 915 / advert / get tx_power / password <new>">
  <button class="btn btn-secondary" onclick=runcli()>Send</button>
  <pre id=cliout></pre>
  <p class=text-muted>Runs any repeater CLI command. Multi-line outputs (neighbors, stats) appear on USB serial.</p></div></div>
</div>
<script src=/app.js></script></body></html>)HTML";

static const char MQTT_BRIDGE_JS[] PROGMEM = R"JS(
let filled=false;
function set(id,v){let e=document.getElementById(id);if(e&&document.activeElement!=e)e.value=v;}
async function runcli(){let c=document.getElementById('cmd').value;if(!c)return;
 let r=await fetch('/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c)});
 document.getElementById('cliout').textContent=r.ok?(await r.text()):('error '+r.status);}
async function tick(){let j;try{j=await(await fetch('/api.json')).json();}catch(e){return;}
 document.getElementById('hdr').textContent=j.wifi+(j.ip!='-'?(' '+j.ip):'');
 document.getElementById('stat').innerHTML=
  `wifi <b>${j.wifi}</b> ${j.ip}<br>mqtt <b>${j.mqtt?'connected':'down'}</b> ${j.host||'(unset)'}:${j.port}`+
  `<br>topic <b>${j.topic}</b> &middot; source <b>${j.pktsrc}</b>`;
 if(!filled){filled=true;set('ssid',j.ssid);set('host',j.host);set('port',j.port);set('user',j.user);
  set('topic',j.topic);set('secret',j.secret);let p=document.getElementById('pktsrc');if(p)p.value=j.pktsrc;
  let t=document.getElementById('tls');if(t)t.checked=j.tls;}}
tick();setInterval(tick,3000);
)JS";

MQTTBridge *MQTTBridge::_instance = nullptr;

void MQTTBridge::mqtt_cb(char *topic, uint8_t *payload, unsigned int len) {
  if (_instance) _instance->onMqttData(payload, len);
}

MQTTBridge::MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _mqtt(_net), _web(HTTP_PORT),
      _next_wifi_check(0), _sta_since(0), _next_mqtt_try(0),
      _ap_active(false), _web_started(false), _time_ok(false) {
  _instance = this;
  _client_id[0] = 0;
}

void MQTTBridge::cfgDefaults() {
  memset(&_cfg, 0, sizeof(_cfg));
  _cfg.magic = MQTT_BRIDGE_CFG_MAGIC;
  _cfg.mqtt_port = MQTT_BRIDGE_PORT;
  strncpy(_cfg.ssid,  MQTT_BRIDGE_SSID,     sizeof(_cfg.ssid)-1);
  strncpy(_cfg.pass,  MQTT_BRIDGE_PASS,     sizeof(_cfg.pass)-1);
  strncpy(_cfg.host,  MQTT_BRIDGE_HOST,     sizeof(_cfg.host)-1);
  strncpy(_cfg.user,  MQTT_BRIDGE_USER,     sizeof(_cfg.user)-1);
  strncpy(_cfg.mpass, MQTT_BRIDGE_PASSWORD, sizeof(_cfg.mpass)-1);
  strncpy(_cfg.topic, MQTT_BRIDGE_TOPIC,    sizeof(_cfg.topic)-1);
  _cfg.wifi_enabled = _cfg.ssid[0] ? 1 : 0;
  _cfg.web_enabled = 1;
}
void MQTTBridge::loadCfg() {
  cfgDefaults();
  if (SPIFFS.exists(MQTT_BRIDGE_CFG_FILE)) {
    File f = SPIFFS.open(MQTT_BRIDGE_CFG_FILE);
    if (f) {
      MqttBridgeCfg tmp; size_t n = f.read((uint8_t*)&tmp, sizeof(tmp)); f.close();
      if (n == sizeof(tmp) && tmp.magic == MQTT_BRIDGE_CFG_MAGIC) _cfg = tmp;
    }
  }
  if (_cfg.topic[0] == 0) strncpy(_cfg.topic, MQTT_BRIDGE_TOPIC, sizeof(_cfg.topic)-1);
}
void MQTTBridge::saveCfg() {
  File f = SPIFFS.open(MQTT_BRIDGE_CFG_FILE, "w", true);
  if (f) { f.write((const uint8_t*)&_cfg, sizeof(_cfg)); f.close(); }
}

void MQTTBridge::begin() {
  SPIFFS.begin(true);
  loadCfg();

  uint64_t mac = ESP.getEfuseMac();
  snprintf(_client_id, sizeof(_client_id), "meshbridge-%04x%08x",
           (uint16_t)(mac >> 32), (uint32_t)mac);

  _mqtt.setBufferSize(MQTT_FRAME_MAX + 16);
  _mqtt.setSocketTimeout(6);   // keep a failed broker from freezing the web UI
  _mqtt.setCallback(mqtt_cb);
  applyMqttClient();

  if (wifiOk()) { WiFi.mode(WIFI_STA); WiFi.begin(_cfg.ssid, _cfg.pass); }

  _initialized = true;
  BRIDGE_DEBUG_PRINTLN("MQTT bridge up: ssid=%s host=%s tls=%d topic=%s\n",
                       _cfg.ssid, _cfg.host, _cfg.tls_enabled, _cfg.topic);
}

// Point PubSubClient at the plain or TLS transport per config.
void MQTTBridge::applyMqttClient() {
  if (_cfg.tls_enabled) {
    _tls.setCACert(ISRG_ROOT_X1);   // verify broker against Let's Encrypt root
    _tls.setTimeout(6);
    _mqtt.setClient(_tls);
  } else {
    _mqtt.setClient(_net);
  }
  if (_cfg.host[0]) _mqtt.setServer(_cfg.host, _cfg.mqtt_port);
}

// TLS cert validity needs a real clock. Use MeshCore's own time source — the
// mesh RTC, which is set from GPS (or `gps sync` / advert clock-sync). Push it
// into the system clock that mbedTLS reads. No NTP/internet time needed.
void MQTTBridge::clockFromRtc() {
  uint32_t t = _rtc ? _rtc->getCurrentTime() : 0;
  if (t > 1577836800UL) {            // >= 2020 => a plausibly-real (synced) clock
    struct timeval tv = { (time_t)t, 0 };
    settimeofday(&tv, nullptr);
    _time_ok = true;
  }
}

void MQTTBridge::end() {
  _mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  _initialized = false;
}

void MQTTBridge::startAP() {
  uint64_t mac = ESP.getEfuseMac();
  char ss[28]; snprintf(ss, sizeof(ss), "MeshBridge-%04x", (uint16_t)mac);
  WiFi.mode(WIFI_AP_STA);
  const char* pw = MQTT_BRIDGE_AP_PASS;
  if (pw && strlen(pw) >= 8) WiFi.softAP(ss, pw); else WiFi.softAP(ss);
  _ap_active = true;
  BRIDGE_DEBUG_PRINTLN("AP up: %s\n", ss);
}

void MQTTBridge::connectMqtt() {
  if (_cfg.host[0] == 0) return;
  if (_cfg.tls_enabled && !_time_ok) return;   // wait for NTP so cert dates validate
  bool ok = _cfg.user[0] ? _mqtt.connect(_client_id, _cfg.user, _cfg.mpass)
                         : _mqtt.connect(_client_id);
  if (ok) { _mqtt.subscribe(_cfg.topic); BRIDGE_DEBUG_PRINTLN("MQTT connected, sub %s\n", _cfg.topic); }
  else BRIDGE_DEBUG_PRINTLN("MQTT connect failed rc=%d\n", _mqtt.state());
}

void MQTTBridge::netLoop() {
  unsigned long now = millis();
  if (now >= _next_wifi_check) {
    _next_wifi_check = now + 3000;
    if (WiFi.status() == WL_CONNECTED) {
      _sta_since = 0;
    } else if (!_ap_active) {
      if (wifiOk()) {
        if (_sta_since == 0) { _sta_since = now; WiFi.mode(WIFI_STA); WiFi.begin(_cfg.ssid, _cfg.pass); }
        else if (now - _sta_since > MQTT_STA_TIMEOUT_MS) startAP();
      } else startAP();
    }
    clockFromRtc();   // align system clock with the GPS-fed mesh RTC (for TLS)
  }
  if (_cfg.host[0] && WiFi.status() == WL_CONNECTED) {
    if (!_mqtt.connected() && now >= _next_mqtt_try) { _next_mqtt_try = now + 8000; connectMqtt(); }
    if (_mqtt.connected()) _mqtt.loop();
  }
}

bool MQTTBridge::requireAuth() {
  const char* pw = _prefs->password;
  if (pw[0] == 0) return true;   // no admin password set -> leave open
  if (!_web.authenticate("admin", pw)) { _web.requestAuthentication(); return false; }
  return true;
}

void MQTTBridge::handleCli() {
  if (!requireAuth()) return;
  if (!_cli_fn) { _web.send(503, "text/plain", "cli unavailable"); return; }
  char cmd[160];
  strncpy(cmd, _web.arg("cmd").c_str(), sizeof(cmd) - 1); cmd[sizeof(cmd) - 1] = 0;
  char reply[256]; reply[0] = 0;
  _cli_fn(_cli_ctx, cmd, reply, sizeof(reply));
  _web.send(200, "text/plain", reply[0] ? reply : "(ok)");
}

void MQTTBridge::startWeb() {
  _web.on("/", [this]() {
    if (!requireAuth()) return;
    _web.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _web.send(200, "text/html; charset=utf-8", "");
    _web.sendContent_P(MQTT_BRIDGE_HEAD);
    if (WiFi.status() == WL_CONNECTED)   // STA has internet -> pretty (Bootstrap CDN)
      _web.sendContent(F("<link href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css\" rel=stylesheet>"));
    _web.sendContent_P(MQTT_BRIDGE_BODY);
    _web.sendContent("");
  });
  _web.on("/app.js", [this]() { _web.send_P(200, "application/javascript", MQTT_BRIDGE_JS); });
  _web.on("/api.json", [this]() { if (!requireAuth()) return; handleApi(); });
  _web.on("/config", HTTP_POST, [this]() { if (!requireAuth()) return; handleConfig(); });
  _web.on("/cli", HTTP_POST, [this]() { handleCli(); });
  _web.onNotFound([this]() { _web.send(404, "text/plain", "not found\n"); });
  _web.begin();
  if (MDNS.begin("meshbridge")) MDNS.addService("http", "tcp", HTTP_PORT);
  _web_started = true;
  IPAddress ip = (WiFi.status()==WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
  BRIDGE_DEBUG_PRINTLN("web up http://%s/\n", ip.toString().c_str());
}

void MQTTBridge::handleApi() {
  bool up = (WiFi.status() == WL_CONNECTED);
  char buf[420];
  snprintf(buf, sizeof(buf),
    "{\"wifi\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"mqtt\":%s,\"host\":\"%s\",\"port\":%u,"
    "\"user\":\"%s\",\"topic\":\"%s\",\"tls\":%s,\"pktsrc\":\"%s\",\"secret\":\"%s\"}",
    up?"connected":(_ap_active?"AP":"down"),
    up?WiFi.localIP().toString().c_str():(_ap_active?WiFi.softAPIP().toString().c_str():"-"),
    _cfg.ssid, _mqtt.connected()?"true":"false", _cfg.host, _cfg.mqtt_port, _cfg.user, _cfg.topic,
    _cfg.tls_enabled?"true":"false", _prefs->bridge_pkt_src ? "rx" : "tx", _prefs->bridge_secret);
  _web.send(200, "application/json", buf);
}

void MQTTBridge::handleConfig() {
  String a = _web.arg("action");
  if (a == "wifi") {
    strncpy(_cfg.ssid, _web.arg("ssid").c_str(), sizeof(_cfg.ssid)-1); _cfg.ssid[sizeof(_cfg.ssid)-1]=0;
    if (_web.arg("wpass").length()) { strncpy(_cfg.pass, _web.arg("wpass").c_str(), sizeof(_cfg.pass)-1); _cfg.pass[sizeof(_cfg.pass)-1]=0; }
    _cfg.wifi_enabled = 1; saveCfg();
    _ap_active = false; _sta_since = 0;
    WiFi.disconnect(); WiFi.mode(WIFI_STA); WiFi.begin(_cfg.ssid, _cfg.pass);
  } else if (a == "mqtt") {
    strncpy(_cfg.host, _web.arg("host").c_str(), sizeof(_cfg.host)-1); _cfg.host[sizeof(_cfg.host)-1]=0;
    if (_web.arg("port").length()) _cfg.mqtt_port = (uint16_t)_web.arg("port").toInt();
    strncpy(_cfg.user, _web.arg("user").c_str(), sizeof(_cfg.user)-1); _cfg.user[sizeof(_cfg.user)-1]=0;
    if (_web.arg("mpass").length()) { strncpy(_cfg.mpass, _web.arg("mpass").c_str(), sizeof(_cfg.mpass)-1); _cfg.mpass[sizeof(_cfg.mpass)-1]=0; }
    if (_web.arg("topic").length()) { strncpy(_cfg.topic, _web.arg("topic").c_str(), sizeof(_cfg.topic)-1); _cfg.topic[sizeof(_cfg.topic)-1]=0; }
    _cfg.tls_enabled = _web.hasArg("tls") ? 1 : 0;
    saveCfg();
    applyMqttClient();
    _mqtt.disconnect(); _next_mqtt_try = 0;   // reconnect with new transport/topic
  } else if (a == "repeater" && _cli_fn) {
    char cmd[80], reply[200];
    String src = _web.arg("pktsrc");
    if (src.length()) { snprintf(cmd, sizeof(cmd), "set bridge.source %s", src.c_str()); _cli_fn(_cli_ctx, cmd, reply, sizeof(reply)); }
    String sec = _web.arg("secret");
    if (sec.length()) { snprintf(cmd, sizeof(cmd), "set bridge.secret %s", sec.c_str()); _cli_fn(_cli_ctx, cmd, reply, sizeof(reply)); }
  }
  _web.sendHeader("Location", "/");
  _web.send(303, "text/plain", "ok");
}

void MQTTBridge::loop() {
  if (!_initialized) return;
  netLoop();
  if (_cfg.web_enabled && netUp() && !_web_started) startWeb();
  if (_web_started) _web.handleClient();
}

// Toggle the web UI at runtime (CLI: `set bridge.web on|off`), persisted.
void MQTTBridge::setWebEnabled(bool enable) {
  _cfg.web_enabled = enable ? 1 : 0;
  saveCfg();
  if (enable) {
    if (netUp() && !_web_started) startWeb();
  } else if (_web_started) {
    _web.stop();
    MDNS.end();
    _web_started = false;
    BRIDGE_DEBUG_PRINTLN("web server stopped\n");
  }
}

void MQTTBridge::xorCrypt(uint8_t *data, size_t len) {
  size_t keyLen = strlen(_prefs->bridge_secret);
  if (keyLen == 0) return;
  for (size_t i = 0; i < len; i++) data[i] ^= _prefs->bridge_secret[i % keyLen];
}

// Mesh -> MQTT
void MQTTBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !packet || !_mqtt.connected()) return;
  if (_seen_packets.hasSeen(packet)) return;   // loop guard

  uint8_t mp[MAX_TRANS_UNIT];
  uint8_t n = packet->writeTo(mp);
  if (n == 0) return;

  uint8_t buf[MQTT_FRAME_MAX];
  buf[0] = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF;
  buf[1] = BRIDGE_PACKET_MAGIC & 0xFF;
  memcpy(buf + BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE, mp, n);
  uint16_t cs = fletcher16(buf + BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE, n);
  buf[2] = (cs >> 8) & 0xFF; buf[3] = cs & 0xFF;
  xorCrypt(buf + BRIDGE_MAGIC_SIZE, n + BRIDGE_CHECKSUM_SIZE);

  size_t total = BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE + n;
  if (_mqtt.publish(_cfg.topic, buf, total)) BRIDGE_DEBUG_PRINTLN("MQTT TX len=%d\n", n);
}

// MQTT -> Mesh
void MQTTBridge::onMqttData(uint8_t *data, unsigned int len) {
  if (len < (BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE) || len > MQTT_FRAME_MAX) return;
  uint16_t magic = (data[0] << 8) | data[1];
  if (magic != BRIDGE_PACKET_MAGIC) return;

  uint8_t dec[MQTT_FRAME_MAX];
  size_t encLen = len - BRIDGE_MAGIC_SIZE;
  memcpy(dec, data + BRIDGE_MAGIC_SIZE, encLen);
  xorCrypt(dec, encLen);

  uint16_t rcs = (dec[0] << 8) | dec[1];
  size_t plen = encLen - BRIDGE_CHECKSUM_SIZE;
  if (!validateChecksum(dec + BRIDGE_CHECKSUM_SIZE, plen, rcs)) return;

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) return;
  if (pkt->readFrom(dec + BRIDGE_CHECKSUM_SIZE, (uint8_t)plen)) {
    BRIDGE_DEBUG_PRINTLN("MQTT RX len=%d\n", plen);
    onPacketReceived(pkt);
  } else _mgr->free(pkt);
}

void MQTTBridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}

#endif
