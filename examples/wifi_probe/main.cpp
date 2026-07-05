// RF-switch truth table probe: scan WiFi under every GPIO3/GPIO14 combination
// to empirically find the working antenna config on this XIAO C6 unit.
#include <Arduino.h>
#include <WiFi.h>
void setup() {
  Serial.begin(115200);
  delay(4000);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}
static const int combos[4][2] = {{0,0},{0,1},{1,0},{1,1}};
void loop() {
  for (int i = 0; i < 4; i++) {
    pinMode(3, OUTPUT);  digitalWrite(3, combos[i][0]);
    pinMode(14, OUTPUT); digitalWrite(14, combos[i][1]);
    delay(300);
    int n = WiFi.scanNetworks();
    Serial.printf("[rf] GPIO3=%d GPIO14=%d -> %d networks", combos[i][0], combos[i][1], n);
    if (n > 0) Serial.printf("  e.g. %s %ddBm", WiFi.SSID(0).c_str(), WiFi.RSSI(0));
    Serial.println(n > 0 ? "   <<< WORKS" : "");
    WiFi.scanDelete();
  }
  Serial.println("[rf] ---- sweep done, repeating ----");
  delay(3000);
}
