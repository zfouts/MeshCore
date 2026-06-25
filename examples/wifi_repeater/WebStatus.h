#pragma once

// Read-only HTTP status server for wifi_repeater.
//
//   GET /          -> static HTML/JS shell (served as-is; cheap)
//   GET /api.json  -> compact JSON the page polls every 5s
//   GET /metrics   -> Prometheus text-format metrics
//   POST /advert/* -> trigger an advert (flood / zerohop)
//
// Low-overhead by design: the ESP only rebuilds the small JSON blob per refresh;
// the browser does all rendering (tables, topology graph, noise sparkline). The
// page itself is static so serving "/" costs essentially nothing.

#if defined(ESP32) && defined(WITH_WIFI)

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <helpers/AdvertDataHelpers.h>   // ADV_TYPE_* labels
#include "MyMesh.h"

#ifndef HTTP_PORT
  #define HTTP_PORT 80
#endif

class WebStatus {
  WebServer _server;
  MyMesh&   _mesh;

  static void pubkeyHex(MyMesh& m, char* out) {     // out: 2*PUB_KEY_SIZE+1 bytes
    mesh::Utils::toHex(out, m.getSelfId().pub_key, PUB_KEY_SIZE);
  }

  static const char* nodeTypeLabel(uint8_t t) {
    switch (t) {
      case ADV_TYPE_REPEATER: return "repeater";
      case ADV_TYPE_CHAT:     return "client";
      case ADV_TYPE_ROOM:     return "room server";
      case ADV_TYPE_SENSOR:   return "sensor";
      default:                return "?";
    }
  }

  // JSON-escape a C string into a quoted JSON value.
  static String jstr(const char* s) {
    String o = "\"";
    for (const char* p = s; *p; p++) {
      char c = *p;
      if      (c == '"' || c == '\\') { o += '\\'; o += c; }
      else if (c == '\n')             o += "\\n";
      else if (c == '\r')             o += "\\r";
      else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
      else                            o += c;
    }
    o += "\"";
    return o;
  }

  // Compact JSON snapshot the page polls. Built fresh per request; kept small.
  void handleApi() {
    NodePrefs* p = _mesh.getNodePrefs();
    char pk[2 * PUB_KEY_SIZE + 1]; pubkeyHex(_mesh, pk);
    uint32_t now = millis() / 1000;

    String j; j.reserve(4096);
    j += "{\"node\":"; j += jstr(p->node_name);
    j += ",\"pk\":\""; j += pk; j += "\"";
    j += ",\"ver\":\""; j += FIRMWARE_VERSION; j += "\",\"role\":\""; j += FIRMWARE_ROLE; j += "\"";
    j += ",\"up\":"; j += now;
    j += ",\"freq\":"; j += String((double)p->freq * 1000000.0, 0);
    j += ",\"bw\":"; j += String((double)p->bw * 1000.0, 0);
    j += ",\"sf\":"; j += p->sf; j += ",\"cr\":"; j += p->cr; j += ",\"txp\":"; j += (int)p->tx_power_dbm;
    j += ",\"batt\":"; j += board.getBattMilliVolts();
    j += ",\"heap\":"; j += ESP.getFreeHeap();
    j += ",\"wifi\":{\"ssid\":"; j += jstr(WiFi.SSID().c_str());
    j += ",\"ip\":\""; j += WiFi.localIP().toString(); j += "\",\"rssi\":"; j += WiFi.RSSI(); j += "}";

    j += ",\"noise\":{\"cur\":"; j += _mesh.getNoiseCur();
    j += ",\"min\":"; j += _mesh.getNoiseMin(); j += ",\"max\":"; j += _mesh.getNoiseMax();
    j += ",\"h\":[";
    int ncnt = _mesh.getNoiseCount();
    for (int i = 0; i < ncnt; i++) { if (i) j += ','; j += _mesh.getNoiseHist(i); }
    j += "]}";

    j += ",\"ctr\":{\"sf\":"; j += _mesh.getNumSentFlood();
    j += ",\"sd\":"; j += _mesh.getNumSentDirect();
    j += ",\"rf\":"; j += _mesh.getNumRecvFlood();
    j += ",\"rd\":"; j += _mesh.getNumRecvDirect(); j += "}";
    j += ",\"neigh\":"; j += _mesh.getNeighbourCount();
    j += ",\"pubtot\":"; j += _mesh.getPublicChatTotal();

    j += ",\"msgs\":[";
    int nm = _mesh.getPublicChatCount();
    for (int i = 0; i < nm; i++) {
      const MyMesh::PublicChatMsg* m = _mesh.getPublicChatMsg(i); if (!m) continue;
      if (i) j += ',';
      j += "{\"ago\":"; j += (now - m->rx_uptime);
      j += ",\"snr\":"; j += String(m->snr / 4.0, 1);
      j += ",\"t\":"; j += jstr(m->text); j += "}";
    }
    j += "]";

    j += ",\"chans\":[";
    uint8_t pubhash = _mesh.getPublicChannelHash(); bool first = true;
    for (int hb = 0; hb < 256; hb++) {
      uint32_t c = _mesh.getChannelMsgCount(hb); if (!c) continue;
      if (!first) j += ','; first = false;
      char hx[8]; snprintf(hx, sizeof(hx), "0x%02X", hb);
      j += "{\"h\":\""; j += hx; j += "\",\"l\":\""; j += (hb == pubhash ? "public" : "unknown");
      j += "\",\"c\":"; j += c; j += ",\"ago\":"; j += (now - _mesh.getChannelLastSeen(hb)); j += "}";
    }
    j += "]";

    j += ",\"nodes\":[";
    int nn = _mesh.getHeardCount();
    for (int i = 0; i < nn; i++) {
      const MyMesh::HeardNode* n = _mesh.getHeard(i); if (!n) continue;
      if (i) j += ',';
      char npk[16]; mesh::Utils::toHex(npk, (uint8_t*)n->pubkey, sizeof(n->pubkey));
      j += "{\"n\":"; j += jstr(n->name);
      j += ",\"pk\":\""; j += npk; j += "\",\"ty\":\""; j += nodeTypeLabel(n->type);
      j += "\",\"snr\":"; j += String(n->snr / 4.0, 1);
      j += ",\"hop\":"; j += n->hops; j += ",\"ago\":"; j += (now - n->last_uptime);
      j += ",\"c\":"; j += n->count; j += "}";
    }
    j += "]}";

    _server.send(200, "application/json", j);
  }

  void handleMetrics() {
    NodePrefs* p = _mesh.getNodePrefs();
    char pk[2 * PUB_KEY_SIZE + 1]; pubkeyHex(_mesh, pk);

    String b; b.reserve(1800);
    auto m = [&](const char* name, const char* type, const char* help, double v) {
      b += "# HELP "; b += name; b += ' '; b += help; b += '\n';
      b += "# TYPE "; b += name; b += ' '; b += type; b += '\n';
      b += name; b += ' '; b += String(v, 0); b += '\n';
    };

    b += "# HELP meshcore_info Node identity and build info\n# TYPE meshcore_info gauge\n";
    b += "meshcore_info{node=\""; b += p->node_name;
    b += "\",pubkey=\""; b += pk;
    b += "\",version=\""; b += FIRMWARE_VERSION;
    b += "\",role=\""; b += FIRMWARE_ROLE; b += "\"} 1\n";

    m("meshcore_uptime_seconds",       "counter", "Seconds since boot",          millis() / 1000);
    m("meshcore_sent_flood_total",     "counter", "Flood packets sent",          _mesh.getNumSentFlood());
    m("meshcore_sent_direct_total",    "counter", "Direct packets sent",         _mesh.getNumSentDirect());
    m("meshcore_recv_flood_total",     "counter", "Flood packets received",      _mesh.getNumRecvFlood());
    m("meshcore_recv_direct_total",    "counter", "Direct packets received",     _mesh.getNumRecvDirect());
    m("meshcore_neighbours",           "gauge",   "Neighbours in the table",     _mesh.getNeighbourCount());
    m("meshcore_nodes_heard",          "gauge",   "Distinct nodes heard via advert", _mesh.getHeardCount());
    m("meshcore_public_messages_total","counter", "Public-channel messages seen", _mesh.getPublicChatTotal());
    m("meshcore_battery_millivolts",   "gauge",   "Battery voltage in mV",       board.getBattMilliVolts());
    m("meshcore_free_heap_bytes",      "gauge",   "Free heap in bytes",          ESP.getFreeHeap());
    m("meshcore_wifi_rssi_dbm",        "gauge",   "WiFi RSSI in dBm",            WiFi.RSSI());
    m("meshcore_noise_floor_dbm",      "gauge",   "Operating-channel noise floor (dBm)", _mesh.getNoiseCur());
    m("meshcore_radio_freq_hz",        "gauge",   "LoRa frequency in Hz",        p->freq * 1000000.0);
    m("meshcore_radio_bandwidth_hz",   "gauge",   "LoRa bandwidth in Hz",        p->bw * 1000.0);
    m("meshcore_radio_sf",             "gauge",   "LoRa spreading factor",       p->sf);
    m("meshcore_radio_cr",             "gauge",   "LoRa coding rate",            p->cr);
    m("meshcore_radio_tx_power_dbm",   "gauge",   "LoRa TX power in dBm",        p->tx_power_dbm);

    uint8_t pubhash = _mesh.getPublicChannelHash();
    b += "# HELP meshcore_channel_messages_total Group-text messages seen per channel hash\n";
    b += "# TYPE meshcore_channel_messages_total counter\n";
    for (int hb = 0; hb < 256; hb++) {
      uint32_t c = _mesh.getChannelMsgCount(hb);
      if (!c) continue;
      char hx[8]; snprintf(hx, sizeof(hx), "0x%02X", hb);
      b += "meshcore_channel_messages_total{hash=\""; b += hx;
      b += "\",label=\""; b += (hb == pubhash ? "public" : "unknown");
      b += "\"} "; b += String((double)c, 0); b += '\n';
    }

    _server.send(200, "text/plain; version=0.0.4", b);
  }

  // Send an advert on button click. flood -> multi-hop, !flood -> zero-hop.
  void handleAdvert(bool flood) {
    _mesh.sendSelfAdvertisement(1500, flood);
    String h = "<!doctype html><meta charset=utf-8>"
               "<meta http-equiv=refresh content=\"2;url=/\">"
               "<body style='font-family:system-ui,sans-serif;margin:2rem'>&#10003; ";
    h += flood ? "Flood" : "Zero-hop";
    h += " advert queued.<p><a href=\"/\">&larr; back</a></p></body>";
    _server.send(200, "text/html; charset=utf-8", h);
  }

  void handleRoot() {
    static const char PAGE[] =
R"PAGE(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>MeshCore Repeater</title>
<style>
body{font-family:system-ui,sans-serif;margin:1.2rem;max-width:46rem;color:#222}
h1{font-size:1.3rem;margin:.2rem 0}h2{font-size:1.05rem;margin:1.3rem 0 .3rem}
small,.muted{color:#888}table{border-collapse:collapse;width:100%}
td{padding:.25rem .5rem;border-bottom:1px solid #eee;vertical-align:top}
td:first-child{color:#555}code{word-break:break-all;font-size:.85em}
button{padding:.4rem .8rem;margin:.3rem .4rem .3rem 0;cursor:pointer;border:1px solid #bbb;border-radius:.3rem;background:#f6f6f6}
button:hover{background:#eaeaea}form{display:inline}
#topo{border:1px solid #eee;border-radius:.4rem}
</style></head><body>
<h1 id=node>&hellip;</h1><div id=sub class=muted></div>
<p><form method=post action="/advert/flood"><button>Send flood advert</button></form>
<form method=post action="/advert/zerohop"><button>Send zero-hop advert</button></form></p>
<div id=stats></div>
<h2>Noise floor <small id=noiselbl></small></h2><div id=noise></div>
<h2>Topology <small class=muted>(this node + heard nodes)</small></h2><div id=topo></div>
<h2>Nodes heard <small id=nn></small></h2><div id=nodes></div>
<h2>Public channel <small id=pm></small></h2><div id=msgs></div>
<h2>Channels seen</h2><div id=chans></div>
<p class=muted><a href="/metrics">/metrics</a> &middot; <a href="/api.json">/api.json</a> &middot; auto-refresh 5s</p>
<script>
var $=function(i){return document.getElementById(i)};
var E=function(s){return String(s).replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})};
var ago=function(s){return s<60?s+'s':s<3600?((s/60|0)+'m'):((s/3600|0)+'h')};
function spark(h){if(!h||!h.length)return'';var w=240,t=40,mn=Math.min.apply(0,h),mx=Math.max.apply(0,h),r=(mx-mn)||1;
 var p=h.map(function(v,i){return (i/Math.max(1,h.length-1)*w).toFixed(1)+','+(t-(v-mn)/r*t).toFixed(1)}).join(' ');
 return '<svg viewBox="0 0 '+w+' '+t+'" width='+w+' height='+t+'><polyline points="'+p+'" fill=none stroke=#39c stroke-width=1.5/></svg>'}
function topo(d){var ns=d.nodes||[],W=460,H=340,cx=W/2,cy=H/2,mh=1;
 ns.forEach(function(n){mh=Math.max(mh,n.hop||0)});
 var s='<svg id=topo viewBox="0 0 '+W+' '+H+'" width=100% style="max-width:'+W+'px">';
 ns.forEach(function(n,i){var a=i/ns.length*2*Math.PI-Math.PI/2,rr=60+(n.hop||0)/mh*110;
  n._x=cx+Math.cos(a)*rr;n._y=cy+Math.sin(a)*rr;
  var c=n.snr>=5?'#2a7':n.snr>=0?'#c90':'#c33';
  s+='<line x1='+cx+' y1='+cy+' x2='+n._x.toFixed(0)+' y2='+n._y.toFixed(0)+' stroke="'+c+'" stroke-width='+(n.hop?1:2)+(n.hop?' stroke-dasharray="4 3"':'')+'/>'});
 ns.forEach(function(n){var c=n.ty=='repeater'?'#36c':n.ty=='sensor'?'#3a3':n.ty=='room server'?'#a4c':'#888';
  s+='<circle cx='+n._x.toFixed(0)+' cy='+n._y.toFixed(0)+' r=6 fill="'+c+'"/>';
  s+='<text x='+n._x.toFixed(0)+' y='+(n._y-9).toFixed(0)+' font-size=11 text-anchor=middle>'+E(n.n||n.pk.slice(0,8))+'</text>'});
 s+='<circle cx='+cx+' cy='+cy+' r=9 fill=#111 /><text x='+cx+' y='+(cy-13)+' font-size=12 text-anchor=middle font-weight=bold>'+E(d.node)+'</text>';
 return s+'</svg>'}
function rows(a){return a.map(function(r){return '<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>'}).join('')}
function render(d){
 document.title=d.node;
 $('node').innerHTML=E(d.node)+' <small>('+E(d.role)+')</small>';$('sub').textContent=d.ver;
 $('stats').innerHTML='<table>'+rows([
  ['Public key','<code>'+E(d.pk)+'</code>'],['Uptime',ago(d.up)],
  ['Radio',(d.freq/1e6).toFixed(3)+' MHz &middot; BW '+(d.bw/1e3)+' &middot; SF'+d.sf+' &middot; CR'+d.cr+' &middot; '+d.txp+' dBm'],
  ['Sent flood/direct',d.ctr.sf+' / '+d.ctr.sd],['Recv flood/direct',d.ctr.rf+' / '+d.ctr.rd],
  ['Neighbours',d.neigh],['Battery',d.batt+' mV'],['Free heap',d.heap+' B'],
  ['WiFi',E(d.wifi.ssid)+' &middot; '+d.wifi.ip+' &middot; '+d.wifi.rssi+' dBm']])+'</table>';
 $('noiselbl').textContent='cur '+d.noise.cur+' dBm &middot; min '+d.noise.min+' &middot; max '+d.noise.max;
 $('noiselbl').innerHTML='cur '+d.noise.cur+' dBm &middot; min '+d.noise.min+' &middot; max '+d.noise.max;
 $('noise').innerHTML=spark(d.noise.h)||'<span class=muted>sampling&hellip;</span>';
 $('topo').innerHTML=d.nodes.length?topo(d):'<p class=muted style=padding:1rem>No nodes heard yet.</p>';
 var ns=d.nodes.slice().sort(function(a,b){return a.ago-b.ago});
 $('nn').textContent='('+ns.length+')';
 $('nodes').innerHTML=ns.length?'<table>'+ns.map(function(n){return '<tr><td>'+(n.n?E(n.n):'<code>'+n.pk+'</code>')+
  '<br><small>'+E(n.ty)+' &middot; '+n.pk+'</small></td><td>'+n.snr+' dB &middot; '+(n.hop?n.hop+' hop'+(n.hop>1?'s':''):'direct')+
  '<br><small>'+ago(n.ago)+' ago &middot; '+n.c+'&times;</small></td></tr>'}).join('')+'</table>':'<p class=muted>No adverts heard yet.</p>';
 $('pm').textContent='('+d.pubtot+' seen)';
 $('msgs').innerHTML=d.msgs.length?'<table>'+d.msgs.map(function(m){return '<tr><td style=white-space:nowrap>'+ago(m.ago)+
  ' ago<br><small>'+m.snr+' dB</small></td><td>'+E(m.t)+'</td></tr>'}).join('')+'</table>':'<p class=muted>No public messages heard yet.</p>';
 $('chans').innerHTML=d.chans.length?'<table>'+d.chans.map(function(c){return '<tr><td>'+c.h+(c.l=='public'?' <small>(Public)</small>':'')+
  '</td><td>'+c.c+' msgs &middot; '+ago(c.ago)+' ago</td></tr>'}).join('')+'</table>':'<p class=muted>No channel traffic seen yet.</p>'}
function refresh(){fetch('/api.json').then(function(r){return r.json()}).then(render).catch(function(){})}
refresh();setInterval(refresh,5000);
</script></body></html>)PAGE";
    _server.send(200, "text/html; charset=utf-8", String(PAGE));
  }

public:
  WebStatus(MyMesh& mesh) : _server(HTTP_PORT), _mesh(mesh) {}

  void begin() {
    _server.on("/", [this]() { handleRoot(); });
    _server.on("/api.json", [this]() { handleApi(); });
    _server.on("/metrics", [this]() { handleMetrics(); });
    _server.on("/advert/flood",   HTTP_POST, [this]() { handleAdvert(true); });
    _server.on("/advert/zerohop", HTTP_POST, [this]() { handleAdvert(false); });
    _server.onNotFound([this]() { _server.send(404, "text/plain", "not found\n"); });
    _server.begin();
  }

  void loop() { _server.handleClient(); }
};

#endif // ESP32 && WITH_WIFI
