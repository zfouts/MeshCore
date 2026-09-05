# observer_node changelog

## Unreleased
- Removed the dead bot-command and `!path` map-link surface. `handleBotChannel()`
  and `buildBotReply()` were declared and called but NEVER DEFINED -- the
  implementation went away when observer_node was decoupled from combined_node,
  and `WITH_BOT_COMMANDS` is defined by no env, so none of it was compiled.
  `observerPathShortUrl()` (a blocking ~5 s HTTPS POST to the mesh-observer
  device API) had zero call sites for the same reason: its only caller was the
  bot's `!path` handler. Gone with it: `set bot.enable`, `set bot.path_channel`,
  `set obs.url`, `set obs.token`, their custom-vars entries, the
  `HTTPClient`/`WiFiClientSecure` includes and the `jsonSanitize()` helper.
  Frees 10,792 bytes of flash and shortens the 176-byte custom-vars frame,
  which had been evicting live status on nodes with a long ssid+host.
  KEPT, because they drive live features and are NOT bot-only:
    * `bot.channel` -- the low-battery "going dark" beacon transmits on it
      (`observerLowBattBeacon`).
    * `bot.control_channel` -- the wardrive beacon asks the fleet for path
      replies on it.
  Their `bot.` prefix is now a misnomer; left alone for the moment rather than
  breaking provisioning again so soon after the dotted rename.
- `bot_enabled`, `bot_path_mask`, `obs_url` and `obs_token` remain in NodePrefs
  and in the DataStore read/write sequence, marked RESERVED. They are dead
  fields but prefs are stored POSITIONALLY: bot_*/obs_* occupy bytes 137-388,
  immediately before mqtt_host at 389, so deleting them would shift every MQTT
  field and corrupt the stored configuration of every deployed node.
- `set mqtt.audience <host>` — Ed25519-signed JWT authentication, so a node can
  publish to the public MeshCore collectors (CoreScope, letsmesh, meshmapper,
  cascadiamesh, ...) which issue no credentials. The node mints the token from
  its own mesh identity in `ObserverJWT.{h,cpp}` (header `{"alg":"EdDSA"}`,
  payload `publicKey`/`aud`/`iat`/`exp`, signed over `header.payload` via
  `LocalIdentity::sign()` — the keypair is never exported) and connects as
  `v1_<UPPERCASE_PUBKEY>`. Wire-compatible with agessaman/MeshCore's JWTHelper.
  24 h lifetime, re-minted on every (re)connect, which is the renewal path.
  Takes precedence over `mqtt.user`/`mqtt.pwd`; `-` clears it. Minting is
  refused if the clock reads pre-2020, since an un-SNTP'd node would emit a
  token every collector rejects — the ladder simply retries once time is set.
  NOTE this authenticates identity but authorises nothing: the token is
  self-issued, so accepting it means open enrollment. It stops a publisher
  impersonating another node and gives a stable bannable identity; it is
  strictly weaker than a server-side allowlist, so brokers you control should
  stay on username/password.
- Optional build-time defaults `OBS_DEFAULT_MQTT_HOST`, `OBS_DEFAULT_MQTT_IATA`
  and `OBS_DEFAULT_MQTT_AUDIENCE` seed a fresh node's collector settings.
  Deliberately NOT set in the committed platformio.ini: a public build of this
  firmware must never dial somebody's private broker. Supply them from a
  private build or a provisioning script.
- Settings are now namespaced with a dot -- `mqtt.host`, `mqtt.user`, `mqtt.pwd`,
  `mqtt.topic`, `mqtt.iata`, `mqtt.packets`, `mqtt.tls_insecure`, `wifi.ssid`,
  `wifi.pwd`, `bot.enable`, `bot.channel`, `bot.control_channel`,
  `bot.path_channel`, `obs.url`, `obs.token`, `advert.dump`, `advert.interval`
  -- matching how the wider MeshCore observer ecosystem namespaces its config.
  The old underscore spellings are STILL ACCEPTED on `set`: names are
  canonicalised (`.` -> `_`) at the entry point, so existing provisioning
  scripts keep working. The dotted form is what the node emits in the
  custom-vars reply, so `get` and `get custom` report dotted names.
  Prefs are stored positionally in the prefs file, so nothing is migrated and
  no stored configuration is affected.
- Default `autoadd_config` to `AUTO_ADD_OVERWRITE_OLDEST`. It was
  zero-initialised, so a full contact table silently stopped learning and the
  retained `contact/` roster froze at whatever it held. Observers are
  unattended and hear far more nodes than MAX_CONTACTS, so the table now rings,
  evicting the oldest non-favourite. `manual_add_contacts` stays 0 (auto-add
  every advert type). Applies to fresh prefs only; both stay runtime-settable.
- **Fix: TLS/wss sessions dropped every ~36 s. The mesh was starving mbedTLS of
  internal DRAM.** `the_mesh` carries `contacts[MAX_CONTACTS+MAX_ANON_CONTACTS]`
  inline -- 119,520 bytes of `.bss` at MAX_CONTACTS=350, the largest object on
  the node. The Arduino esp32 libs build mbedTLS with
  `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` and
  `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` (in AND out, asymmetric content
  length off), so TLS needs ~32 KB of buffers that can come from internal DRAM
  and NOWHERE else -- it can never use the board's 2 MB of PSRAM. Measured on
  the node: free internal heap collapsed to **888 bytes, largest block 532**,
  while ~2 MB of PSRAM sat idle; the write path then timed out
  (`Writing didn't complete in specified timeout: errno=119`,
  `Error to resend data`) and the client closed the session, and re-handshakes
  failed with `tls0x8017` (SSL setup alloc failure) / `-0x2700`. `the_mesh` is
  now constructed in PSRAM. After: **195,836 free / 180,212 largest**, and a
  wss session held >10 min across two roster sweeps where it previously died
  every ~36 s.
  - The allocation is deliberately made on FIRST USE, not at static-init time:
    PSRAM is not yet registered with the heap allocator while C++ static
    constructors run, so a static-init `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
    silently returns NULL and falls back to internal DRAM -- which looks like
    the fix is applied while changing nothing. Verify with the `mesh=` pointer
    in the heap log: `0x3d……`/`0x3c……` is PSRAM, `0x3f……` is internal.
  - `the_mesh` is therefore now `#define the_mesh theMeshInstance()`
    (MyMesh.h) rather than a global object; call sites are unchanged.
- Heap diagnostic in `mqttLoop()`: every 10 s, logs
  `MQTT: conn=… int_free=… int_largest=… psram_free=… mesh=…` to serial. Only
  the INTERNAL figures matter to a handshake, and it needs a large CONTIGUOUS
  block, so the largest-free-block is logged next to the total. This is what
  identified the fault above after two wrong theories; the `mesh=` pointer is
  the one-glance check that the PSRAM allocation actually took.
- Contact-roster republish is now paced: `mqttPublishContacts()` walked the
  whole table and enqueued every contact back-to-back (~100 KB of outbox copies
  in one pass at 324 contacts). It now publishes `OBS_MQTT_CONTACTS_BATCH` (8)
  entries per pass every `OBS_MQTT_CONTACTS_SLICE_MS` (250 ms) from a resumable
  cursor, completing a full sweep in ~10 s and waiting out the rest of
  `OBS_MQTT_CONTACTS_INTERVAL_S` only once the cursor wraps. Same retained
  roster, same refresh cadence, ~2.6 KB peak instead of ~100 KB.
  NOTE: this was first committed as the fix for the session drops -- it was
  not. Sessions died at ~36 s, far short of the 300 s interval, and pacing
  alone changed nothing; the DRAM fault above was the cause. It is kept
  because bursting 324 enqueues at a heap this size is wrong regardless.
  `mqttPublishHeard()` was already capped at 16 entries.
- Custom-vars reply: live `wifi:`/`mqtt:` status now packs FIRST (before the
  stored config and bot vars). The reply frame is best-effort (176-byte cap)
  and on a node with a long ssid+ip+host the `mqtt:` status var was evicted —
  hiding the connect error codes exactly when they're needed. Status buffer
  widened 24→48 so `connecting t<type> tls0x<err> sock<errno> rc<code>` is
  never truncated.
- Periodic zero-hop self-advert default lowered from every 15 min to every 6 h
  (`OBS_ADVERT_INTERVAL_S` 900 -> 21600). The 15-min cadence was tuned for the
  mobile/wardrive case; for stationary observers it was needless chatter.
- `set advert_interval <s>` is now wired up in observer builds (previously the
  build-time default was the only knob — the runtime parser lived in
  fleet_node's bot-command surface, which observers compile out). Set over the
  companion `set` surface (USB/BLE/TCP, e.g. meshcli), persisted across
  reboots. 0 = off, `-` = restore the build default, max 86400 (24 h);
  echoed in the custom-vars dump as `advert_interval:<s>` (effective value).
- MQTT over WebSockets: `mqtt_host` accepts `wss://` (TLS, default 443) and
  `ws://` (plain, default 80) alongside `mqtts://`/`mqtt://`. `wss://` is now
  the recommended transport — encrypted end-to-end through a standard HTTPS
  ingress (TLS terminates at the proxy, ordinary LE cert, no broker certfile),
  single 443 endpoint shared with browser clients. Same pinned-LE-roots
  verification and `mqtt_tls_insecure` opt-out as `mqtts://`. No URI path
  support (handshake requests `/`). Plain `mqtt://` is deprecated
  (unencrypted; lab/bench only). Note: wss uses the same esp-tls/mbedtls
  stack as mqtts — the TLS heap footprint and re-handshake fragility are
  unchanged; the reconnect watchdog remains the mitigation.
- Advert dump: `set advert_dump on` (persisted, default off) publishes each
  heard advert to `<prefix>/advert` — pubkey, 8-byte packet hash, the advert's
  own timestamp (`adv_ts`), our NTP receive time (`rx_ts`), computed clock
  skew (`skew_s`), type/name/snr/hops, and the full packet as hex (`raw`).
  Built for auditing node clocks (which nodes advertise a bad timestamp) and
  byte-level advert decode; verified byte-faithful (adv_ts == wire bytes).
  Tap is `onAdvertRecv` (adverts only, non-blocking enqueue).
- Fix: `mqtt_tls_insecure` was in NodePrefs but never in the DataStore
  load/save, so it didn't persist; added to both (with `advert_dump`).
- Per-user topic namespace (multi-user safety): default prefix is now
  `meshcore/<mqtt_user>/<node_name>` (was `meshcore/<node_name>`), so the
  username segment lines up with a broker `meshcore/%u/#` ACL and each user is
  isolated to their own subtree. Anonymous (no `mqtt_user`) falls back to
  `meshcore/<node_name>`. Fleet send topic is likewise per-user:
  `meshcore/<user>/all/send/+` (`MQTT_SHARED_SEND_PREFIX` -D replaced by
  `MQTT_SHARED_SEND_ENABLE`). Topic buffers enlarged for the longer prefix.
  NOTE: existing nodes move topics on upgrade — clear old-scheme retained
  topics on the broker after reflashing.
- MQTT reconnect watchdog: esp_mqtt auto-retries, but a dropped TLS session
  frequently can't re-handshake (heap fragmentation -> mbedtls setup/handshake
  fails, `tls0x8017`/`0x801a`) and wedges until a reboot. New: if MQTT stays
  down while WiFi is up, escalate -- full client re-init at OBS_MQTT_REBOOT_S/2,
  `ESP.restart()` at OBS_MQTT_REBOOT_S (default 300s). Arms only after a first
  successful connect so an unreachable broker can't cold-reboot-loop; a WiFi
  outage resets the timer. Keeps the MQTT->mesh send bridge from silently
  going deaf after a connection drop.
- MQTT TLS insecure opt-out: `set mqtt_tls_insecure on|off` (NodePrefs, default
  0). When on, attaches no CA so esp-tls skips server-cert verification — for
  private-CA / IP-addressed brokers. Encryption without authentication; per
  node, echoed in the var dump only while on. Default still pins LE roots.
- MQTT TLS: `set mqtt_host mqtts://host[:port]` (default 8883; explicit port
  wins). Broker cert verified against the Let's Encrypt production roots
  (X1/X2/YR/YE) pinned in `MqttCaCerts.h` — no per-device certs, no
  insecure-skip, non-LE CAs rejected. Plain `mqtt://` unchanged.
- SNTP on WiFi connect (`pool.ntp.org`): a power-on reset seeds the RTC to a
  2024 constant, which fails TLS cert validation until a companion syncs the
  clock — unattended TLS nodes now get real time on their own.
- `get mqtt` now surfaces the last connect failure (error type, esp-tls err,
  sock errno, CONNACK code) instead of a permanent bare "connecting" —
  distinguishes DNS vs TCP-timeout vs TLS vs auth at a glance.
- Contact roster walk skips empty contact-table slots (was publishing a bogus
  retained `contact/00000000`).
- Fully decoupled naming from combined_node: `CombinedNode.*` →
  `ObserverNode.*`, `CombinedRepeatFreq.h` → `ObserverRepeatFreq.h`,
  `Combined*` types → `Observer*`, `combinedXxx()` fns → `observerXxx()`,
  `COMBINED_*` build flags → `OBS_*` (e.g. `OBS_MQTT_INTERVAL_S`,
  `OBS_ADVERT_INTERVAL_S`), `WITH_COMBINED_EXTRAS` → `WITH_OBSERVER_EXTRAS`.
  Both shipped envs updated; override any old `-D COMBINED_*` flags with the
  new names. The observer tree now stands alone.
- Removed Home Assistant MQTT discovery (retained `homeassistant/sensor/*/config`
  publishes + the `mc-<mac>` uid). The observer publishes plain topics only;
  HA users can map `<prefix>/telemetry` manually if wanted.
- Added `MQTT.md`: standalone spec of the MQTT interface (topics, payload
  schemas, QoS/retain rules, timing, send-bridge contract, adoption checklist).
- MQTT → mesh send bridge: node now SUBSCRIBES to `<prefix>/send/+`; a payload
  published to `<prefix>/send/<channel_idx>` is transmitted as channel text
  (via `sendGroupMessage`, as this node's name) into that channel slot. Scope
  is deliberately narrow — channels the node holds keys for, text only, no
  DM/CLI path. esp_mqtt-task → main-loop handoff via FreeRTOS queue (depth 4);
  rate-limited `MQTT_SEND_MAX_PER_MIN`=6 (over-budget msgs stay queued;
  queue overflow drops). First deliberate exception to publish-only MQTT.
- Send bridge addressing: `<prefix>/send/<channel>` accepts a slot index OR a
  channel name (case-insensitive; leading `#` on the stored name ignored, as
  MQTT publish topics can't contain `#`). Name resolution happens at drain
  time on the main task (channel state is mesh state), so the queue carries
  the raw topic suffix. Unknown name/slot -> nothing sent.
- Fleet-wide send topic: also subscribes `MQTT_SHARED_SEND_PREFIX/send/+`
  (default `meshcore/all`; `-D` override, `""` disables) so one publish
  reaches every observer. Documented for DISJOINT meshes only — same-mesh
  fleets would double-transmit (dedup can't collapse distinct timestamps).
- Probe DISABLED in both shipped envs (`WITH_OBSERVER_PROBE` removed from
  `heltec_v4_observer_node_wifi` / `Xiao_S3_WIO_observer_node_wifi`): the
  twice-daily flood login + `GET_TELEMETRY_DATA` per repeater spent shared
  airtime on battery/temp data we no longer want. Observer is now passive —
  no flood logins, no telemetry requests; `/repeater/<pk>/telemetry` and
  `/repeater/<pk>/path` are no longer published. Code stays in-tree; re-add
  the define to restore it.
- `<prefix>/heard/<pk>` now carries `"snr"` — the advert's SNR as received by
  this node (per-packet, from `Packet::getSNR()`; stashed by an `onAdvertRecv`
  override since `onDiscoveredContact` isn't handed the packet). For `hops_n`=0
  this measures the link to the origin node; for multi-hop, the last relay's
  transmission. RSSI is deliberately omitted: packets are processed from a
  delayed inbound queue, so the radio's last-RSSI may belong to another packet.
- Initial observer_node variant: companion stack + `_wifi` MQTT publisher
  with bot/relay removed and an active telemetry prober added. Self-contained
  tree — shares no example sources with anything else.
- Active prober (`ObserverProbe.h` schedule/policy + `ObserverProbeGlue.cpp`
  mesh glue): walks known `ADV_TYPE_REPEATER` contacts and, twice a day per
  repeater (`OBS_PROBE_PERIOD_S`=12 h), does a blank-password `sendLogin(c,"")`
  then `sendRequest(REQ_TYPE_GET_TELEMETRY_DATA)`. Parses the CayenneLPP reply
  (battery voltage + MCU temperature). Single probe in flight; `OBS_PROBE_MIN_GAP_S`
  =45 s throttle; `OBS_PROBE_FAIL_BACKOFF_S`=24 h back-off on a rejected/silent
  node (blank password only — never guesses, never requests admin). Gated on a
  valid RTC epoch so the twice-daily schedule is meaningful.
- Publishes retained `<prefix>/repeater/<pk>/telemetry` (`batt_mv`,`temp_c`,`ts`)
  on receipt and `<prefix>/repeater/<pk>/path` (`hops` hex) from the hop chain
  the flooded login teaches (`onContactPathUpdated`) — one sweep refreshes both
  health and topology.
- Does not relay (auto-relay was gated on `WITH_BOT_COMMANDS`, absent here).
