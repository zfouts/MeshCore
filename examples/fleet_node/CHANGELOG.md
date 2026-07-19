# fleet_node changelog

## Unreleased
- Initial fleet_node example: a minimal, mesh-only, relay-by-default node whose
  entire command surface is a private control room. Derived from combined_node,
  then stripped to just relay + control-channel admin — no public `!` commands,
  no WiFi/MQTT/observer/TLS, no wardrive. Independent tree (shares no sources).
- Control room: `@<node name> <verb>` on the channel indexed by
  `bot_control_channel` (auth = possession of that private channel's key; only
  the named node acts; rate-limited; DMs never reach it). Verbs: `relay on|off`,
  `ble on|off`, `sensors`, `advert repeater|companion`, `reboot`, `info`,
  `set advert_interval|txpower|location`.
- Advert masquerade (`@name advert repeater|companion`): floods a self-advert
  built with a chosen adv_type (ADV_TYPE_REPEATER / ADV_TYPE_CHAT) using the
  node's real identity, so it presents as a repeater or companion on demand
  (`MyMesh::createAdvertAs`).
- Relay-by-default (`WITH_RELAY_POLICY` + `RELAY_DEFAULT_ON`); `client_repeat`
  = 1 on first boot.
- Build envs (BLE + USB) for heltec_v4, Xiao_S3_WIO, Xiao_C6, Xiao_nrf52,
  WioTrackerL1. nRF52 runs headless (no display).
