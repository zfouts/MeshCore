# combined_node changelog

## Unreleased
- `bot_path_channel` (uint64 `bot_path_mask` at offset 381, same `+`/`-`
  syntax): channels where ONLY `!path` answers -- everything else silent,
  and 0-hop (direct) requests are ignored too (no route to map). For public
  channels (#bot) that should map routes without exposing the full command
  surface. A channel in both sets gets full commands.
- FIX: `@name ...` targeted admin never worked over the air -- the '!'-only
  prefix check dropped '@' messages before the control-channel handler ran.
- Multi-channel bot: the bot now answers on a SET of channels
  (`NodePrefs.bot_channel_mask`, uint64 at offset 373; legacy single
  `bot_channel` auto-migrates at boot and remains the "primary" — the
  low-battery beacon target). `set bot_channel +#bot` adds a channel,
  `-#bot` removes one, a bare value replaces the set; `get` lists them
  `+`-joined. Replies always go to the channel the request arrived on.
- `@name set obs_url <url>` / `@name set obs_token [label:]token` on the
  control channel: provision the observer config over the mesh. Labelled
  tokens ("garden:a1b2…", the observer's multi-token secret format) are
  accepted verbatim; the label is stripped node-side. The reply never echoes
  the token. FIX: `obs_token` grew 33→65 bytes — a 48-char `openssl rand
  -hex 24` secret was being silently TRUNCATED (every post would 403);
  oversize values are now rejected, never clipped. Nodes provisioned before
  this fix must re-set their token.
- `!path` map links via mesh-observer: on `_wifi` builds with `set obs_url` /
  `set obs_token` configured (persisted, NodePrefs offsets 243/308), the node
  POSTs the hop chain (+ its own and the wardrive requester's position) to
  the observer's `/api/device/path` and appends the returned short map URL
  to the reply. Weak/strong hook (`combinedPathShortUrl`) like the WiFi
  vars; fails soft to the plain hex reply; blocks the radio up to ~5 s;
  HTTPS without cert validation (token is the auth).
- New read-only bot commands: `!batt` (mV/% + min/max since boot + 1h trend,
  sampled once a minute), `!boot` (reset cause + persisted boot counter --
  NodePrefs `boot_count` at offset 239), `!heard <name|hex>` (point query
  into the neighbour table), `!rf` (freq/sf/bw/cr/txpower one-liner),
  `!time` (RTC + drift vs sender), `!wifi` (status on `_wifi` builds).
- New control-channel writes: `!advert` (flood advert on demand),
  `!time sync` (adopt sender's clock), `@name reboot` (deferred 5 s so the
  reply airs first; targeted-only so one message can't bounce the fleet),
  `@name set gps on|off` (persisted), `@name set advert_interval <s>`
  (runtime-only; the periodic advert timer is now runtime-driven).
- `!help` (control-only) now lists everything, write commands included.
- `!path` hop count reads `[4h]` instead of `[#4]`.
- `!help` is control-channel only: on the bot channel the node stays silent,
  so the command list isn't advertised.
- Bot commands are channel-only: DMs are ignored entirely (a `!` DM is just a
  chat message), and write commands (`!relay`/`!ble`/`!wd on|off`) are
  authorized ONLY on the control channel — the direct (0-hop) DM auth path is
  removed. The DM reply ACK-track/resend machinery
  (`COMBINED_BOT_REPLY_RETRIES`) is gone with it; channel replies were always
  fire-and-forget.
- `!path` hops are always the raw hex path-hash (public-key prefix) again --
  the best-effort name resolution added in v0.2.0 is removed (names hid the
  key prefixes, and short hashes could alias to the wrong contact anyway).
- `_wifi` envs for all ESP32 boards (`WITH_RUNTIME_WIFI`; new on Heltec V4 and
  XIAO S3, reworked on XIAO C6): companion over WiFi TCP (port 5000) **and**
  USB (`SerialMux` fans one companion endpoint over both; replies follow the
  last-active link). Credentials are runtime custom vars provisioned over the
  USB cable — `set wifi_ssid <ssid>` / `set wifi_pwd <pwd>` persists and
  applies live, no reboot/recompile; `-` clears (WiFi off); `get` reports
  `wifi_ssid` + `wifi` state (IP when associated), never the password.
  Compile-time `WIFI_SSID`/`WIFI_PWD` demoted to an optional first-boot
  default. `_usb`/`_ble` envs carry no WiFi code (no BLE+WiFi coexistence);
  `set wifi_ssid` errors there and on nRF52. New NodePrefs fields persisted
  at offsets 141/174. CI release builds include the `_wifi` suffix.
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
