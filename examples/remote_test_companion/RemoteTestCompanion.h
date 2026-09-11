// remote_test_companion -- build-time configuration, force-included (`-include`)
// into every translation unit of a stock companion_radio build.
//
// This file is pure preprocessor on purpose: `-include` applies to the C files
// in the Arduino core too, so nothing here may need a C++ compiler.
//
// Two things happen here:
//
//  1. ALLOWED_REPEAT_FREQ_RANGE widens the frequencies on which repeat mode may
//     be ENABLED. Stock companion_radio only allows it on three exact spot
//     frequencies (433.0 / 869.495 / 918.0 MHz), so a US/CA node on 910.525 MHz
//     gets ERR_CODE_ILLEGAL_ARG from `set radio ...,on`. The default below is the
//     whole 902-928 MHz US/CA ISM band; override the LO/HI values with -D if you
//     are somewhere else.
//
//  2. COMPANION_CHANNEL_MSG_HOOK names the function companion_radio calls for
//     every decrypted channel message (after queueing it for the app). Ours is
//     the "!msg" bot in RemoteTestCompanion.cpp.

#pragma once

#ifndef REMOTE_TEST_REPEAT_LO_KHZ
#define REMOTE_TEST_REPEAT_LO_KHZ 902000
#endif
#ifndef REMOTE_TEST_REPEAT_HI_KHZ
#define REMOTE_TEST_REPEAT_HI_KHZ 928000
#endif
#ifndef ALLOWED_REPEAT_FREQ_RANGE
#define ALLOWED_REPEAT_FREQ_RANGE \
  { REMOTE_TEST_REPEAT_LO_KHZ, REMOTE_TEST_REPEAT_HI_KHZ }
#endif

#ifndef COMPANION_CHANNEL_MSG_HOOK
#define COMPANION_CHANNEL_MSG_HOOK remoteTestOnChannelMsg
#endif

// Name of the private channel that acts as the control room. Compared without
// any leading '#' and case-insensitively, so "control", "#control" and
// "#Control" all match. Override with -D REMOTE_TEST_CONTROL_CHANNEL='"ops"'.
#ifndef REMOTE_TEST_CONTROL_CHANNEL
#define REMOTE_TEST_CONTROL_CHANNEL "control"
#endif

// Minimum spacing between accepted commands. Each command costs two transmits
// (the relayed message plus the acknowledgement), so this caps the airtime a
// chatty operator -- or a typo loop -- can burn.
#ifndef REMOTE_TEST_MIN_INTERVAL_MS
#define REMOTE_TEST_MIN_INTERVAL_MS 3000
#endif
