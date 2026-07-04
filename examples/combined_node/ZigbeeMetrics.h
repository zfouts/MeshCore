// combined_node -- Zigbee end-device metrics bridge (ESP32-C6/H2 only).
//
// Exposes the node's mesh telemetry as Zigbee endpoints so a Home Assistant
// Zigbee network (ZHA / Zigbee2MQTT) can graph mesh health with no WiFi/MQTT:
// battery, uptime, rx/relayed/dropped packet counts, neighbour count, last
// RSSI/SNR, free heap -- plus a controllable on/off endpoint bound to the
// LoRa relay (client_repeat), so HA automations can toggle repeating.
//
// Deliberately decoupled from MyMesh: the mesh side fills a plain snapshot
// struct each report tick, and HA-initiated relay toggles are queued here and
// consumed from the main loop (the Zigbee stack runs in its own task; nothing
// touches mesh state cross-task).

#pragma once

#if defined(WITH_ZIGBEE_METRICS)

#include <stdint.h>

#ifndef COMBINED_ZIGBEE_REPORT_S
#define COMBINED_ZIGBEE_REPORT_S 30   // attribute report cadence (seconds)
#endif

struct ZigbeeMetricsSnapshot {
  uint16_t batt_mv;       // battery/VBUS millivolts
  uint8_t  batt_pct;      // LiPo state-of-charge estimate (0-100)
  float    uptime_hours;
  uint32_t rx_count;      // raw packets heard
  uint32_t relayed;       // packets we forwarded
  uint32_t dropped;       // floods dropped by relay policy
  uint8_t  neighbours;    // directly-heard nodes in the table
  float    last_rssi;     // dBm of most recent RX
  float    last_snr;      // dB of most recent RX
  uint32_t free_heap;     // bytes
  bool     relay_on;      // client_repeat state (reported on the outlet EP)
};

// Register endpoints and start the Zigbee stack (call once, after prefs are
// loaded). Joins any open Zigbee network; until then updates are no-ops.
void zigbeeMetricsBegin(const char* node_name);

// Push current values to the Zigbee clusters (call every COMBINED_ZIGBEE_REPORT_S).
void zigbeeMetricsUpdate(const ZigbeeMetricsSnapshot& s);

// True once joined to a Zigbee network.
bool zigbeeMetricsConnected();

// HA toggled the relay endpoint: returns 1 (on), 0 (off) or -1 (nothing
// pending) and clears the pending state. Consume from the main loop.
int zigbeeMetricsConsumePendingRelay();

// Leave the Zigbee network and clear join state, then reboot into pairing
// mode (`set zigbee.reset 1` via meshcore-cli). Does not return.
void zigbeeMetricsFactoryReset();

#endif // WITH_ZIGBEE_METRICS
