// combined_node — shared runtime state for the robustness extensions.
//
// Held by MyMesh via a single `CombinedState* _combined` pointer (see the
// WITH_COMBINED_EXTRAS block in companion_radio/MyMesh.h) so the companion
// header only gains one member. All combined_node .cpp files include this.
//
// Tunables are compile-time build flags (override in the env build_flags); a
// car repeater is set-and-flash, so there is intentionally no over-the-mesh
// admin surface.

#pragma once

#include <stdint.h>
#include "RateLimiter.h"

// ---- build-time tunables (override any of these in the env build_flags) ----
#ifndef COMBINED_LOW_BATT_MV
#define COMBINED_LOW_BATT_MV 0               // auto power-off below this (0 = DISABLED).
#endif                                       // Off by default: a car/vehicle repeater
                                             // must never deep-sleep itself. Set >0 only
                                             // for genuinely battery-powered deployments.
#ifndef COMBINED_LOW_BATT_FLOOR
#define COMBINED_LOW_BATT_FLOOR 2500         // below this we assume "no battery / USB" and
#endif                                       // never shut down (avoids a floating ADC trip)
#ifndef COMBINED_GPS_INTERVAL_S
#define COMBINED_GPS_INTERVAL_S 120          // GPS refresh interval while mobile
#endif
#ifndef COMBINED_ADVERT_INTERVAL_S
#define COMBINED_ADVERT_INTERVAL_S 900       // periodic zero-hop location advert (0 = off)
#endif
#ifndef COMBINED_WDT_TIMEOUT_S
#define COMBINED_WDT_TIMEOUT_S 30            // hardware watchdog timeout (ESP32)
#endif
#ifndef COMBINED_BOT_RATE_MAX
#define COMBINED_BOT_RATE_MAX 6              // max bot replies ...
#endif
#ifndef COMBINED_BOT_RATE_SECS
#define COMBINED_BOT_RATE_SECS 60            // ... per this many seconds
#endif
#ifndef COMBINED_MAX_NEIGHBOURS
#define COMBINED_MAX_NEIGHBOURS 16           // size of the heard-neighbour table
#endif
#ifndef COMBINED_LOW_BATT_STRIKES
#define COMBINED_LOW_BATT_STRIKES 5          // consecutive low reads before shutdown (debounce)
#endif
#ifndef COMBINED_LOWBATT_BEACON
#define COMBINED_LOWBATT_BEACON 0            // 1 = broadcast a "going dark" msg on the bot
#endif                                       // channel just before low-batt shutdown (solar nodes).
#ifndef COMBINED_LOWBATT_BEACON_DRAIN_MS
#define COMBINED_LOWBATT_BEACON_DRAIN_MS 5000 // max time to wait for the beacon to actually TX
#endif                                        // before sleeping (covers flood pre-TX random delay)
// NOTE: bot_enabled / bot_channel are persisted in companion's NodePrefs
// (reliable, dedicated prefs storage) rather than here -- the DataStore blob
// store rejects sub-100-byte records and evicts by age, so it can't hold them.

// Single-cell LiPo cell voltage (mV) -> approximate state-of-charge percent
// (0..100). All combined_node targets run single-cell LiPo packs. Instantaneous
// reading, so it sags under TX load -- a rough gauge, not a coulomb-counter.
uint8_t combinedLipoPercent(uint16_t mv);

struct CombinedNeighbour {
  uint8_t  prefix[4];           // first 4 bytes of pubkey
  char     name[20];
  uint32_t last_heard;          // RTC seconds
  bool     used;
};

struct CombinedStats {
  uint32_t rx_count;            // raw packets received
  uint32_t relayed;            // packets we forwarded
  uint32_t dropped;            // floods dropped by relay policy
  uint32_t boot_rtc;           // RTC time at boot
};

struct CombinedState {
  CombinedStats     stats;
  CombinedNeighbour neighbours[COMBINED_MAX_NEIGHBOURS];
  RateLimiter       bot_limiter{COMBINED_BOT_RATE_MAX, COMBINED_BOT_RATE_SECS};
  uint32_t          next_advert_ms;   // when to send the next periodic advert
  float             last_rssi;        // RSSI/SNR of the most recent raw RX (for !ping)
  float             last_snr;
  uint8_t           low_batt_strikes;
  bool              wdt_on;
};
