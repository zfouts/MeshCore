# combined_node changelog

## Unreleased
- **Zigbee metrics bridge** (`Xiao_C6_combined_node_zigbee`, ESP32-C6): the node
  joins a Home Assistant Zigbee network (ZHA/Z2M) as an end device and reports
  mesh telemetry as sensor entities -- battery V/%, uptime, rx/relayed/dropped
  packets, neighbour count, last RSSI/SNR, free heap -- plus an On/Off endpoint
  bound to the LoRa relay so HA can toggle repeating. `set zigbee.reset 1`
  re-enters pairing; `get zigbee` reports joined/joining. USB companion, no BLE
  (Zigbee shares the C6's 2.4GHz radio). Pins pioarduino 55.03.39 (Arduino
  core 3.3) for ZigbeeAnalog; uses the stock zigbee.csv partition table (NOTE:
  flashing from a min_spiffs build erases prefs/contacts -- reconfigure after).
- XIAO ESP32-C6 combined_node support (usb/ble envs), hardware-validated on the
  Wio-SX1262: IDF5 watchdog path, boot-trace aid, pad pin-probe tool.

This fork maintains `combined_node` (companion + repeater + telemetry/bot) as
its own firmware line. Releases are cut by pushing a `combined-node-vX.Y.Z` tag,
which triggers `.github/workflows/build-combined-node-firmwares.yml` (builds all
`*_combined_node_{usb,ble,solar}` envs and drafts a GitHub Release).

## v0.2.0 (2026-07-03)
- `set bot_control_channel <name|idx|off|name,b64psk>`: designate a (private)
  channel authorized for control writes at any hop count -- one channel message
  controls every listening node. Key possession is the auth. (Field-validated:
  `!relay on|off` from the control channel round-trips on air.)
- `!help` bot command lists the read-only commands (control commands unlisted).
- Control writes (`!relay`, `!ble`) require a direct (0-hop) DM or the control
  channel; other channel requests get `DM-only`, relayed DMs get `direct-only`.
- `name,psk` channel binding re-keys an existing same-name channel instead of
  joining a duplicate (a dup breaks channel matching: packets resolve to the
  first hash match).
- Removed the unused `ui-tiny/` display-UI copy (no build env referenced it).
- Bot DM replies are ACK-tracked and resent (bounded, `COMBINED_BOT_REPLY_RETRIES`)
  so `!ping`/`!path` survive a lost packet.
- `!path` resolves each hop to a known name (self/contact) where possible.
- Release-prep: `FIRMWARE_BUILD_DATE` uses `__DATE__` for local builds (CI stamps
  the tag/date); version tagged `-combined`; correctness fixes in
  `combinedAppendVars` (frame-bounds) and `set bot_channel <name>,<b64>` (full
  16/32-byte PSK); bot_channel index range check.
- Added `!path` and `!relay` bot commands.

## Maintenance note
Not upstreamable (MeshCore dev declined the PR), so this is maintained
independently. Periodically merge `upstream/dev` to pull core fixes — the
combined_node copy of companion_radio drifts ~zero as long as it's re-synced.
