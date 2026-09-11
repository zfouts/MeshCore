// remote_test_companion -- the control-room bot behind COMPANION_CHANNEL_MSG_HOOK.
//
// A stock companion_radio node that also listens on ONE private channel (the
// control room, named by REMOTE_TEST_CONTROL_CHANNEL) for commands. Possession of
// that channel's key is the whole authorisation model: anyone in the room can
// drive the bot, nobody outside it can. Nothing is accepted from any other
// channel or from direct messages.
//
// Commands (case-insensitive, optionally prefixed "@<node name> " to address one
// bot when several share the room):
//
//   !msg #<channel> <text>   post <text> into <channel> as this node
//   !ping                    reply with SNR / hop count of the request
//
// Every reply goes back into the control room tagged "@<sender> ...", so the
// operator sees what happened and which node did it.

#include <Arduino.h>
#include <Mesh.h>
#include "../companion_radio/MyMesh.h"

#ifndef MAX_GROUP_CHANNELS
#error "remote_test_companion needs a companion_radio build with MAX_GROUP_CHANNELS"
#endif

static unsigned long last_cmd_millis = 0;

// Channel names compare without a leading '#' and case-insensitively.
static bool channelNameEq(const char* name, const char* arg, size_t arg_len) {
  if (name[0] == '#') name++;
  if (arg_len > 0 && arg[0] == '#') { arg++; arg_len--; }
  return strlen(name) == arg_len && strncasecmp(name, arg, arg_len) == 0;
}

// Returns the channel index whose name matches, or -1.
static int findChannelByName(const char* arg, size_t arg_len, ChannelDetails& out) {
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (the_mesh.getChannel(i, out) && out.name[0] && channelNameEq(out.name, arg, arg_len)) return i;
  }
  return -1;
}

static void replyToRoom(ChannelDetails& room, const char* sender, const char* fmt, ...) {
  char reply[MAX_TEXT_LEN + 1];
  int n = snprintf(reply, sizeof(reply), "@%s ", sender);
  if (n < 0 || n >= (int)sizeof(reply)) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(reply + n, sizeof(reply) - n, fmt, args);
  va_end(args);
  the_mesh.sendGroupMessage(the_mesh.getRTCClock()->getCurrentTimeUnique(), room.channel,
                            the_mesh.getNodeName(), reply, strlen(reply));
}

void remoteTestOnChannelMsg(uint8_t channel_idx, mesh::Packet* pkt, uint32_t timestamp, const char* text) {
  ChannelDetails room;
  if (!the_mesh.getChannel(channel_idx, room)) return;
  if (!channelNameEq(room.name, REMOTE_TEST_CONTROL_CHANNEL, strlen(REMOTE_TEST_CONTROL_CHANNEL))) {
    return;   // not the control room -- the bot is deaf everywhere else
  }

  // Group text arrives as "<sender>: <body>" (BaseChatMesh::sendGroupMessage).
  char sender[32] = "?";
  const char* body = strstr(text, ": ");
  if (body) {
    size_t slen = body - text;
    if (slen >= sizeof(sender)) slen = sizeof(sender) - 1;
    memcpy(sender, text, slen);
    sender[slen] = 0;
    body += 2;
  } else {
    body = text;
  }

  // Optional "@<node name> " prefix: with several bots in one room, only the
  // named one acts and the rest stay silent.
  if (body[0] == '@') {
    const char* name = the_mesh.getNodeName();
    size_t nlen = strlen(name);
    body++;
    if (nlen == 0 || strncasecmp(body, name, nlen) != 0 || (body[nlen] != ' ' && body[nlen] != 0)) {
      return;   // addressed to someone else (or a bot's own "@<sender> ..." reply)
    }
    body += nlen;
  }
  while (*body == ' ') body++;
  if (body[0] != '!') return;   // ordinary chatter in the control room

  if (last_cmd_millis != 0 && millis() - last_cmd_millis < REMOTE_TEST_MIN_INTERVAL_MS) {
    MESH_DEBUG_PRINTLN("remote_test: rate-limited command from %s", sender);
    return;
  }
  last_cmd_millis = millis();

  if (strncasecmp(body, "!ping", 5) == 0 && (body[5] == 0 || body[5] == ' ')) {
    if (pkt->isRouteFlood()) {
      replyToRoom(room, sender, "pong from %s (snr %.1f, %u hops)", the_mesh.getNodeName(), pkt->getSNR(), (unsigned)pkt->path_len);
    } else {
      replyToRoom(room, sender, "pong from %s (snr %.1f, direct)", the_mesh.getNodeName(), pkt->getSNR());
    }
    return;
  }

  if (strncasecmp(body, "!msg", 4) == 0 && body[4] == ' ') {
    const char* p = body + 4;
    while (*p == ' ') p++;
    const char* chan = p;
    while (*p && *p != ' ') p++;
    size_t chan_len = p - chan;
    while (*p == ' ') p++;
    const char* msg = p;
    if (chan_len == 0 || chan[0] != '#' || msg[0] == 0) {
      replyToRoom(room, sender, "usage: !msg #<channel> <text>");
      return;
    }

    ChannelDetails target;
    int target_idx = findChannelByName(chan, chan_len, target);
    if (target_idx < 0) {
      replyToRoom(room, sender, "no channel named %.*s on %s", (int)chan_len, chan, the_mesh.getNodeName());
      return;
    }
    if (target_idx == channel_idx) {
      replyToRoom(room, sender, "refusing to post into the control room itself");
      return;
    }

    bool ok = the_mesh.sendGroupMessage(the_mesh.getRTCClock()->getCurrentTimeUnique(), target.channel,
                                        the_mesh.getNodeName(), msg, strlen(msg));
    if (ok) {
      replyToRoom(room, sender, "sent to #%s as %s: %.60s", target.name[0] == '#' ? target.name + 1 : target.name,
                  the_mesh.getNodeName(), msg);
    } else {
      replyToRoom(room, sender, "send failed (no packet buffer)");
    }
    return;
  }

  replyToRoom(room, sender, "unknown command; try !msg #<channel> <text> or !ping");
}
