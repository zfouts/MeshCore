// Minimal WiFi-only probe -- the Wio/SX1262 is never initialized or touched.
// Sweeps candidate antenna-switch GPIOs (excluding USB pins 12/13!) plus the
// official GPIO3/GPIO14 pair, scanning WiFi under each config.
#include <Arduino.h>
#include <WiFi.h>
static const int CAND[] = {3, 14, 15, 10, 11, 4, 5, 6, 7, 0};
static const int NC = sizeof(CAND)/sizeof(CAND[0]);
void setup() {
  Serial.begin(115200);
  delay(4000);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}
static int scanN() { int n = WiFi.scanNetworks(); WiFi.scanDelete(); return n; }
void loop() {
  for (int i = 0; i < NC; i++) pinMode(CAND[i], INPUT);
  delay(200);
  Serial.printf("[rf] baseline -> %d networks\n", scanN());
  pinMode(3, OUTPUT); digitalWrite(3, LOW); delay(100);
  for (int v = 0; v <= 1; v++) {
    pinMode(14, OUTPUT); digitalWrite(14, v); delay(200);
    Serial.printf("[rf] G3=0 G14=%d -> %d networks\n", v, scanN());
  }
  pinMode(3, INPUT); pinMode(14, INPUT);
  for (int i = 0; i < NC; i++) {
    for (int v = 0; v <= 1; v++) {
      for (int j = 0; j < NC; j++) pinMode(CAND[j], INPUT);
      pinMode(CAND[i], OUTPUT); digitalWrite(CAND[i], v);
      delay(200);
      int n = scanN();
      Serial.printf("[rf] GPIO%d=%d -> %d%s\n", CAND[i], v, n, n>0 ? "  <<< WORKS" : "");
    }
  }
  Serial.println("[rf] ---- sweep done ----");
  delay(5000);
}
