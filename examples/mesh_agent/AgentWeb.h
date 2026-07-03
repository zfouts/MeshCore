#pragma once

// Basic config web UI for the MQTT agent: set WiFi (SSID/pass) and MQTT
// (host/port/user/pass) + base topic, see live status. Must be included AFTER
// MeshAgent is defined.

#if defined(ESP32) && defined(WITH_WIFI)

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#ifndef HTTP_PORT
  #define HTTP_PORT 80
#endif

static const char AGENT_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>MeshAgent</title><style>
 body{font-family:system-ui,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}
 header{background:#16202a;padding:10px 14px;font-weight:600}
 .wrap{padding:12px;max-width:560px;margin:auto}
 .card{background:#161c24;border-radius:8px;padding:12px;margin:10px 0}
 .stat{font-size:14px;line-height:1.7}.stat b{color:#5cd6c0}
 label{display:block;font-size:13px;color:#8aa;margin:8px 0 3px}
 input,button{font:inherit;padding:8px;border-radius:6px;border:1px solid #2a3340;background:#1e2630;color:#eee;box-sizing:border-box}
 input{width:100%}input[type=number]{width:110px}
 button{background:#2c8a78;border:0;cursor:pointer;font-weight:600;margin-top:8px}
 h3{margin:2px 0 4px}.ok{color:#5cd6c0}.bad{color:#e0795b}.muted{color:#789;font-size:12px}
</style></head><body>
<header>🛰 MeshAgent <span id=hdr class=muted></span></header>
<div class=wrap>
 <div class=card><div class=stat id=stat></div></div>

 <div class=card><h3>WiFi</h3><form method=post action=/config>
  <label>SSID</label><input name=ssid id=ssid>
  <label>Password (blank = keep)</label><input name=wpass type=password>
  <input type=hidden name=action value=wifi><button>Save &amp; connect</button></form>
  <p class=muted>No network? It hosts AP "MeshAgent-xxxx" so this page stays reachable.</p>
 </div>

 <div class=card><h3>MQTT</h3><form method=post action=/config>
  <label>Broker host</label><input name=mhost id=mhost>
  <label>Port</label><input name=mport id=mport type=number>
  <label>Username (optional)</label><input name=muser id=muser>
  <label>Password (blank = keep)</label><input name=mpass type=password>
  <input type=hidden name=action value=mqtt><button>Save MQTT</button></form>
 </div>

 <div class=card><h3>Topic</h3><form method=post action=/config>
  <label>Base topic</label><input name=base id=base>
  <input type=hidden name=action value=base><button>Save topic</button>
  <p class=muted>Publishes to <code>&lt;base&gt;/&lt;id&gt;/rx</code> &amp; <code>/status</code>; listens on <code>/cmd/#</code>.</p></form>
 </div>
</div>
<script>
let filled=false;
async function tick(){
 let j;try{j=await(await fetch('/api.json')).json();}catch(e){return;}
 document.getElementById('hdr').textContent=j.id+' · '+j.wifi+(j.ip!='-'?(' '+j.ip):'');
 document.getElementById('stat').innerHTML=
  `node id <b>${j.id}</b><br>wifi <b class=${j.wifi=='connected'?'ok':'bad'}>${j.wifi}</b> ${j.ip}<br>`+
  `mqtt <b class=${j.mqtt?'ok':'bad'}>${j.mqtt?'connected':'down'}</b> ${j.mhost||'(unset)'}:${j.mport}<br>`+
  `base <b>${j.base}</b><br>rx <b>${j.rx}</b> · published <b>${j.pub}</b> · cmds <b>${j.cmd}</b>`;
 if(!filled){filled=true;set('ssid',j.ssid);set('mhost',j.mhost);set('mport',j.mport);set('muser',j.muser);set('base',j.base);}
}
function set(id,v){let e=document.getElementById(id);if(e&&document.activeElement!=e)e.value=v;}
tick();setInterval(tick,3000);
</script></body></html>)HTML";

class AgentWeb {
  MeshAgent& _m;
  WebServer _server;

  void handleApi() {
    bool up = (WiFi.status() == WL_CONNECTED);
    char buf[420];
    snprintf(buf, sizeof(buf),
      "{\"id\":\"%s\",\"wifi\":\"%s\",\"ip\":\"%s\",\"mqtt\":%s,\"mhost\":\"%s\",\"mport\":%u,"
      "\"muser\":\"%s\",\"ssid\":\"%s\",\"base\":\"%s\",\"rx\":%u,\"pub\":%u,\"cmd\":%u}",
      _m.nodeId(), up?"connected":(_m.apActive()?"AP":"down"),
      up?WiFi.localIP().toString().c_str():(_m.apActive()?WiFi.softAPIP().toString().c_str():"-"),
      _m.mqttUp()?"true":"false", _m.mqttHost(), _m.mqttPort(), _m.mqttUser(),
      _m.ssid(), _m.baseTopic(), _m.rxCount(), _m.pubCount(), _m.cmdCount());
    _server.send(200, "application/json", buf);
  }

  void handleConfig() {
    String a = _server.arg("action");
    if (a == "wifi")      _m.setWifi(_server.arg("ssid").c_str(), _server.arg("wpass").c_str());
    else if (a == "mqtt") _m.setMqtt(_server.arg("mhost").c_str(), _server.arg("mport").toInt(),
                                     _server.arg("muser").c_str(), _server.arg("mpass").c_str());
    else if (a == "base") _m.setBaseTopic(_server.arg("base").c_str());
    _server.sendHeader("Location", "/");
    _server.send(303, "text/plain", "ok");
  }

public:
  AgentWeb(MeshAgent& m) : _m(m), _server(HTTP_PORT) {}
  void begin() {
    _server.on("/", [this]() { _server.send_P(200, "text/html; charset=utf-8", AGENT_PAGE); });
    _server.on("/api.json", [this]() { handleApi(); });
    _server.on("/config", HTTP_POST, [this]() { handleConfig(); });
    _server.onNotFound([this]() { _server.send(404, "text/plain", "not found\n"); });
    _server.begin();
  }
  void loop() { _server.handleClient(); }
};

#endif // ESP32 && WITH_WIFI
