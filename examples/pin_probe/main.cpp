// One-off hardware probe: find where the Wio-SX1262 is wired on a XIAO ESP32-C6.
// Brute-forces NSS/RESET over the XIAO pad GPIOs and reads SX126x register
// 0x0320 (version string, starts "SX126") over SPI(19,20,18) = D8/D9/D10.
// Not part of any shipped firmware; flashed manually, then deleted.
#include <Arduino.h>
#include <SPI.h>
#include <ctype.h>

static SPIClass spi(0);
// XIAO C6 pad GPIOs: D0=0 D1=1 D2=2 D3=21 D4=22 D5=23 D6=16 D7=17
static const int CAND[] = {0, 1, 2, 21, 22, 23, 16, 17};
static const int NC = sizeof(CAND) / sizeof(CAND[0]);

static void readReg(int nss, uint16_t addr, uint8_t* buf, int n) {
  digitalWrite(nss, LOW);
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  spi.transfer(0x1D);                    // ReadRegister
  spi.transfer(addr >> 8);
  spi.transfer(addr & 0xFF);
  spi.transfer(0);                       // status
  for (int i = 0; i < n; i++) buf[i] = spi.transfer(0);
  spi.endTransaction();
  digitalWrite(nss, HIGH);
}

static void probe(int sck, int miso, int mosi) {
  spi.end();
  spi.begin(sck, miso, mosi);
  Serial.printf("[probe] SPI sck=%d miso=%d mosi=%d\n", sck, miso, mosi);
  for (int r = 0; r < NC; r++) {
    for (int c = 0; c < NC; c++) {
      if (r == c) continue;
      int RST = CAND[r], NSS = CAND[c];
      if (RST == sck || RST == miso || RST == mosi) continue;
      if (NSS == sck || NSS == miso || NSS == mosi) continue;
      for (int i = 0; i < NC; i++) pinMode(CAND[i], INPUT);   // release all
      pinMode(NSS, OUTPUT); digitalWrite(NSS, HIGH);
      pinMode(RST, OUTPUT);
      digitalWrite(RST, LOW); delay(5); digitalWrite(RST, HIGH); delay(30);
      uint8_t buf[16] = {0};
      readReg(NSS, 0x0320, buf, 16);
      bool hit = buf[0] == 'S' && buf[1] == 'X';
      if (hit || (buf[0] != 0x00 && buf[0] != 0xFF)) {
        char txt[17];
        for (int i = 0; i < 16; i++) txt[i] = isprint(buf[i]) ? buf[i] : '.';
        txt[16] = 0;
        Serial.printf("  RST=%-2d NSS=%-2d -> %02x%02x%02x%02x '%s'%s\n",
                      RST, NSS, buf[0], buf[1], buf[2], buf[3], txt,
                      hit ? "   <<< SX126x FOUND" : "");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(4000);
  Serial.println("[probe] start");
  probe(19, 20, 18);   // D8/D9/D10 (expected)
  probe(19, 18, 20);   // miso/mosi swapped, just in case
  Serial.println("[probe] done");
}

void loop() { delay(1000); }
