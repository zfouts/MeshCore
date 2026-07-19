// fleet_node — shared runtime state for the robustness extensions.
//
// Held by MyMesh via a single `FleetState* _fleet` pointer (see the
// WITH_FLEET_EXTRAS block in companion_radio/MyMesh.h) so the companion
// header only gains one member. All fleet_node .cpp files include this.
//
// Tunables are compile-time build flags (override in the env build_flags); a
// car repeater is set-and-flash, so there is intentionally no over-the-mesh
// admin surface.

#pragma once

#include <stdint.h>
#include "RateLimiter.h"

// ---- build-time tunables (override any of these in the env build_flags) ----
#ifndef FLEET_LOW_BATT_MV
#define FLEET_LOW_BATT_MV 0               // auto power-off below this (0 = DISABLED).
#endif                                       // Off by default: a car/vehicle repeater
                                             // must never deep-sleep itself. Set >0 only
                                             // for genuinely battery-powered deployments.
#ifndef FLEET_LOW_BATT_FLOOR
#define FLEET_LOW_BATT_FLOOR 2500         // below this we assume "no battery / USB" and
#endif                                       // never shut down (avoids a floating ADC trip)
#ifndef FLEET_GPS_INTERVAL_S
#define FLEET_GPS_INTERVAL_S 120          // GPS refresh interval while mobile
#endif
#ifndef FLEET_ADVERT_INTERVAL_S
#define FLEET_ADVERT_INTERVAL_S 900       // periodic zero-hop location advert (0 = off)
#endif
#ifndef FLEET_WDT_TIMEOUT_S
#define FLEET_WDT_TIMEOUT_S 30            // hardware watchdog timeout (ESP32)
#endif
#ifndef FLEET_BOT_RATE_MAX
#define FLEET_BOT_RATE_MAX 6              // max bot replies ...
#endif
#ifndef FLEET_BOT_RATE_SECS
#define FLEET_BOT_RATE_SECS 60            // ... per this many seconds
#endif
#ifndef FLEET_LOW_BATT_STRIKES
#define FLEET_LOW_BATT_STRIKES 5          // consecutive low reads before shutdown (debounce)
#endif
#ifndef FLEET_LOWBATT_BEACON
#define FLEET_LOWBATT_BEACON 0            // 1 = broadcast a "going dark" msg on the bot
#endif                                       // channel just before low-batt shutdown (solar nodes).
#ifndef FLEET_LOWBATT_BEACON_DRAIN_MS
#define FLEET_LOWBATT_BEACON_DRAIN_MS 5000 // max time to wait for the beacon to actually TX
#endif                                        // before sleeping (covers flood pre-TX random delay)
// NOTE: bot_enabled / bot_control_channel are persisted in companion's NodePrefs
// (reliable, dedicated prefs storage) rather than here -- the DataStore blob
// store rejects sub-100-byte records and evicts by age, so it can't hold them.

// Single-cell LiPo cell voltage (mV) -> approximate state-of-charge percent
// (0..100). All fleet_node targets run single-cell LiPo packs. Instantaneous
// reading, so it sags under TX load -- a rough gauge, not a coulomb-counter.
uint8_t fleetLipoPercent(uint16_t mv);

struct FleetState {
  RateLimiter       bot_limiter{FLEET_BOT_RATE_MAX, FLEET_BOT_RATE_SECS};
  uint32_t          next_advert_ms;   // when to send the next periodic advert
  uint32_t          advert_interval_s; // runtime advert cadence (`@name set advert_interval`, 0 = off)
  uint32_t          reboot_at_ms;     // deferred `@name reboot` deadline (0 = not armed)
  char              boot_reason[14];  // why we booted, captured once at begin
  uint8_t           low_batt_strikes;
  bool              wdt_on;
};
