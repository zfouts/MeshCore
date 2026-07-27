// repeater_companion — widen the frequencies allowed to enable repeat mode.
//
// There is no C++ in this "example": repeater_companion IS examples/companion_radio,
// built with this one header force-included (`-include`). Stock companion_radio
// already has everything else -- the `client_repeat` pref, its persistence, and
// allowPacketForward() reading it live per packet (companion_radio/MyMesh.cpp:486).
//
// The ONLY thing stock companion_radio won't do is let you turn repeat ON at an
// arbitrary frequency: CMD_SET_RADIO_PARAMS rejects it unless the frequency falls
// in repeat_freq_ranges[] (MyMesh.cpp:993), which defaults to just three exact
// spot frequencies -- 433.0, 869.495 and 918.0 MHz. A US node on 910.525 MHz gets
// ERR_CODE_ILLEGAL_ARG and repeat silently stays off. That single gate is the
// entire reason this build exists.
//
// Defined in a header rather than a -D flag because brace/comma initializer values
// get mangled passing through PlatformIO/shell quoting.
//
// Values are kHz, as {lower, upper} pairs. The range below spans everything the
// radio itself accepts, so repeat can be enabled on whatever frequency the node is
// already running. That is deliberate -- it puts frequency choice entirely in the
// operator's hands. REGULATORY NOTE: nothing here checks that your frequency is
// legal to repeat on in your region; narrow these bounds if you want the firmware
// to enforce that for you (e.g. { 902000, 928000 } for US/CA ISM only).

#pragma once

#ifndef ALLOWED_REPEAT_FREQ_RANGE
#define ALLOWED_REPEAT_FREQ_RANGE \
  { 150000, 2500000 }
#endif
