# remote_test_companion

A companion (chat) node you can drive from somewhere else on the mesh. It joins
a **private control room** (a group channel with its own key) and, when someone
in that room says

    !msg #Public hello from the far side

it posts `hello from the far side` into `#Public` as itself. It also relays
(repeat mode) on the 902-928 MHz US/CA band, so the same box can sit on a hill
as a repeater and double as a remote test transmitter.

## What it is (and is not)

`examples/companion_radio` builds **unchanged** with two additions:

| Piece | Where |
|---|---|
| `-include RemoteTestCompanion.h` | widens the repeat-frequency gate to 902-928 MHz and names the hook function |
| `RemoteTestCompanion.cpp` | the bot: parses commands from the control room, posts and acknowledges |
| `COMPANION_CHANNEL_MSG_HOOK` | a 4-line, `#ifdef`-guarded call at the end of `MyMesh::onChannelMessageRecv` in companion_radio; a no-op in every other build |

Nothing else is forked, so these builds pick up companion_radio fixes for free.
The repeat-gate half is the same trick `examples/repeater_companion` uses.

## Build and flash

### Envs

| Board | Envs |
|---|---|
| Heltec V4 | `heltec_v4_remote_test_companion_ble`, `heltec_v4_remote_test_companion_usb` |
| XIAO ESP32-S3 + Wio SX1262 | `Xiao_S3_WIO_remote_test_companion_ble`, `Xiao_S3_WIO_remote_test_companion_usb` |
| XIAO ESP32-C6 + Wio SX1262 | `Xiao_C6_remote_test_companion_ble` |
| XIAO nRF52840 + Wio SX1262 | `Xiao_nrf52_remote_test_companion_ble`, `Xiao_nrf52_remote_test_companion_usb` |
| Seeed Wio Tracker L1 | `WioTrackerL1_remote_test_companion_ble`, `WioTrackerL1_remote_test_companion_usb` |

`_ble` talks to the phone app over Bluetooth (PIN `123456`); `_usb` exposes the
companion protocol on the USB serial port instead. Both accept meshcli over USB
for setup. Pick `_ble` for a node you will carry, `_usb` for one that lives on a
bench or behind a Raspberry Pi.

### Build

From the repo root, with [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
installed:

    pio run -e Xiao_nrf52_remote_test_companion_ble

The first build downloads the toolchain and takes a few minutes; later builds
are incremental. Output lands in `.pio/build/<env>/`.

To build every remote_test_companion (and repeater_companion) env with
versioned, release-style artifacts in `out/`:

    FIRMWARE_VERSION=v1.17.1 bash build.sh build-fleet-firmwares

That produces `<env>-<version>-<sha>.uf2` for nRF52 boards and
`<env>-<version>-<sha>.bin` plus a `-merged.bin` for ESP32 boards.

### Flash

**Any board, over USB with PlatformIO** (auto-detects the port; add
`--upload-port /dev/tty...` if you have several devices attached):

    pio run -e Xiao_nrf52_remote_test_companion_ble -t upload

**nRF52 boards (XIAO nRF52840, Wio Tracker L1) by drag-and-drop**, no tools
needed on the flashing machine: double-tap the reset button so the board mounts
as a USB drive, then copy the `.uf2` from `out/` (or build one with
`python3 bin/uf2conv/uf2conv.py .pio/build/<env>/firmware.hex -c -o firmware.uf2 -f 0xADA52840`)
onto it. The drive ejects itself and the node reboots into the new firmware.

**ESP32 boards (Heltec V4, XIAO S3, XIAO C6) with esptool**, if `-t upload`
cannot get the board into download mode:

    pio run -e heltec_v4_remote_test_companion_ble -t mergebin
    esptool.py --chip auto --port /dev/tty.usbmodem101 --baud 460800 \
        write_flash 0x0 .pio/build/heltec_v4_remote_test_companion_ble/firmware-merged.bin

The merged image contains bootloader, partition table and app, so flashing it
at `0x0` is a complete fresh install.

Board notes:

- A fresh ESP32 install wipes the settings partition: identity, name and
  channels come back to defaults. Flashing only `firmware.bin` (what
  `-t upload` does) keeps them.
- XIAO S3: opening the USB-JTAG serial port can latch the chip in download mode.
  If the node looks dead after flashing, unplug and replug it.
- XIAO C6: the radio does not come up after a flash until the board has been
  power-cycled, so unplug it once before testing.
- Heltec V4: if `-t upload` hangs at "Connecting...", use the esptool route
  above.

### Confirm it took

Over USB with meshcli (`pipx install meshcore-cli`):

    meshcli -s /dev/tty.usbmodem101 infos
    meshcli -s /dev/tty.usbmodem101 set radio 910.525,62.5,7,5,on

The version reported by `infos` should carry the build's commit hash when built
through `build.sh`, and the `set radio ... ,on` line must come back OK: on a
stock companion build it fails with an illegal-argument error because 910.525
is outside the default repeat gate. Then follow Setup.

## Setup

1. Flash a `*_remote_test_companion_{ble,usb}` env.
2. Give the node a name (`set name Solar-7`).
3. Create the control room on the node **and** on the phone or CLI you will
   drive it from: same channel name, same key. The default name is `control`
   (leading `#` and case are ignored). With meshcli:

       add_channel control <hex key>

   Pick a random key and keep it private. Whoever holds it can make the node
   transmit.
4. Optionally turn relaying on (5th radio argument):

       set radio 910.525,62.5,7,5,on

   Omitting the 5th argument keeps the current setting; check with `infos`.

## Commands

Sent as ordinary messages in the control room. Case-insensitive.

| Command | Effect |
|---|---|
| `!msg #<channel> <text>` | post `<text>` into `<channel>` as this node. The channel must already exist on the node (`#Public` always does). |
| `!ping` | reply with the SNR and hop count of your request, useful to confirm the room is reachable before sending. |
| `@<node name> !msg ...` | address one node when several share a room; the others stay silent. |

Every command is acknowledged back into the room, tagged `@<sender>`:

    @Alice sent to #Public as Solar-7: hello from the far side
    @Alice no channel named #austin on Solar-7
    @Alice pong from Solar-7 (snr 8.5, 2 hops)

Plain chatter in the room (anything not starting with `!` or `@<name> !`) is
ignored. Commands are rate-limited to one every 3 s per node
(`REMOTE_TEST_MIN_INTERVAL_MS`); extra ones are dropped silently. The bot will
not post into the control room itself.

## Build-time knobs

Pass with `-D` in the env, or edit `RemoteTestCompanion.h`.

| Macro | Default | Meaning |
|---|---|---|
| `REMOTE_TEST_CONTROL_CHANNEL` | `"control"` | name of the control room (quote it: `-D REMOTE_TEST_CONTROL_CHANNEL='"ops"'`) |
| `REMOTE_TEST_REPEAT_LO_KHZ` / `_HI_KHZ` | `902000` / `928000` | band on which repeat mode may be enabled |
| `REMOTE_TEST_MIN_INTERVAL_MS` | `3000` | minimum spacing between accepted commands |

## Regulatory note

The 902-928 MHz default is the US/CA ISM band. The firmware does not know where
it is; narrow or move the range if you deploy elsewhere.
