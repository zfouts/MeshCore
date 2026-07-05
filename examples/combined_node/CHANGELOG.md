# combined_node changelog

## Unreleased
- XIAO ESP32-C6 combined_node support (usb/ble/wifi envs), hardware-validated
  on the Wio-SX1262: IDF5 watchdog path, default-on 2.4GHz RF-switch fix
  (upstream left the antenna switch unpowered), boot-trace aid, pad pin-probe
  and RF scan tools. The wifi env serves the companion protocol over TCP
  (`meshcli -t <ip>`), like the upstream S3 wifi companion.

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
