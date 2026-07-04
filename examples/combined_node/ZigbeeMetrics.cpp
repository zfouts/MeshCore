// combined_node -- Zigbee end-device metrics bridge. See ZigbeeMetrics.h.
// Only compiled by *_combined_node_zigbee envs (ESP32-C6/H2, Arduino core >=3.2
// for ZigbeeAnalog). The Zigbee stack runs in its own task; this module owns
// all Zigbee objects and the mesh side only passes plain snapshots in.

#if defined(WITH_ZIGBEE_METRICS)

#include <Arduino.h>
#include "Zigbee.h"
#include "ZigbeeMetrics.h"

// One generic numeric sensor endpoint per metric. ZHA/Z2M map the Analog
// Input cluster straight to a sensor entity, using the description string as
// the name (keep them <=16 chars for ZCL string limits).
static ZigbeeAnalog zbBattV(10);
static ZigbeeAnalog zbBattPct(11);
static ZigbeeAnalog zbUptime(12);
static ZigbeeAnalog zbRx(13);
static ZigbeeAnalog zbRelayed(14);
static ZigbeeAnalog zbDropped(15);
static ZigbeeAnalog zbNeigh(16);
static ZigbeeAnalog zbRssi(17);
static ZigbeeAnalog zbSnr(18);
static ZigbeeAnalog zbHeap(19);
// On/Off endpoint bound to the LoRa relay -- controllable from HA. Modeled as
// a "light" because the On/Off cluster is what every HA integration can both
// read AND drive; rename/re-domain it in HA if the icon bothers you.
static ZigbeeLight zbRelay(20);

static volatile int s_pending_relay = -1;   // set by zigbee task, consumed by main loop
static bool s_started = false;

static void onRelayChange(bool state) {
  s_pending_relay = state ? 1 : 0;   // queue for the mesh loop; do NOT touch mesh state here
}

static void addMetric(ZigbeeAnalog& ep, const char* desc) {
  ep.addAnalogInput();
  ep.setAnalogInputDescription(desc);
  ep.setAnalogInputResolution(0.1);
  Zigbee.addEndpoint(&ep);
}

void zigbeeMetricsBegin(const char* node_name) {
  if (s_started) return;

  // First endpoint carries the device identity shown in HA.
  zbBattV.setManufacturerAndModel("MeshCore", "CombinedNode");

  addMetric(zbBattV,   "Battery V");
  addMetric(zbBattPct, "Battery %");
  addMetric(zbUptime,  "Uptime h");
  addMetric(zbRx,      "RX packets");
  addMetric(zbRelayed, "Relayed");
  addMetric(zbDropped, "Dropped");
  addMetric(zbNeigh,   "Neighbours");
  addMetric(zbRssi,    "RSSI dBm");
  addMetric(zbSnr,     "SNR dB");
  addMetric(zbHeap,    "Heap kB");

  zbRelay.onLightChange(onRelayChange);
  Zigbee.addEndpoint(&zbRelay);

  // End device: joins any open network; keeps (re)trying in its own task, so
  // this must not block the mesh loop waiting for a coordinator.
  if (Zigbee.begin(ZIGBEE_END_DEVICE)) {
    s_started = true;
    Serial.println("[zigbee] stack started, joining...");
  } else {
    Serial.println("[zigbee] ERROR: stack failed to start");
  }
  (void)node_name;
}

void zigbeeMetricsUpdate(const ZigbeeMetricsSnapshot& s) {
  if (!s_started || !Zigbee.connected()) return;

  zbBattV.setAnalogInput(s.batt_mv / 1000.0f);
  zbBattPct.setAnalogInput(s.batt_pct);
  zbUptime.setAnalogInput(s.uptime_hours);
  zbRx.setAnalogInput((float)s.rx_count);
  zbRelayed.setAnalogInput((float)s.relayed);
  zbDropped.setAnalogInput((float)s.dropped);
  zbNeigh.setAnalogInput(s.neighbours);
  zbRssi.setAnalogInput(s.last_rssi);
  zbSnr.setAnalogInput(s.last_snr);
  zbHeap.setAnalogInput(s.free_heap / 1024.0f);

  zbBattV.reportAnalogInput();
  zbBattPct.reportAnalogInput();
  zbUptime.reportAnalogInput();
  zbRx.reportAnalogInput();
  zbRelayed.reportAnalogInput();
  zbDropped.reportAnalogInput();
  zbNeigh.reportAnalogInput();
  zbRssi.reportAnalogInput();
  zbSnr.reportAnalogInput();
  zbHeap.reportAnalogInput();

  zbRelay.setLight(s.relay_on);   // reflect actual relay state back to HA
}

bool zigbeeMetricsConnected() {
  return s_started && Zigbee.connected();
}

int zigbeeMetricsConsumePendingRelay() {
  int v = s_pending_relay;
  s_pending_relay = -1;
  return v;
}

void zigbeeMetricsFactoryReset() {
  Serial.println("[zigbee] factory reset -> rebooting into pairing");
  Zigbee.factoryReset();   // wipes zb_storage and restarts
}

#endif // WITH_ZIGBEE_METRICS
