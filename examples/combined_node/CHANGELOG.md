# combined_node changelog

This fork maintains `combined_node` (companion + repeater + telemetry/bot) as
its own firmware line. Releases are cut by pushing a `combined-node-vX.Y.Z` tag,
which triggers `.github/workflows/build-combined-node-firmwares.yml` (builds all
`*_combined_node_{usb,ble,solar}` envs and drafts a GitHub Release).

## Unreleased
- `!help` bot command lists the commands the build supports.
- Direct-only (0-hop) gating on control writes (`!relay`, `!ble`): only a sender
  in direct radio range can toggle node state; relayed requests get refused.
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
