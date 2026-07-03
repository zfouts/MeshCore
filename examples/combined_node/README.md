# combined_node

A firmware that is **companion radio + relay + extended telemetry/bot commands**
in one node. It is built as an *extension* of `examples/companion_radio`, not a
fork of it, so upstream improvements to companion_radio flow straight through.

## How it works

The `*_combined_node_*` build environments compile the **unmodified**
companion_radio sources together with this directory's `BotCommands.cpp`, and
define `-D WITH_BOT_COMMANDS=1`.

All bot/relay/telemetry logic lives here in `BotCommands.cpp`. The only changes
in `companion_radio` are small `#ifdef WITH_BOT_COMMANDS` hooks that are inert
in every existing environment:

- `MyMesh.h` — declares `handleBotCommand()`, `sendBotReply()`, `_relay_count`.
- `MyMesh.cpp::onMessageRecv()` — dispatches incoming DMs to the bot (the
  message is still delivered to the companion app as normal).
- `MyMesh.cpp::allowPacketForward()` — counts relayed packets.
- `MyMesh` constructor — enables relay (`client_repeat`) by default on first
  boot (gated by `RELAY_DEFAULT_ON`).

This keeps the merge surface against upstream to a handful of guarded lines.

## Dual role

This is designed to run as a **mobile / car repeater that is also your personal
companion**. Both roles are active at once:

- As a **companion** it speaks the full companion_radio app protocol over
  BLE/USB/WiFi — your phone pairs to it normally.
- As a **repeater** it carries other people's traffic to extend range, using a
  repeater-grade forward policy (hop limits + loop detection), not just the
  noisy plain `client_repeat`.

Relaying is content-agnostic: the node forwards encrypted packets it cannot
read, so everyone on the mesh benefits, not only your contacts.

## Relay policy (`RelayPolicy.cpp`, `WITH_RELAY_POLICY`)

Refines the plain `client_repeat` relay so it behaves like a real repeater:

- `RELAY_FLOOD_MAX` (64) — drop a flood packet once it has travelled this many hops.
- `RELAY_FLOOD_MAX_UNSCOPED` (64) — same, for un-scoped `ROUTE_TYPE_FLOOD` packets.
- `RELAY_FLOOD_MAX_ADVERT` (8) — adverts kept low so they don't blanket the mesh.
- `RELAY_LOOP_DETECT` (2) — loop detection: `0`=off `1`=minimal `2`=moderate `3`=strict.

Override any of these in the env's `build_flags`. Transport-code/region (scope)
enforcement is intentionally **not** included — a car repeater should carry
un-scoped floods to extend range for everyone, and companion has no `region_map`.

## Robustness extensions (`CombinedNode.cpp`, `WITH_COMBINED_EXTRAS`)

Built for an *unattended, mobile* node:

- **Hardware watchdog** (ESP32) — auto-reboots if `loop()` stalls for
  `COMBINED_WDT_TIMEOUT_S` (30s). Nothing else in MeshCore uses a watchdog.
- **Low-battery auto power-off** — `board.powerOff()` when battery stays below
  `COMBINED_LOW_BATT_MV` (3300mV) for several consecutive reads (debounced).
  (Stock companion never implemented this even though the flag existed.)
- **Mobile GPS advertising** — first boot enables GPS + shares live location in
  adverts, and sends a periodic zero-hop advert (`advint`, default 900s) so the
  mesh tracks the node as it moves.
- **Heard-neighbour table + stats** — tracks directly-heard (0-hop) nodes, plus
  RX / relayed / dropped counters, exposed read-only over the mesh via
  `!stats` / `!neighbors`.
- **Bot reply rate-limiting** — caps auto-replies (`COMBINED_BOT_RATE_MAX` per
  `COMBINED_BOT_RATE_SECS`) so a node can't drain airtime/battery by spamming.

All tunables (relay limits, low-batt threshold, advert interval, etc.) are
compile-time build flags — set and flash. There is intentionally **no
over-the-mesh admin surface**; runtime configuration is done locally through the
companion app.

> **Not yet included: OTA.** WiFi OTA (AsyncElegantOTA) needs an on-device WiFi
> test pass and interacts with companion's WiFi app-link selection, so it's a
> deliberate follow-up rather than shipped untested.

## Features

- **Companion**: full companion_radio app protocol (BLE/USB/WiFi), contacts,
  channels, telemetry requests.
- **Relay**: `client_repeat` defaults on + repeater-grade `RelayPolicy`
  (limits via build flags). Relay on/off still toggleable from the companion app.
- **Bot commands**: a message beginning with `!` is auto-replied to — in a
  **direct message** always, and in a **group channel** if that channel is set
  as the bot channel (see below). Unrecognised commands are ignored silently.
  - `!ping` — `pong` with how this node heard you: SNR, RSSI, hop count (or
    `direct`), and approximate one-way latency
  - `!path` — the complete route this request took to reach the node, as
    `<name> [#<hops>] <hash>,<hash>,...` — the requester's name, the hop
    count, and each traversed repeater's path-hash in hex (traversal order),
    or `<name> [#0] direct` when heard straight from the sender with no relay
  - `!info` — node name, firmware version, relay on/off
  - `!uptime` — time since boot
  - `!telemetry` — battery mV, uptime, packets relayed, sensor count
  - `!stats` — rx / relayed / dropped / uptime / battery
  - `!neighbors` — directly-heard nodes and how long ago
  - `!relay on` / `!relay off` / `!relay` — enable/disable packet forwarding
    (a control command); takes effect immediately and is persisted. Bare
    `!relay` reports the current state.

### Bot configuration (via meshcore-cli)

Two settings are exposed as custom vars and persisted, so you configure the bot
locally over BLE/USB (not over the mesh):

| Var | Values | Meaning |
|-----|--------|---------|
| `bot_enable`  | `0` / `1` | master enable for the bot (DMs + channel) |
| `bot_channel` | channel name, index, or `off` | group channel the bot answers on (`off` = DM only) |

```
meshcore-cli get bot_enable bot_channel     # read current values
meshcore-cli set bot_channel "#lhtx-test"   # answer on a named channel (auto-joins if missing)
meshcore-cli set bot_channel 0              # ...or by index
meshcore-cli set bot_channel off            # disable channel bot (DM only)
meshcore-cli set bot_enable 0               # turn the whole bot off
```

`bot_channel` accepts a **channel name**, a numeric index, or `off`. If you give
a **hashtag channel** name (`#name`) the node isn't joined to yet, it auto-joins
by deriving the key (`sha256("#name")[:16]`) into a free channel slot and saving
it. Private (random-key) channels can't be derived from a name -- join those in
the app first, then set `bot_channel` to them by name. `get` reports the channel
back by name.

Defaults: `bot_enable=1`, `bot_channel=off`. Channel replies go back to the same
channel; the reply rate-limit applies to channels too.

## Build

Supported boards (each has a `_usb` and `_ble` env):

```
# Heltec V4 (ESP32-S3)
pio run -e heltec_v4_combined_node_ble     # or _usb

# Seeed XIAO S3 / Wio S3 (ESP32-S3)
pio run -e Xiao_S3_WIO_combined_node_ble   # or _usb

# Seeed XIAO nRF52840
pio run -e Xiao_nrf52_combined_node_ble    # or _usb
```

The watchdog uses `esp_task_wdt` on ESP32 and the nRF52 hardware WDT on nRF52
(paused while the CPU sleeps so the idle loop can't trip it). GPS location-advert
only applies on boards that define `ENV_INCLUDE_GPS` (e.g. Heltec V4); it is inert
elsewhere. To add another board, copy that variant's `companion_radio` env and add
`-D WITH_BOT_COMMANDS -D WITH_RELAY_POLICY -D WITH_COMBINED_EXTRAS`,
`-include examples/combined_node/CombinedRepeatFreq.h`, and
`+<../examples/combined_node/*.cpp>`.

## Configuration flags

- `WITH_BOT_COMMANDS` — enables the bot extension (set by the combined envs).
- `BOT_CMD_PREFIX` — command prefix char (default `!`).
- `RELAY_DEFAULT_ON` — relay enabled on first boot (default `1`).
- `WITH_RELAY_POLICY` — enables repeater-grade forward filtering (set by the combined envs).
- `RELAY_FLOOD_MAX` / `RELAY_FLOOD_MAX_UNSCOPED` / `RELAY_FLOOD_MAX_ADVERT` — flood hop limits.
- `RELAY_LOOP_DETECT` — loop detection level (`0`–`3`).
