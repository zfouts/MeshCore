// fleet_node — allowed frequencies for repeat (relay) mode.
//
// Force-included via the env's `-include` flag so this expands cleanly into
// companion_radio's `repeat_freq_ranges[]`. We define it in a header rather
// than a -D build flag because brace/comma initializer values get mangled
// passing through PlatformIO/shell quoting.
//
// Values are in kHz, as {lower, upper} pairs. The US 902-928 MHz ISM band is
// included so US channels (e.g. 910.525 MHz) are allowed to enable repeat.

#pragma once

#ifndef ALLOWED_REPEAT_FREQ_RANGE
#define ALLOWED_REPEAT_FREQ_RANGE \
  { 433000, 433000 }, \
  { 869495, 869495 }, \
  { 902000, 928000 }
#endif
