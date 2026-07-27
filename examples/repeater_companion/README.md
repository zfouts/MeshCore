# repeater_companion

A companion (chat) node that can also relay, on whatever frequency it is already
running.

**There is no firmware here.** This directory holds a single header. The
`*_repeater_companion_*` build envs compile `examples/companion_radio` completely
unchanged and force-include `RepeatFreq.h` via `-include`. Nothing is forked, so
these builds inherit companion_radio fixes for free.

## Why it exists

Stock `companion_radio` already does the relaying part:

| Piece | Where |
|---|---|
| `client_repeat` pref | `companion_radio/NodePrefs.h:32` |
| persisted to flash | `companion_radio/DataStore.cpp:215,255` (offset 62) |
| read live, per packet | `companion_radio/MyMesh.cpp:486` (`allowPacketForward`) |
| set at runtime | `companion_radio/MyMesh.cpp:1396` (`CMD_SET_RADIO_PARAMS`) |

The one thing it won't do is let you *enable* repeat at an arbitrary frequency.
`CMD_SET_RADIO_PARAMS` refuses unless the frequency falls inside
`repeat_freq_ranges[]` (`companion_radio/MyMesh.cpp:993`), which defaults to three
exact spot frequencies:

    { 433000, 433000 }, { 869495, 869495 }, { 918000, 918000 }

A US node on 910.525 MHz is not in that list, so it gets `ERR_CODE_ILLEGAL_ARG`
and repeat silently stays off. `RepeatFreq.h` overrides the table with the full
range the radio accepts. That gate is the entire reason this build target exists.

## Usage

Flash any `*_repeater_companion_ble` or `*_repeater_companion_usb` env, then turn
relaying on with meshcli:

    set radio 910.525,62.5,7,5,on

The 5th argument is the repeat byte. **Omitting it preserves the current setting**
— older firmware defaulted the absent byte to 0, which silently turned relaying
off during provisioning. Check the current state with `infos`.

## Regulatory note

`RepeatFreq.h` deliberately does not constrain frequency — that is the operator's
call. If you would rather the firmware enforce a band, narrow the range in that
header (e.g. `{ 902000, 928000 }` for US/CA ISM only).

## Relationship to simple_repeater

Different roles, both useful:

- `simple_repeater` — a true `ADV_TYPE_REPEATER` backbone node with the full admin
  CLI (`password`, `set advert.interval`, neighbour table, CLI-over-mesh). This is
  what deployed infrastructure nodes run.
- `repeater_companion` — a chat node that also forwards. You get a companion app /
  meshcli client on it, and it contributes hops. Good for carry nodes and bench
  work, not a substitute for a repeater on infrastructure.
