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
  SNR and ingress path.
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
- **Fleet-wide send topic** — every observer also subscribes to
  `MQTT_SHARED_SEND_PREFIX/send/+` (default `meshcore/all`), so a single
  publish reaches the whole fleet. **For observers on disjoint meshes only**
  (one bridge per mesh): two subscribers on the *same* mesh each transmit
  their own copy — packet dedup can't collapse them (different timestamps) and
  the channel sees doubles. Same-mesh sends should use the per-node
  `<prefix>/send/` topic. `-D MQTT_SHARED_SEND_PREFIX='""'` disables it.

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
contract, adoption checklist): **[MQTT.md](MQTT.md)**.

```
<prefix>/contact/<pubkey>          roster (retained) — every heard node
<prefix>/heard/<pk>                advert ingress: hops + SNR (retained)
<prefix>/msg/dm, /msg/channel      message mirror (at receipt)
<prefix>/send/<idx|name>           SUBSCRIBED — inbound text → mesh channel
meshcore/all/send/<idx|name>       SUBSCRIBED — fleet-wide send (disjoint meshes)
<prefix>/telemetry, /sensors       this node's own health / attached sensors
<prefix>/status                    online/offline (retained, LWT)
<prefix>/repeater/<pk>/telemetry   probe builds only (retained)
<prefix>/repeater/<pk>/path        probe builds only (retained)
```

`<prefix>` = `mqtt_topic` or `meshcore/<node_name>`.

## Build

`_wifi` only (the output path is MQTT). Configure Wi-Fi + broker at
runtime over USB: `set wifi_ssid` / `set wifi_pwd`,
`set mqtt_host <[mqtt(s)://]host[:port]>` (+ `mqtt_user` / `mqtt_pwd`).
`mqtts://` = TLS, verified against the Let's Encrypt roots pinned in
`MqttCaCerts.h` (see MQTT.md §2).

```
pio run -e heltec_v4_observer_node_wifi
pio run -e Xiao_S3_WIO_observer_node_wifi
```
