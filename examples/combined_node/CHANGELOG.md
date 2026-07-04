# combined_node changelog

This fork maintains `combined_node` (companion + repeater + telemetry/bot) as
its own firmware line. Releases are cut by pushing a `combined-node-vX.Y.Z` tag,
which triggers `.github/workflows/build-combined-node-firmwares.yml` (builds all
`*_combined_node_{usb,ble,solar}` envs and drafts a GitHub Release).

## Unreleased
- `set bot_control_channel <name|idx|off|name,b64psk>`: designate a (private)
  channel authorized for control writes at any hop count -- one channel message
  controls every listening node. Key possession is the auth.
- `!help` bot command lists the read-only commands (control commands unlisted).
- Control writes (`!relay`, `!ble`) require a direct (0-hop) DM or the control
  channel; other channel requests get `DM-only`, relayed DMs get `direct-only`.
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
