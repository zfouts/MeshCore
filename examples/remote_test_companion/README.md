# remote_test_companion

A companion (chat) node you can drive from somewhere else on the mesh. It joins
a **private control room** (a group channel with its own key) and, when someone
in that room says

    !msg #Public hello from the far side

it posts `hello from the far side` into `#Public` as itself. It also relays
(repeat mode) on the 902-928 MHz US/CA band, so the same box can sit on a hill
as a repeater and double as a remote test transmitter.

## What it is (and is not)

`examples/companion_radio` builds **unchanged** with two additions:

| Piece | Where |
|---|---|
| `-include RemoteTestCompanion.h` | widens the repeat-frequency gate to 902-928 MHz and names the hook function |
| `RemoteTestCompanion.cpp` | the bot: parses commands from the control room, posts and acknowledges |
| `COMPANION_CHANNEL_MSG_HOOK` | a 4-line, `#ifdef`-guarded call at the end of `MyMesh::onChannelMessageRecv` in companion_radio; a no-op in every other build |

Nothing else is forked, so these builds pick up companion_radio fixes for free.
The repeat-gate half is the same trick `examples/repeater_companion` uses.

## Setup

1. Flash a `*_remote_test_companion_{ble,usb}` env.
2. Give the node a name (`set name Solar-7`).
3. Create the control room on the node **and** on the phone or CLI you will
   drive it from: same channel name, same key. The default name is `control`
   (leading `#` and case are ignored). With meshcli:

       add_channel control <hex key>

   Pick a random key and keep it private. Whoever holds it can make the node
   transmit.
4. Optionally turn relaying on (5th radio argument):

       set radio 910.525,62.5,7,5,on

   Omitting the 5th argument keeps the current setting; check with `infos`.

## Commands

Sent as ordinary messages in the control room. Case-insensitive.

| Command | Effect |
|---|---|
| `!msg #<channel> <text>` | post `<text>` into `<channel>` as this node. The channel must already exist on the node (`#Public` always does). |
| `!ping` | reply with the SNR and hop count of your request, useful to confirm the room is reachable before sending. |
| `@<node name> !msg ...` | address one node when several share a room; the others stay silent. |

Every command is acknowledged back into the room, tagged `@<sender>`:

    @Alice sent to #Public as Solar-7: hello from the far side
    @Alice no channel named #austin on Solar-7
    @Alice pong from Solar-7 (snr 8.5, 2 hops)

Plain chatter in the room (anything not starting with `!` or `@<name> !`) is
ignored. Commands are rate-limited to one every 3 s per node
(`REMOTE_TEST_MIN_INTERVAL_MS`); extra ones are dropped silently. The bot will
not post into the control room itself.

## Build-time knobs

Pass with `-D` in the env, or edit `RemoteTestCompanion.h`.

| Macro | Default | Meaning |
|---|---|---|
| `REMOTE_TEST_CONTROL_CHANNEL` | `"control"` | name of the control room (quote it: `-D REMOTE_TEST_CONTROL_CHANNEL='"ops"'`) |
| `REMOTE_TEST_REPEAT_LO_KHZ` / `_HI_KHZ` | `902000` / `928000` | band on which repeat mode may be enabled |
| `REMOTE_TEST_MIN_INTERVAL_MS` | `3000` | minimum spacing between accepted commands |

## Regulatory note

The 902-928 MHz default is the US/CA ISM band. The firmware does not know where
it is; narrow or move the range if you deploy elsewhere.
