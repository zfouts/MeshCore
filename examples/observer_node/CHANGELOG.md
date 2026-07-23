# observer_node changelog

## Unreleased
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
