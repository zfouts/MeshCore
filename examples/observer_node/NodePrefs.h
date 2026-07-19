#pragma once
#include <cstdint> // For uint8_t, uint32_t

#define TELEM_MODE_DENY            0
#define TELEM_MODE_ALLOW_FLAGS     1     // use contact.flags
#define TELEM_MODE_ALLOW_ALL       2

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1

struct NodePrefs {  // persisted to file
  float airtime_factor;
  char node_name[32];
  float freq;
  uint8_t sf;
  uint8_t cr;
  uint8_t multi_acks;
  uint8_t manual_add_contacts;
  float bw;
  int8_t tx_power_dbm;
  uint8_t telemetry_mode_base;
  uint8_t telemetry_mode_loc;
  uint8_t telemetry_mode_env;
  float rx_delay_base;
  uint32_t ble_pin;
  uint8_t  advert_loc_policy;
  uint8_t  buzzer_quiet;
  uint8_t  gps_enabled;      // GPS enabled flag (0=disabled, 1=enabled)
  uint32_t gps_interval;     // GPS read interval in seconds
  uint8_t autoadd_config;    // bitmask for auto-add contacts config
  uint8_t rx_boosted_gain; // SX126x RX boosted gain mode (0=power saving, 1=boosted)
  uint8_t client_repeat;
  uint8_t path_hash_mode;    // which path mode to use when sending
  uint8_t autoadd_max_hops;  // 0 = no limit, 1 = direct (0 hops), N = up to N-1 hops (max 64)
  char default_scope_name[31];
  uint8_t default_scope_key[16];
  uint8_t bot_enabled;   // observer_node: bot master enable (0/1)
  uint8_t bot_channel;   // observer_node: group channel index the bot answers on (0xFF = off)
  uint8_t ble_enabled;   // observer_node: BLE advertising enable (0/1) -- toggled via `!ble on/off`
  uint8_t bot_control_channel; // observer_node: channel index authorized for control writes (0xFF = off)
  char wifi_ssid[33];    // observer_node: WiFi STA SSID, set via `set wifi_ssid` ("" = WiFi off)
  char wifi_pwd[65];     // observer_node: WiFi passphrase, set via `set wifi_pwd` ("" = open network)
  uint32_t boot_count;   // observer_node: boots since first flash (surfaced via `!boot`)
  char obs_url[65];      // observer_node: mesh-observer base URL for !path map links ("" = off)
  char obs_token[65];    // observer_node: observer device token (sent as X-Device-Token;
                         // 64 chars fits an openssl rand -hex 24/32 secret)
  uint64_t bot_channel_mask; // observer_node: bitmask of channel indices the bot answers on
                             // (multi-channel; 0 = derived from legacy bot_channel at boot)
  uint64_t bot_path_mask;    // observer_node: channels where ONLY `!path` answers (e.g. a
                             // public #bot) -- every other command stays silent there
  char mqtt_host[65];    // observer_node: broker "[mqtt(s)://]host[:port]", default 1883/8883-tls ("" = off)
  char mqtt_user[33];    // observer_node: MQTT username ("" = anonymous)
  char mqtt_pwd[65];     // observer_node: MQTT password (never echoed back)
  char mqtt_topic[33];   // observer_node: topic prefix ("" = "meshcore/<node name>")
  uint8_t mqtt_tls_insecure; // observer_node: 1 = skip TLS cert validation for mqtts://
                             // (opt-in; default 0 = verify against pinned LE roots)
};