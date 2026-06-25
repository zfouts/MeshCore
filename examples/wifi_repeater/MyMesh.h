#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/RegionMap.h>
#include "RateLimiter.h"

#ifdef WITH_BRIDGE
extern AbstractBridge* bridge;
#endif

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
};

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "6 Jun 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.16.0"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  int8_t  reply_path_len;
  uint8_t reply_path_hash_size;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  RateLimiter discover_limiter, anon_limiter;
  uint32_t pending_discover_tag;
  unsigned long pending_discover_until;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#endif

#if defined(WITH_WIFI)
  // Public-channel message capture (HTTP page). A repeater only relays encrypted
  // packets and cannot read DMs/private channels, but the PUBLIC channel uses a
  // well-known key -- so we hold that key and decrypt public group-text packets
  // as they pass through, keeping the most recent few for the web UI.
public:
  struct PublicChatMsg {
    uint32_t rx_uptime;     // seconds-since-boot when heard
    uint32_t msg_timestamp; // sender's timestamp (epoch, if their clock is set)
    int8_t   snr;           // link quality (snr * 4), as heard by this node
    char     text[100];     // decrypted "name: message"
  };
  // A node we've heard an advert from. Unlike channels, adverts carry the node's
  // name in the clear, so this is a real named view of the surrounding mesh.
  struct HeardNode {
    uint8_t  pubkey[6];     // pubkey prefix (display + dedup)
    char     name[24];      // advertised name ("" if none)
    uint8_t  type;          // ADV_TYPE_* (repeater/client/room/sensor)
    int8_t   snr;           // snr * 4, as last heard
    uint8_t  hops;          // path-hash count when last heard (0 = direct)
    uint32_t last_uptime;   // seconds-since-boot last heard
    uint32_t count;         // adverts heard from this node
  };
private:
  static const int PUBLIC_CHAT_LOG = 20;
  mesh::GroupChannel _pub_chan;
  bool      _pub_chan_ready = false;
  PublicChatMsg _chatlog[PUBLIC_CHAT_LOG];
  int       _chatlog_head = 0;
  int       _chatlog_count = 0;
  uint32_t  _public_msg_total = 0;
  // Channel activity: group-text packets seen per 1-byte channel hash, even for
  // channels we can't decrypt. The hash is sha256(key)[0] -- coarse (256 values,
  // collisions possible), so this counts traffic, it does not uniquely ID a channel.
  uint32_t  _chan_count[256];
  uint32_t  _chan_last[256];
  static const int HEARD_NODES = 32;
  HeardNode _heard[HEARD_NODES];
  int       _heard_count = 0;
  // Operating-channel noise floor (dBm) ring, sampled ~1/sec from getCurrentRSSI.
  static const int NOISE_HIST = 60;
  int8_t    _noise_hist[NOISE_HIST];
  int       _noise_head = 0, _noise_count = 0;
  int16_t   _noise_cur = 0, _noise_min = 0, _noise_max = 0;
  void beginPublicChannel();
  void recordHeardNode(const mesh::Identity& id, const char* name, uint8_t type,
                       int8_t snr, uint8_t hops);
#endif

  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  File openAppend(const char* fname);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  bool getCADEnabled() const override {
    return _prefs.cad_enabled;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  bool filterRecvFloodPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;
#if defined(WITH_WIFI)
  // Offer the public channel key for decryption, and capture decrypted public
  // group-text messages. No-ops for any other channel (we hold no other keys).
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;
#endif

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void sendNodeDiscoverReq();
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  // Number of neighbours currently held in the table (heard_timestamp != 0;
  // the array is zero-initialized, so that reliably marks an occupied slot).
  // Exposed for the HTTP stats/metrics endpoints.
  int getNeighbourCount() const {
#if MAX_NEIGHBOURS
    int n = 0;
    for (int i = 0; i < MAX_NEIGHBOURS; i++)
      if (neighbours[i].heard_timestamp != 0) n++;
    return n;
#else
    return 0;
#endif
  }

#if defined(WITH_WIFI)
  // Recent public-channel messages, newest first (i = 0 .. getPublicChatCount()-1).
  int getPublicChatCount() const { return _chatlog_count; }
  uint32_t getPublicChatTotal() const { return _public_msg_total; }
  const PublicChatMsg* getPublicChatMsg(int i) const {
    if (i < 0 || i >= _chatlog_count) return nullptr;
    int idx = (_chatlog_head - 1 - i + 2 * PUBLIC_CHAT_LOG) % PUBLIC_CHAT_LOG;
    return &_chatlog[idx];
  }
  // Channel activity by 1-byte channel hash (0..255).
  uint32_t getChannelMsgCount(int hashByte) const {
    return (hashByte >= 0 && hashByte < 256) ? _chan_count[hashByte] : 0;
  }
  uint32_t getChannelLastSeen(int hashByte) const {
    return (hashByte >= 0 && hashByte < 256) ? _chan_last[hashByte] : 0;
  }
  uint8_t getPublicChannelHash() const { return _pub_chan.hash[0]; }
  // Nodes heard via advert (raw storage order; caller may sort by last_uptime).
  int getHeardCount() const { return _heard_count; }
  const HeardNode* getHeard(int i) const {
    return (i >= 0 && i < _heard_count) ? &_heard[i] : nullptr;
  }

  // Operating-channel noise floor (dBm). Sampled from the loop (see main.cpp).
  void recordNoiseSample(float rssi) {
    int v = (int)rssi;
    if (v > 0) v = 0; else if (v < -128) v = -128;
    _noise_cur = v;
    if (_noise_count == 0) { _noise_min = v; _noise_max = v; }
    else { if (v < _noise_min) _noise_min = v; if (v > _noise_max) _noise_max = v; }
    _noise_hist[_noise_head] = (int8_t)v;
    _noise_head = (_noise_head + 1) % NOISE_HIST;
    if (_noise_count < NOISE_HIST) _noise_count++;
  }
  int getNoiseCur()   const { return _noise_cur; }
  int getNoiseMin()   const { return _noise_min; }
  int getNoiseMax()   const { return _noise_max; }
  int getNoiseCount() const { return _noise_count; }
  int getNoiseHist(int i) const {            // oldest-first, for plotting
    if (i < 0 || i >= _noise_count) return 0;
    int start = (_noise_count < NOISE_HIST) ? 0 : _noise_head;
    return _noise_hist[(start + i) % NOISE_HIST];
  }
#endif

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  void loop();

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
    if (enable == bridge.isRunning()) return;
    if (enable)
    {
      bridge.begin();
    }
    else 
    {
      bridge.end();
    }
  }

  void restartBridge() override {
    if (!bridge.isRunning()) return;
    bridge.end();
    bridge.begin();
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

#if defined(USE_SX1262) || defined(USE_SX1268)
  void setRxBoostedGain(bool enable) override;
#endif
};
