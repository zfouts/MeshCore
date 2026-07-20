# fleet_node

A **minimal, mesh-only relay** with one feature: a private **control room** for
managing a fleet of nodes over the mesh itself — no WiFi, no MQTT, no internet,
no public commands.

It relays by default (like a Meshtastic router) and is otherwise silent. The
only way to interact with it is the control room: messages addressed
`@<node name> <verb>` on a private control channel. Possession of that
channel's key is the entire authorization model — a single channel message can
drive the whole fleet, and only the named node acts. Direct messages never
reach it, and there are no `!` public commands at all.

## Control room

Point the node at a private channel and enable it (over USB/BLE with meshcli):

```
set bot_control_channel <idx|#name|name,base64psk>   # which channel is the control room
set bot_enable 1                                     # master enable
```

Then, from any device that holds that channel's key, address a node by name:

| Command | Effect |
|---|---|
| `@name relay on\|off`        | toggle packet forwarding (`client_repeat`), persisted, effective immediately |
| `@name ble on\|off`          | toggle BLE advertising (persisted); reply rides LoRa so it arrives after `off` |
| `@name sensors`              | one-line sensor summary (temp/humidity/pressure/voltage/gps; "no sensors" if none) |
| `@name advert repeater\|companion` | flood a self-advert **masquerading** as that node type (see below) |
| `@name reboot`               | deferred 5 s reboot (the remote unstick for a sealed node) |
| `@name info`                 | `<name> fw <version> relay:on\|off ble:on\|off` |
| `@name set advert_interval <s>` | periodic-advert cadence, runtime-only (reboot restores the build default) |
| `@name set txpower <dbm>`    | radio TX power, persisted |
| `@name set location <lat,lon>\|off` | node position for adverts, persisted |

Replies are tagged `@<sender> <node>: <reply>` back to the control channel, and
every command is rate-limited per node. A node that isn't the named target
stays silent; an unrecognised verb stays silent.

### Advert masquerade

`@name advert repeater|companion` floods a self-advert built with a chosen
`adv_type` — `repeater` = `ADV_TYPE_REPEATER`, `companion` = `ADV_TYPE_CHAT` —
using the node's **real identity and keypair**. It doesn't fabricate a node; it
changes only the type the node announces, so the rest of the mesh routes to it
as a repeater or a companion on demand. Useful for seeding how the network
perceives a node, or presenting a relay as a repeater to clients.

## Relay behaviour

Relay-by-default: first boot sets `client_repeat = 1` (`RELAY_DEFAULT_ON`). The
flood filter (`WITH_RELAY_POLICY`, ported from `simple_repeater`) enforces hop
limits and loop detection; direct/path-routed packets always forward. Toggle
per node at runtime with `@name relay on|off`.

## Build

Mesh-only, so BLE and USB variants only (no `_wifi`). Configure the control
channel and identity at runtime over USB with meshcli.

```
pio run -e heltec_v4_fleet_node_ble        # or _usb
pio run -e Xiao_S3_WIO_fleet_node_ble      # or _usb
pio run -e Xiao_C6_fleet_node_ble          # or _usb  (no display)
pio run -e Xiao_nrf52_fleet_node_ble       # or _usb  (headless)
pio run -e WioTrackerL1_fleet_node_ble     # or _usb
```

## Relationship to combined_node

fleet_node began as a copy of `combined_node` with everything but relay + the
control room stripped out: no public `!` commands, no WiFi/MQTT/observer/TLS, no
wardrive. From here the trees are independent — they share no source files.
