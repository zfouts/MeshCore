// One-off diagnostic: scan for Zigbee networks and print PAN ID / channel /
// permit-joining for everything in range. Answers "is the coordinator visible
// and open?" without any join attempt. Flash via the *_combined_node_zigbee
// env with a src-filter override; not part of any shipped firmware.
#include <Arduino.h>
#include "Zigbee.h"

static void printScan(uint16_t found) {
  if (found == 0) {
    Serial.println("[scan] no zigbee networks heard");
    return;
  }
  zigbee_scan_result_t* r = Zigbee.getScanResult();
  Serial.printf("[scan] %u network(s):\n", found);
  Serial.println("  PAN    | CH | PermitJoin | RouterCap | EDCap");
  for (int i = 0; i < found; i++) {
    Serial.printf("  0x%04x | %2u | %-10s | %-9s | %s\n",
                  r[i].short_pan_id, r[i].logic_channel,
                  r[i].permit_joining ? "YES" : "no",
                  r[i].router_capacity ? "yes" : "no",
                  r[i].end_device_capacity ? "yes" : "no");
  }
  Zigbee.scanDelete();
}

void setup() {
  Serial.begin(115200);
  delay(4000);
  // Stack must be running before a scan (no endpoints needed).
  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    Serial.println("[scan] zigbee stack failed to start");
    return;
  }
  Serial.println("[scan] starting zigbee network scan (all channels)...");
  Zigbee.scanNetworks();
}

void loop() {
  int16_t st = Zigbee.scanComplete();
  if (st >= 0) {
    printScan(st);
    Serial.println("[scan] rescanning in 10s...");
    delay(10000);
    Zigbee.scanNetworks();
  } else if (st == -2) {
    Serial.println("[scan] scan failed, retrying...");
    delay(5000);
    Zigbee.scanNetworks();
  }
  delay(500);
}
