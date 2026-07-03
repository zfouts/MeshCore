#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BridgeBase.h"

#ifdef WITH_MQTT_BRIDGE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <PubSubClient.h>

/**
 * @brief Bridge that tunnels mesh packets over MQTT to link two LoRa meshes
 *        across TCP/IP. See MQTTBridge.cpp for the wire format + loop guard.
 *
 * Self-contained connectivity: owns WiFi (STA, with AP fallback so it is always
 * reachable) and a small web UI to configure WiFi + MQTT + topic at runtime
 * (persisted to flash). Build flags MQTT_BRIDGE_SSID/PASS/HOST/PORT/USER/
 * PASSWORD/TOPIC act only as first-boot defaults.
 */

struct MqttBridgeCfg {
  uint32_t magic;
  uint8_t  wifi_enabled;
  uint8_t  tls_enabled;       // TLS to the broker (verified against Let's Encrypt root)
  uint8_t  web_enabled;       // serve the config/admin web UI
  uint16_t mqtt_port;
  char     ssid[33];
  char     pass[64];
  char     host[64];
  char     user[33];
  char     mpass[64];
  char     topic[48];
};

class MQTTBridge : public BridgeBase {
public:
  MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  void begin() override;
  void end() override;
  void loop() override;
  void sendPacket(mesh::Packet *packet) override;
  void onPacketReceived(mesh::Packet *packet) override;
  void setWebEnabled(bool enable);   // toggle the web UI at runtime (persisted)

  // Register the repeater's CLI so the web UI can run any `set`/`get`/etc.
  // command (sender_timestamp=0 = local admin). cmd is mutable (tokenized).
  typedef void (*CliFn)(void *ctx, char *cmd, char *reply, int reply_sz);
  void setCli(void *ctx, CliFn fn) { _cli_ctx = ctx; _cli_fn = fn; }

private:
  void *_cli_ctx = nullptr;
  CliFn _cli_fn = nullptr;
  bool requireAuth();   // HTTP basic auth against _prefs->password
  void handleCli();

  static MQTTBridge *_instance;
  static void mqtt_cb(char *topic, uint8_t *payload, unsigned int len);

  WiFiClient       _net;
  WiFiClientSecure _tls;
  PubSubClient     _mqtt;
  WebServer        _web;
  MqttBridgeCfg    _cfg;
  unsigned long _next_wifi_check, _sta_since, _next_mqtt_try;
  bool          _ap_active, _web_started, _time_ok;
  char          _client_id[24];

  void applyMqttClient();   // point PubSubClient at plain or TLS transport
  void clockFromRtc();      // feed system clock from the GPS-synced mesh RTC (for TLS)

  void cfgDefaults();
  void loadCfg();
  void saveCfg();
  void onMqttData(uint8_t *data, unsigned int len);
  void connectMqtt();
  void startAP();
  void netLoop();
  void startWeb();
  void handleApi();
  void handleConfig();
  void xorCrypt(uint8_t *data, size_t len);

  bool wifiOk() const { return _cfg.wifi_enabled && _cfg.ssid[0]; }
  bool netUp()  const { return WiFi.status() == WL_CONNECTED || _ap_active; }
};

#endif
