# MeshCore Observer → MQTT: Interface Specification

This document specifies the complete MQTT contract implemented by
`observer_node`, precisely enough that other firmware or bridge
implementations can adopt it as a standard and interoperate with the same
dashboards, maps, and downstream consumers.

Reference implementation: `main.cpp` (client lifecycle + publishers) and
`MyMesh.cpp` (`sendMqttChannelText`, payload sources) in this directory.

---

## 1. Design principles

An adopting implementation should preserve these invariants — they are what
make the contract safe and predictable, independent of firmware internals:

1. **Publish-only, with one narrow inbound path.** The node mirrors what it
   hears onto the broker. The *only* inbound path is the send bridge
   (§6): broker write access lets you *speak in channels the node already
   holds keys for*, as the node — never DM, never CLI/admin, never
   configuration. Compromising the broker must not compromise the node.
2. **Retained = current state, non-retained = event stream.** Anything a
   late-joining subscriber needs (roster, topology, online status) is
   published retained so the broker always holds the latest value. Anything
   that is a moment-in-time sample (own telemetry, mirrored messages) is not
   retained.
3. **Never block the radio.** All publishes triggered from packet-receive
   context are *enqueued* to the MQTT client's own task/thread (non-blocking,
   `esp_mqtt_client_enqueue`); the MQTT connection runs and reconnects in its
   own task. A broker outage must not stall mesh processing.
4. **Own outbound socket.** The MQTT connection is a separate outbound TCP
   client — it must not consume the (single-client) companion TCP console.
5. **Fail soft, fail silent where airtime is at stake.** A send-bridge message
   naming a channel the node doesn't hold is dropped without a reply
   (§6.4) — the alternative is transmitting errors onto shared airtime.
6. **Clean up retained state you stop producing.** If a publisher is removed
   (e.g. the optional prober), its retained topics linger on the broker until
   explicitly cleared with a retained null publish (`mosquitto_pub -r -n`).
   Removing a feature includes clearing its retained topics.

---

## 2. Connection and session

| Parameter    | Value |
|---|---|
| Transport    | **`wss://` (recommended)** MQTT over TLS WebSockets; also `mqtts://` (native TLS), `ws://` (plain WebSockets) and `mqtt://` (plain TCP, **deprecated** — unencrypted, kept for lab/bench use only) |
| Port         | From `mqtt_host` (`[scheme://]host[:port]`); explicit port always wins, defaults **443** wss / **8883** mqtts / **80** ws / **1883** mqtt. No URI path — the WebSocket handshake always requests `/` |
| TLS trust    | Broker cert verified against the Let's Encrypt production roots **pinned in firmware** (`MqttCaCerts.h`); other CAs rejected, no insecure-skip |
| Auth         | Optional username/password (`mqtt_user` / `mqtt_pwd`; empty = anonymous) |
| Keepalive    | **30 s** |
| Clean session| Yes (esp_mqtt default) |
| Reconnect    | Automatic, owned by the MQTT client task |

Examples: `set mqtt_host wss://broker.example.org` (TLS WebSockets on 443,
the recommended transport — rides a standard HTTPS ingress / Cloudflare, TLS
terminates at the proxy with an ordinary Let's Encrypt cert) or
`set mqtt_host mqtts://broker.example.org:31883` (native TLS straight to the
broker). The CA pin is deliberate — the device trusts LE-issued broker certs
and nothing else; if the broker moves to another CA, refresh `MqttCaCerts.h`
and reflash.

Note wss/mqtts differ only in framing, not in on-device cost: both run the
same esp-tls/mbedtls stack, so the TLS heap footprint and the fragmented-heap
re-handshake fragility apply equally. The reconnect watchdog is the
mitigation either way.

**Insecure opt-out:** `set mqtt_tls_insecure on` disables server-cert
verification entirely (encryption without authentication) — for brokers
behind a private CA or reached by an IP/hostname the pinned roots can't
vouch for. Off by default; per-node; echoed in the var dump only when on.
It trades away MITM protection, so use it only where the transport is
already trusted. Toggling it live rebuilds the MQTT client; if the TLS
buffers can't be reallocated in a fragmented heap the reconnect fails
(status shows `tls0x8017` = mbedtls ssl-setup) — reboot and it comes up
clean on the next boot.

**Last Will and Testament (LWT):** topic `<prefix>/status`, payload
`offline`, **retained**. On every successful connect the node publishes
`online` to the same topic, QoS 1, retained. Subscribers therefore always see
the node's liveness with no polling, including after ungraceful death.

**On-connect sequence** (in order):

1. Publish `online` → `<prefix>/status` (QoS 1, retained).
2. Subscribe `<prefix>/send/+` (QoS 0) — per-node send bridge.
3. Subscribe `<shared>/send/+` (QoS 0) — fleet send bridge (§6.3), unless
   disabled or identical to the node prefix.
4. Publish first telemetry immediately (then on the periodic timer).

**Runtime configuration** (companion CLI `set` vars, persisted in prefs):
`mqtt_host` (empty = MQTT off), `mqtt_user`, `mqtt_pwd`, `mqtt_topic`.
Changing any of them tears down and rebuilds the client.

---

## 3. Topic namespace

```
<prefix>/status                     online|offline        retained  QoS1/LWT
<prefix>/telemetry                  node health JSON      not retained  QoS0
<prefix>/sensors                    attached sensors JSON not retained  QoS0
<prefix>/contact/<pk8>              roster entry          retained  QoS0
<prefix>/heard/<pk8>                advert ingress path   retained  QoS0
<prefix>/msg/dm                     mirrored DM           not retained  QoS0
<prefix>/msg/channel                mirrored channel msg  not retained  QoS0
<prefix>/advert                     advert dump (opt-in)  not retained  QoS0
<prefix>/send/<idx|name>            SUBSCRIBED (inbound)  —
<shared>/send/<idx|name>            SUBSCRIBED (inbound)  —
<prefix>/repeater/<pk8>/telemetry   probe builds only     retained  QoS0
<prefix>/repeater/<pk8>/path        probe builds only     retained  QoS0
```

### 3.1 `<prefix>`

`mqtt_topic` if set (used verbatim), else **`meshcore/<username>/<node_name>`**
— a per-user namespace for multi-user safety. `<username>` is the MQTT login
(`mqtt_user`), so it lines up with a broker ACL of `topic readwrite
meshcore/%u/#`: each user can only touch their own subtree. If no username is
set (anonymous), it falls back to `meshcore/<node_name>`. Topic hygiene is
applied to the free-form `<node_name>` segment (the characters `#`, `+`, `/`,
and space become `-`); trailing `/` stripped. All examples below assume login
`mesh` and node `Observer-1`, i.e. `meshcore/mesh/Observer-1`.

### 3.2 `<pk8>`

Node identity in topics is the **first 4 bytes of the Ed25519 public key as 8
lowercase hex chars** (e.g. `6ef177d4`). This matches MeshCore's on-air
1-byte hop hashes (first pubkey byte) while being long enough to be
collision-free in practice; consumers that need the full key can read it from
the payload's `pubkey` field or correlate via advert data.

---

## 4. Payload conventions

- All payloads are **single-object JSON**, UTF-8, no envelope, no schema
  version field — additive evolution only: consumers MUST ignore unknown
  keys, producers MUST NOT change the meaning or type of existing keys.
- **Optional keys are omitted, not null** (e.g. no `hops` when heard direct,
  no `lat`/`lon` when a node adverts no position, no `temp_c` when a repeater
  reports none).
- Strings originating from the mesh (names, message text) are JSON-escaped:
  `"` `\` `\n` `\r` `\t` escaped, other control characters dropped.
- Timestamps (`ts`, `heard`) are **Unix epoch seconds** from the node's RTC
  (mesh-synced; treat with the usual embedded-RTC skepticism).
- `snr` is dB with 0.25 dB resolution (transported on-air as SNR×4).
- Hop chains (`hops`) are the raw MeshCore path bytes hex-encoded: a sequence
  of 1-byte node hashes (first pubkey byte per hop), in travel order from
  origin toward this node. `hops_n` is the hop count; `0` = heard direct.
  Note the on-wire `path_len` is *encoded* (count in the low 6 bits,
  hash-size−1 in the top 2 bits) — publish decoded values only.

---

## 5. Outbound topics (node → broker)

### 5.1 `<prefix>/telemetry` — node health

Every `OBS_MQTT_INTERVAL_S` (default **60 s**), plus once immediately on
connect. Not retained (it's a stream; `status` covers liveness).

```json
{"batt_mv":4150,"batt_pct":98,"uptime_s":86400,"heap":123456,
 "rx":10233,"relayed":0,"dropped":12,
 "snr":-7.5,"rssi":-112,"boot":"power-on","boots":17}
```

| Key | Meaning |
|---|---|
| `batt_mv`, `batt_pct` | battery voltage / LiPo curve percentage |
| `uptime_s` | seconds since boot |
| `heap` | free heap bytes (`-1` if the platform can't report it) |
| `rx`, `relayed`, `dropped` | packet counters since boot |
| `snr`, `rssi` | last received packet's link quality |
| `boot`, `boots` | last reset reason string, lifetime boot counter |

### 5.2 `<prefix>/sensors` — attached sensors

Same cadence as telemetry. Keys are `<type><channel>` (Cayenne-LPP channel
number) so multiple sensors of one type don't collide. **`{}` is published
when no sensors are attached** — deliberately, so a subscriber can
distinguish "no sensors" from "node offline".

```json
{"voltage1":4.150,"temperature1":24.500,"humidity1":40.125,
 "gps2":{"lat":30.26715,"lon":-97.74306,"alt":149.0}}
```

Types: `temperature` (°C), `humidity` (%RH), `pressure` (hPa), `voltage` (V),
`current` (A), `power` (W), `altitude` (m), `gps` (object with `lat`, `lon`,
`alt`).

### 5.3 `<prefix>/contact/<pk8>` — roster (retained)

Full contact-table walk every `OBS_MQTT_CONTACTS_INTERVAL_S` (default
**300 s**). Retained so the broker always holds the current roster/map across
broker or node restarts; re-published on the timer so names, types, and
adverted positions track change.

```json
{"pubkey":"6ef177d4","name":"ATX-Hilltop","type":"repeater",
 "heard":1784465069,"lat":30.267150,"lon":-97.743060}
```

`type` ∈ `none | chat | repeater | room | sensor | unknown`. `heard` = epoch
of the last advert received from this node. `lat`/`lon` present only when the
node adverts a position (0,0 is treated as "none").

*Caveat for consumers:* entries are only re-published while present in the
contact table — a contact evicted from a full table stops updating but its
last retained value remains. Treat `heard` as the staleness signal, not topic
presence.

### 5.4 `<prefix>/heard/<pk8>` — advert ingress topology (retained)

The receive-side topology source: for each node whose advert this observer
heard, the hop-hash chain the advert flooded over to arrive here, published
every `OBS_MQTT_HEARD_INTERVAL_S` (default **180 s**) for the **16 most
recently heard** nodes. Retained.

```json
{"pubkey":"349daaf7","name":"South-Gate","hops_n":2,
 "ts":1784431091,"snr":-3.2,"hops":"dd17"}
```

`snr` is the advert *as received here*: for `hops_n:0` that is the direct
link to the origin node; for multi-hop it is the last relay's transmission.
`hops` omitted when heard direct.

### 5.5 `<prefix>/msg/dm`, `<prefix>/msg/channel` — message mirror

Event-driven, at receipt, not retained, QoS 0, enqueued (never blocks the
radio path that just delivered the message).

```json
{"from":"alice","text":"on my way","snr":-7.5,"hops_n":1,"hops":"dd",
 "sender_ts":1784504030,"rx_ts":1784504032,"skew_s":2}
```
```json
{"channel":"#bot","text":"KJ5DHR: !path","snr":11.8,"hops_n":4,"hops":"51fb",
 "sender_ts":1784504030,"rx_ts":1784504032,"skew_s":2}
```

DMs carry `from` (contact name); channel messages carry `channel` (stored
channel name, `?` if the slot has none) and the sender is embedded in `text`
as `<sender>: <message>` (that is the on-air group-message format). `hops` /
`hops_n` are the same ingress-path data as §5.4 but sampled at message time —
a second topology feed.

`sender_ts` is the message's own embedded timestamp (the sender's clock, unix
epoch s); `rx_ts` is this observer's NTP receive time; `skew_s` = `rx_ts -
sender_ts` — the same clock-audit fields as `<prefix>/advert` (§5.7), so a
consumer can flag senders with a bad clock from ordinary chat traffic too.

**Operator warning:** this mirrors *every readable message* — including
private channels the node holds keys for. Decrypted mesh traffic lands on the
broker; that is the operator's explicit choice when pointing the node at a
broker, and adopters must document it as such.

### 5.6 `<prefix>/repeater/<pk8>/telemetry`, `.../path` — active prober (optional builds only)

Only in builds with `-D WITH_OBSERVER_PROBE=1` (compiled out of the shipped
envs — the flood logins cost shared airtime). Push-published retained the
moment a probe response arrives:

```json
{"batt_mv":3910,"ts":1784153887,"temp_c":25.2}
```
```json
{"hops":"dd1715b082"}
```

`temp_c` omitted when the repeater reports no temperature. If you run probe
builds and later disable probing, clear these retained topics (§1 rule 6).

### 5.7 `<prefix>/advert` — advert dump (opt-in)

Off by default; enable per node with `set advert_dump on`. Publishes one
message per **advert** the node hears (event stream, not retained), for
diagnosing node clocks and inspecting adverts byte-for-byte.

```json
{"pubkey":"bbc66b12","hash":"a1b2c3d4e5f60718","adv_ts":1784504030,
 "rx_ts":1784504032,"skew_s":2,"type":"repeater","name":"Valhalla 3000",
 "snr":7.0,"hops_n":2,"raw":"1102…"}
```

| Key | Meaning |
|---|---|
| `pubkey` | `<pk8>` — first 4 pubkey bytes, lowercase hex |
| `hash` | 8-byte packet hash (payload+type, **path-independent**), 16 hex — same across relay copies of one advert; differs for a re-stamped advert. Use it to dedupe. |
| `adv_ts` | the advertising node's **own clock** (unix epoch s), decoded from the advert |
| `rx_ts` | this observer's NTP-disciplined **receive** time (unix epoch s) |
| `skew_s` | `rx_ts - adv_ts` — the advertiser's clock error. ~0 = fine; large positive = node in the past; negative = ahead. (A node stuck on the ESP32 power-on seed reads ~+40,000,000.) |
| `type` | `none \| chat \| repeater \| room \| sensor \| unknown` |
| `name` | advertised node name (JSON-escaped) |
| `snr` | advert SNR as heard here (dB) |
| `hops_n` | hop count to reach us (`0` = direct) |
| `raw` | the **full on-wire packet as hex** (header + path + payload) |

**Decoding `raw`** (bytes, offsets into the hex-decoded blob):

```
off 0   header       1B   bits[1:0]=route (1=FLOOD), bits[5:2]=payload type (4=ADVERT), bits[7:6]=ver
off 1   path_len     1B   hash_size = (b>>6)+1;  hash_count = b & 0x3F
off 2   path         hash_count * hash_size bytes   (each hop = leading byte(s) of a relay pubkey)
        --- payload (advert), starts at off = 2 + path_bytes ---
+0      public key   32B
+32     timestamp    4B   LE uint32, unix epoch s  == adv_ts
+36     signature    64B  Ed25519 over pubkey+timestamp+app_data
+100    app flags    1B   0x0F=type; 0x10=has lat/lon; 0x80=has name
+101    latitude     4B   LE int32, degrees × 1e6   (present iff flag 0x10)
+105    longitude    4B   LE int32, degrees × 1e6   (present iff flag 0x10)
+109    node name    rest (present iff flag 0x80)
```

Note the timestamp's absolute offset shifts with `hop_count` (more hops → more
path bytes before the payload). Adverts are **signed, not encrypted**, so the
whole payload decodes with no keys — unlike channel/DM message payloads.

For a fleet clock audit, subscribe to `<prefix>/advert`, group by `pubkey`,
and watch `skew_s`: a few nodes with a large constant skew are the ones with a
broken/unsynced clock. `hash` lets you tell a genuinely re-stamped advert from
the same advert re-heard via a different path.

---

## 6. Inbound: the MQTT → mesh send bridge

The single inbound path. Publishing a plain-text payload to:

```
<prefix>/send/<channel>        this node transmits it
<shared>/send/<channel>        every subscribed observer transmits it
```

causes the node to send the payload as a **channel text message, as this
node's name**, into a channel it already holds keys for.

### 6.1 Channel addressing

`<channel>` is either:

- a **decimal slot index** (`send/0`, 0–255, max 3 digits), or
- a **channel name**, matched case-insensitively against the node's stored
  channel names, with a leading `#` on the *stored* name ignored (MQTT
  topics cannot contain `#`) — so `send/bot` reaches the channel named
  `#bot`.

**Prefer names in a fleet.** Slot order is per-node configuration history; a
name travels with the channel — and a name miss sends *nothing*, instead of
posting into whatever occupies that slot number on some node.

### 6.2 Limits and semantics

| Property | Value |
|---|---|
| Rate limit | `MQTT_SEND_MAX_PER_MIN` (default **6/min**) per node |
| Queue | depth **4**; over-budget messages *wait in queue*; overflow beyond the queue is **dropped** |
| Text length | `MAX_TEXT_LEN` (**160** chars); longer payloads truncated |
| Fragmented MQTT messages | rejected (they're necessarily oversize) |
| Empty payload / unknown channel / unconfigured slot | dropped silently |

The rate limit exists because the bridge converts broker writes into mesh
airtime: **anyone with broker write access spends your mesh's airtime.**
Secure the broker accordingly (auth + ACLs on `+/send/#` at minimum).

### 6.3 Fleet send topic

Every observer also subscribes to `meshcore/<username>/all/send/+` — a
**per-user** fleet topic, so one publish reaches all of *that user's* nodes
while staying inside their ACL namespace. This is for a user's observers on
**disjoint meshes** (one bridge per mesh). Two subscribers on the *same* mesh
each transmit their own copy — packet dedup cannot collapse them (different
timestamps) and the channel sees doubles. Same-mesh sends must use the
per-node `<prefix>/send/` topic. Disable with `-D MQTT_SHARED_SEND_ENABLE=0`.
Only active when a username is set.

### 6.4 Why failures are silent

A miss (unknown name, missing key, empty slot) transmits nothing and reports
nothing: any error reply would itself be mesh airtime, and the bridge must
not be a way to make the node transmit on demand beyond its rate-limited
send. Check `get_channels` on the node when a bridge send doesn't appear.

### 6.5 Implementation note (threading)

In the reference implementation the MQTT client task only *parses and
queues* inbound messages (channel carried as a string); the main loop drains
the queue and touches the mesh stack. Adopters with a similar
single-threaded-mesh constraint should copy this shape: resolving a channel
name to key material requires mesh state, so it happens at drain time, in
the mesh's own context.

---

## 7. Consuming from a frontend / dashboard

Notes for a read-only web or app client (as opposed to a firmware adopter).

### 7.1 Connecting from a browser

Point a web client (e.g. `mqtt.js`) at the same **MQTT-over-WebSocket**
listener the firmware uses (mosquitto `listener <port>` + `protocol
websockets`, with TLS terminated at the ingress in front of it) — since the
firmware moved to `wss://`, browsers and devices share one endpoint. Native
apps can use a TCP listener where the broker still exposes one. Log in with the user's credentials; with `meshcore/%u/#` ACLs the
client only sees its own subtree, so subscribe to `meshcore/<username>/#`.

### 7.2 Build state from retained topics

`status`, `contact/<pk8>`, and `heard/<pk8>` are **retained**, so on connect
the broker immediately replays the current value of each — you get the full
node roster, per-node liveness, and topology without waiting for the next
publish. Build your node/contact list from these. `telemetry`, `sensors`, and
`msg/*` are **live streams** (not retained): you only see them going forward.

### 7.3 Parsing gotchas

- **`status` is a bare string** (`online`/`offline`), not JSON — special-case
  it; don't `JSON.parse` it.
- Everything else is a single JSON object. **Optional keys are omitted, not
  null** — test for presence (`"lat" in c`), don't assume.
- Timestamps (`ts`, `heard`) are unix epoch **seconds**. `snr` is dB. `hops` is
  a hex string of 1-byte hop hashes; `hops_n` is the count (`0` = heard direct).
- Tolerate unknown keys — the schema only grows, never repurposes a key.

### 7.4 Keying the UI

Topics are `meshcore/<username>/<node_name>/<leaf>`. Key your dashboard by the
`<node_name>` path segment (one user may run several nodes), and within a node
by `<pk8>` for per-contact rows. `contact.type` ∈
`none | chat | repeater | room | sensor | unknown`; `contact.heard` is the
staleness signal (a contact evicted from a full table stops updating but its
last retained value lingers — trust `heard`, not topic presence).

### 7.5 Sending (optional)

To let the UI transmit into a mesh channel, publish plain text to
`meshcore/<username>/<node_name>/send/<channel>` (channel name or slot index;
see §6). Read-only dashboards can ignore this and the per-user fleet topic.

---

## 8. Timing summary

| Publisher | Default cadence | Override (`-D`) |
|---|---|---|
| `telemetry`, `sensors` | 60 s | `OBS_MQTT_INTERVAL_S` |
| `contact/*` roster walk | 300 s | `OBS_MQTT_CONTACTS_INTERVAL_S` |
| `heard/*` topology | 180 s | `OBS_MQTT_HEARD_INTERVAL_S` |
| `msg/*` mirror | at receipt | — |
| `advert` dump (opt-in) | per advert heard | `set advert_dump on\|off` |
| `repeater/*` (probe builds) | at probe response | probe tunables, see `ObserverProbe.h` |
| Send-bridge budget | 6 msg/min, queue 4 | `MQTT_SEND_MAX_PER_MIN` |

---

## 9. Adoption checklist

For a firmware or gateway claiming compatibility with this contract:

- [ ] Retained `<prefix>/status` online/offline with LWT, published on connect
- [ ] Topic prefix defaulting to `meshcore/<sanitized-node-name>`, overridable
- [ ] `<pk8>` = first 4 pubkey bytes, lowercase hex, in topic *and* payload
- [ ] JSON conventions of §4 (omit-not-null, escape mesh strings, epoch
      seconds, decoded hop chains)
- [ ] State topics retained, event topics not, per §3 table
- [ ] Publishes from RX context are non-blocking / queued to the client task
- [ ] Send bridge: channel-held-keys only, name+index addressing with `#`
      rule, rate limit + bounded queue, silent misses, no DM/admin path
- [ ] Fleet send subscription only where meshes are disjoint (or disabled)
- [ ] Retained topics cleared when a producing feature is disabled
