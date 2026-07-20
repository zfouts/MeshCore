#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>

class XiaoC6Board : public ESP32Board {
public:
  void begin() {
    ESP32Board::begin();

#ifdef USE_XIAO_ESP32C6_EXTERNAL_ANTENNA
// Connect an external antenna to your XIAO ESP32C6 otherwise, it may be damaged!
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW); // Activate RF switch control

    delay(100);

    pinMode(14, OUTPUT);
    digitalWrite(14, HIGH); // Use external antenna
#else
    // The XIAO C6's 2.4GHz RF switch is unpowered until GPIO3 is driven LOW --
    // without this WiFi/BLE/Zigbee transmit into nothing (the generic
    // esp32-c6-devkitm-1 board profile doesn't run Seeed's initVariant()).
    // GPIO14 LOW selects the built-in ceramic antenna.
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);  // power the RF switch
    delay(100);
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW); // built-in antenna
#endif
  }

  const char* getManufacturerName() const override {
    return "Xiao C6";
  }
};


