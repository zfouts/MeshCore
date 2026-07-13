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

- `MyMesh.h` — declares `buildBotReply()`, `_relay_count`.
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
- **Bot commands**: a **group-channel** message beginning with `!` is
  auto-replied to, on any configured bot channel (multiple supported — `set
  bot_channel +#bot` adds one) or the control channel (see below). Replies go
  back to the channel the request arrived on. **Direct messages are ignored
  entirely** — a `!` DM is just a chat message to the app. Unrecognised
  commands are ignored silently.
  - `!ping` — `pong` with how this node heard you: SNR, RSSI, hop count (or
    `direct`), and approximate one-way latency
  - `!path` — the complete route this request took to reach the node, as
    `<name> [<hops>h] <hash>,<hash>,...` — the requester's name, the hop count,
    and each traversed repeater's raw hex path-hash (public-key prefix) in
    order, e.g. `NAME [8h] 90e2,fe27,ebb0,a484,c0ff,ab2f,d1a9,d690`.
    `<name> [0h] direct` when heard straight from the sender.
    On `_wifi` builds with an aggregator configured (`set obs_url
    <base-url>` / `set obs_token <token>` over meshcli, `-` clears), the
    node also POSTs the hop chain to `/api/device/path` and appends the
    short **map URL** it gets back — the aggregator resolves each hash to a
    located node (disambiguating by chain geometry) and draws the route.
    Run your own: **[txme.sh](https://github.com/zfouts/txme.sh)** is the
    open-source aggregator that implements the device path API this bot
    speaks (see its README for `DEVICE_TOKEN` / deployment setup).
    Wardrive beacons (`!path <lat,lon>`) forward the surveyor's position so
    the map anchors both ends. Fails soft: no WiFi / no observer / timeout
    just means the plain hex reply. NOTE: the HTTP call blocks the radio
    for up to ~5 s; HTTPS is accepted without certificate validation (no CA
    bundle on-device — the device token is the auth).
  - `!help` — lists the commands this build supports. **Control-channel
    only**: on the bot channel the node stays silent, so the command list
    isn't advertised.
  - `!info` — node name, firmware version, relay on/off
  - `!uptime` — time since boot
  - `!telemetry` — battery mV, uptime, packets relayed, sensor count
  - `!stats` — rx / relayed / dropped / uptime / battery
  - `!neighbors` — directly-heard nodes and how long ago
  - `!heard <name|hex>` — when this node last directly heard one station, by
    (partial, case-insensitive) name or hex pubkey-prefix — `!path` hop
    hashes paste in as-is
  - `!batt` — battery with direction, not just a snapshot: current mV/%,
    min/max since boot, and the change over the last hour ("charging fine"
    vs "dying by Thursday")
  - `!boot` — why and how often the node booted: reset cause (`watchdog`,
    `brownout`, `wake-charged` = solar dawn revival, …) plus a persisted
    boot counter, so a brownout-looping sealed node is visible from the mesh
  - `!rf` — radio params in one line (freq / sf / bw / cr / txpower), for
    fleet-wide config audits
  - `!time` — RTC time and drift vs the sender's timestamp (includes flight
    time; seconds-level). `!time sync` (**control-channel only**) adopts the
    sender's timestamp to pull a wandered clock back in line
  - `!wifi` — WiFi status on `_wifi` builds (IP when associated); `n/a`
    elsewhere. Read-only; toggling stays with `@name set wifi`
  - `!advert` (**control-channel only**) — re-announce NOW with a flood
    advert, instead of waiting out the periodic advert timer (after moving a
    node, rotating identity, onboarding new clients)
  - `!relay on` / `!relay off` / `!relay` — enable/disable packet forwarding;
    takes effect immediately and is persisted. Bare `!relay` reports state.
  - **Control commands** (`!relay on|off`, `!ble on|off`, `!wd on|off`,
    `!advert`, `!time sync`) only
    act on the configured **control channel** (see `bot_control_channel`
    below): possession of that private channel's key is the auth, hop count
    does not matter, and one channel message can control the whole fleet. On
    the bot channel they get a `control-channel only` refusal. Deliberately
    no further auth, and these are not listed in `!help`. The bare status
    queries and all read-only commands work on either channel.
  - `!loc` — the node's location (`lat,lon`, tagged `(gps)` when GPS-sourced,
    or `not set`). **Control-channel only**: anywhere else the node stays
    silent as if the command doesn't exist (field nodes are often hidden;
    only holders of the control-channel key can ask where one lives). Also
    deliberately absent from `!help`.
  - `!wd on|off|` (status) — **wardrive mode**: while on, this node beacons
    `!path <lat,lon>` to the control channel every 45 s
    (`COMBINED_WD_INTERVAL_S`); every fleet node that hears it replies with
    the route it arrived by, so carrying the node around maps mesh coverage,
    each reply pinned to the beaconed position. Toggling requires the
    control channel (like `!relay`); runtime-only — a reboot always
    returns to quiet so a forgotten survey can't drain a node.
  - **`@<name> set <var> <value>`** (control-channel only) — node-targeted
    admin, vs. the bare `!relay`/`!ble` which every listening node obeys
    fleet-wide. Only the named node acts and replies; everyone else stays
    silent. Name match is case-insensitive and may contain spaces. Vars:
    - `relay on|off` — packet forwarding (persisted)
    - `ble on|off` — BLE advertising (persisted; `n/a` on non-BLE builds)
    - `txpower <dbm>` — LoRa TX power, applied live (persisted)
    - `location <lat>,<lon>` / `location off` — set/clear node location (persisted)
    - `wifi on|off` — WiFi radio on WiFi builds (**runtime-only**: a reboot
      restores WiFi so a bad toggle can't strand a node; `n/a` elsewhere)
    - `gps on|off` — GPS power knob for deployed nodes (persisted; `n/a` on
      builds without GPS)
    - `advert_interval <secs>` — periodic-advert cadence, `0` = off
      (**runtime-only**: a reboot restores the build default)
    - `obs_url <url>` / `obs_token [label:]token` —
      [txme.sh](https://github.com/zfouts/txme.sh) aggregator config for
      `!path` map links, provisionable over the mesh (no USB cable per node).
      The token value may be pasted exactly as it appears in the observer's
      labelled `DEVICE_TOKEN` secret (`garden:a1b2…`) — the label is stripped,
      only the token is stored (max 64 chars) and it is **never echoed back**
      (the reply confirms length only). It transits encrypted with the
      control channel's key: anyone holding that key can already administer
      the fleet. `-` clears. (persisted)
    - e.g. `@Solar-07 set txpower 17`, `@Xiao C6 Combined set wifi off`
  - **`@<name> reboot`** (control-channel only) — the remote unstick for a
    sealed node. Replies `rebooting in 5s`, then reboots after the reply has
    had time to get on the air. Targeted-only by design: there is no bare
    `!reboot`, so one fat-fingered message can't bounce the whole fleet.

### Bot configuration (via meshcore-cli)

Three settings are exposed as custom vars and persisted, so you configure the
bot locally over BLE/USB (not over the mesh):

| Var | Values | Meaning |
|-----|--------|---------|
| `bot_enable`  | `0` / `1` | master enable for the bot |
| `bot_channel` | channel name, index, or `off`; `+<chan>` / `-<chan>` add/remove | group channel(s) the bot answers on — the bot is **multi-channel**: a bare value replaces the whole set, `+#bot` adds one, `-#bot` removes one (`get` lists them `+`-joined). The plain value also stays the **primary** channel (low-battery beacon target). `off` = control channel only |
| `bot_path_channel` | same syntax as `bot_channel` | channels where **only `!path`** answers — every other command stays silent, as if the bot weren't there, and **0-hop (direct) requests are ignored too** (nothing to map). For public channels (e.g. `#bot`) where you want route mapping without exposing the whole command surface. A channel in both sets gets full commands |
| `bot_control_channel` | channel name, index, or `off` | channel authorized for **control writes** (`!relay`/`!ble on|off`) at any hop count — use a **private** channel; anyone holding its key controls the node |

```
meshcore-cli get bot_enable bot_channel     # read current values
meshcore-cli set bot_channel "#bot"         # answer on a named channel (auto-joins if missing)
meshcore-cli set bot_channel 0              # ...or by index
meshcore-cli set bot_channel off            # disable channel bot (DM only)
meshcore-cli set bot_enable 0               # turn the whole bot off
meshcore-cli set bot_control_channel ops,BASE64PSK  # fleet control from private channel "ops"
meshcore-cli set bot_control_channel off    # control via direct DM only
```

`bot_channel` and `bot_control_channel` accept a **channel name**, a numeric
index, `off`, or `<name>,<base64psk>` (joins a private channel with the given
16/32-byte key into a free slot and binds in one step -- handy for provisioning
the same control channel across a fleet). If you give a **hashtag channel**
name (`#name`) the node isn't joined to yet, it auto-joins by deriving the key
(`sha256("#name")[:16]`) into a free channel slot and saving it. Private
(random-key) channels can't be derived from a name -- join in the app first and
bind by name, or use the `<name>,<base64psk>` form. `get` reports channels back
by name. The bot answers read-only commands on the control channel too.

Defaults: `bot_enable=1`, `bot_channel=off`, `bot_control_channel=off`. Channel replies go back to the same
channel; the reply rate-limit applies to channels too.

### WiFi variant (`_wifi` envs, `WITH_RUNTIME_WIFI`, ESP32 boards)

Each ESP32 board has a third env alongside `_usb`/`_ble`: `_wifi` serves the
companion protocol over a WiFi TCP socket (port 5000) **and** USB. USB is the
provisioning path — credentials are custom vars, set over the same cable you
flashed with, persisted, and applied live (no reboot, no recompile):

| Var | Values | Meaning |
|-----|--------|---------|
| `wifi_ssid` | ssid, or `-` | WiFi network to join (`-` clears and turns WiFi off) |
| `wifi_pwd`  | passphrase, or `-` | WPA passphrase (`-` clears = open network) |

```
meshcore-cli set wifi_ssid MyNetwork set wifi_pwd hunter22   # over USB; applies immediately
meshcore-cli get                              # ...,wifi_ssid:MyNetwork,wifi:192.168.1.57
meshcli -t 192.168.1.57                       # then connect over TCP
meshcore-cli set wifi_ssid -                  # forget creds, WiFi radio off
```

Notes:

- `get` shows the state (`off` / `connecting` / `admin-off` / the IP once
  associated). The password itself is never echoed back.
- Both links are live at once (`SerialMux`): replies go to whichever link the
  app last talked on. `@name set wifi off` (control channel) only kills the
  WiFi radio — the USB console survives, and a reboot restores WiFi.
- An SSID/passphrase of a literal single `-` isn't supported (it's the clear
  sentinel), and values are stored in plain text in the prefs file, like the
  rest of the node config.
- The `_usb` and `_ble` envs have no WiFi code at all (keeps BLE+WiFi radio
  coexistence and RAM pressure out of the picture); `set wifi_ssid` returns an
  error there, same as on non-WiFi hardware (nRF52).
- An optional compile-time `-D WIFI_SSID` / `-D WIFI_PWD` pair still works on a
  `_wifi` env as the first-boot default, and `set wifi_ssid` overrides it.

## Build

Supported boards (each has a `_usb` and `_ble` env; ESP32 boards also `_wifi`):

```
# Heltec V4 (ESP32-S3)
pio run -e heltec_v4_combined_node_ble     # or _usb / _wifi

# Seeed XIAO S3 / Wio S3 (ESP32-S3)
pio run -e Xiao_S3_WIO_combined_node_ble   # or _usb / _wifi

# Seeed XIAO C6 (ESP32-C6)
pio run -e Xiao_C6_combined_node_ble       # or _usb / _wifi

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
