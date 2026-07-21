# observer_node

A **passive network monitor** for a MeshCore mesh: a self-contained variant
of the companion stack plus an MQTT publisher.

It is a monitor, not a chat/relay node: bot commands and the relay policy are
not part of this tree, it does not forward packets, and in the default build it
**never solicits anything from the mesh** — everything it publishes is heard,
not asked for. Its only transmissions are its own periodic zero-hop advert
(`OBS_ADVERT_INTERVAL_S`, 15 min default, `set advert_interval 0` to
silence it), protocol ACKs to traffic addressed directly to it, and channel
messages explicitly injected through the MQTT send bridge below.

## What it does

- **Passive roster** — mirrors every heard advert to MQTT
  `<prefix>/contact/<pubkey>` (name / type / coords / last-heard).
- **Advert ingress topology** — `<prefix>/heard/<pk>` carries the hop-hash
  chain each advert flooded over to reach this node, plus the advert's SNR as
  received here (`hops_n`=0 → direct link to the origin).
- **Message mirror** — readable DMs/channel traffic to `<prefix>/msg/*` with
  SNR, ingress path, and clock fields: `sender_ts` (message's embedded
  timestamp), `rx_ts` (our NTP receive time), and `skew_s = rx_ts - sender_ts`,
  so a consumer can flag senders with a bad clock from ordinary chat.
- **Advert dump** (opt-in, `set advert_dump on`) — publishes each heard advert
  to `<prefix>/advert`: the advertiser's own clock (`adv_ts`), our NTP receive
  time (`rx_ts`), the computed `skew_s`, an 8-byte packet hash (groups relay
  copies), and the full packet as hex (`raw`). Built for auditing node clocks
  (which nodes advertise a bad timestamp) and byte-level advert decode.
- **MQTT → mesh send bridge** — publish text to `<prefix>/send/<channel>` and
  the node transmits it as a channel message (as this node's name).
  `<channel>` is a slot index (`send/0`) **or a channel name** (`send/Public`,
  case-insensitive; a leading `#` on the stored name is ignored, so `send/bot`
  reaches `#bot`). Prefer names in a fleet: slot order is per-node config
  history, the name travels with the channel — and a name miss sends *nothing*
  instead of posting to whatever occupies that slot. Deliberately narrow: only
  channels this node already holds keys for, text only (max 160 chars, longer
  truncated), no DM or CLI/admin path — broker access lets you *speak in the
  node's channels*, not command the node. Throttled to `MQTT_SEND_MAX_PER_MIN`
  (default 6) with a 4-deep queue; overflow is dropped. Note this makes the
  node TX on demand, so anyone with broker write access spends your mesh's
  airtime.
- **Per-user fleet send topic** — every observer also subscribes to
  `meshcore/<user>/all/send/+`, so a single publish reaches all of *that
  user's* nodes (inside their ACL namespace). **For disjoint meshes only**
  (one bridge per mesh): two subscribers on the *same* mesh each transmit
  their own copy — packet dedup can't collapse them (different timestamps) and
  the channel sees doubles. Same-mesh sends should use the per-node
  `<prefix>/send/` topic. `-D MQTT_SHARED_SEND_ENABLE=0` disables it.

## Multi-user topic namespace

Topics are namespaced per user for safety:

```
<prefix> = meshcore/<mqtt_user>/<node_name>
```

The username segment is the MQTT login (`mqtt_user`), so it lines up with a
mosquitto `topic readwrite meshcore/%u/#` ACL — each user is isolated to their
own subtree. Anonymous (no `mqtt_user`) falls back to `meshcore/<node_name>`.
`set mqtt_topic <string>` overrides the whole prefix verbatim. See MQTT.md §3.1.

## Robustness

- **NTP** — the node disciplines its clock from `pool.ntp.org` (SNTP) on every
  WiFi connect, so `rx_ts`/`skew_s` and TLS cert dates are trustworthy without
  a manual `clock sync`.
- **Reconnect watchdog** — if MQTT stays down while WiFi is up, the node
  escalates (full client re-init, then `ESP.restart()` at `OBS_MQTT_REBOOT_S`,
  default 300 s) so the bridge self-heals instead of wedging. Arms only after a
  first successful connect.

## Optional active prober (off by default)

The tree still contains an active telemetry prober (`ObserverProbe.h` /
`ObserverProbeGlue.cpp`), **compiled out of the shipped envs** — its flood
logins cost the whole mesh airtime for data (repeater battery/temp) we no
longer collect. Re-enable with `-D WITH_OBSERVER_PROBE=1` if you need it:
it walks known repeaters twice a day each with a blank-password login +
`GET_TELEMETRY_DATA` request, publishing `<prefix>/repeater/<pk>/telemetry`
and the login-taught hop chain to `<prefix>/repeater/<pk>/path`. It never
guesses passwords and never requests admin; a password-protected repeater
rejects the guest login and the probe backs off `OBS_PROBE_FAIL_BACKOFF_S`.
Probe tunables (`OBS_PROBE_PERIOD_S`, `OBS_PROBE_MIN_GAP_S`,
`OBS_PROBE_TIMEOUT_S`, `OBS_PROBE_FAIL_BACKOFF_S`, `OBS_PROBE_MAX_TRACKED`)
are all `-D` overridable — see `ObserverProbe.h`.

## MQTT topics

Full interface spec (payload schemas, QoS/retain rules, timing, send-bridge
contract, byte-level advert decode, frontend guide, adoption checklist):
**[MQTT.md](MQTT.md)**.

```
<prefix> = meshcore/<mqtt_user>/<node_name>

<prefix>/status                    online/offline (retained, LWT)
<prefix>/telemetry, /sensors       this node's own health / attached sensors
<prefix>/contact/<pubkey>          roster (retained) — every heard node
<prefix>/heard/<pk>                advert ingress: hops + SNR (retained)
<prefix>/msg/dm, /msg/channel      message mirror + clock skew (at receipt)
<prefix>/advert                    advert dump (opt-in): clock skew + raw hex
<prefix>/send/<idx|name>           SUBSCRIBED — inbound text → mesh channel
meshcore/<user>/all/send/<idx|name>  SUBSCRIBED — per-user fleet send
<prefix>/repeater/<pk>/telemetry   probe builds only (retained)
<prefix>/repeater/<pk>/path        probe builds only (retained)
```

## Build

`_wifi` only (the output path is MQTT). Supported boards (ESP32, all with
the same feature set):

```
pio run -e heltec_v4_observer_node_wifi
pio run -e Heltec_v3_observer_node_wifi
pio run -e Xiao_S3_WIO_observer_node_wifi
pio run -e Xiao_C6_observer_node_wifi
```

Configure Wi-Fi, broker, and credentials at runtime over USB:

```
set wifi_ssid <ssid>
set wifi_pwd  <pwd>
set mqtt_host <[scheme://]host[:port]>    # wss:// TLS WebSockets (443, recommended);
                                          # also mqtts:// (8883), ws:// (80),
                                          # mqtt:// plain TCP (1883, deprecated)
set mqtt_user <user>                       # becomes the topic namespace segment
set mqtt_pwd  <pwd>
set advert_dump on                         # optional: clock-audit advert stream
```

`wss://` (MQTT over TLS WebSockets) is the recommended transport: it rides a
standard HTTPS ingress on 443, so the broker needs no cert of its own — TLS
terminates at the proxy with an ordinary Let's Encrypt cert. Both `wss://`
and `mqtts://` verify the server against the LE roots pinned in
`MqttCaCerts.h`; `set mqtt_tls_insecure on` skips verification (encryption
without authentication — trusted networks only). Plain `mqtt://` is
deprecated — unencrypted, lab/bench use only. See MQTT.md §2.

### Releases (CI)

Tag `observer-v1.16.<n>` and push — the `build-observer-firmwares` GitHub
Action builds all four boards and publishes a GitHub Release with
`<board>-v1.16.<n>-observer-<sha>.bin` (+ `-merged.bin` for a fresh flash).
Ad-hoc build (artifacts only, no release):
`gh workflow run build-observer-firmwares.yml --ref main`.
