// combined_node — repeater-grade forward policy
//
// Compile-time extension of examples/companion_radio (only built by the
// *_combined_node_* envs, which define -D WITH_RELAY_POLICY). This upgrades the
// plain `client_repeat` relay into a real range-extending repeater by adding:
//   - flood hop limits  (don't keep re-flooding packets that already went far)
//   - loop detection     (drop floods this node already appears in too often)
// ported from examples/simple_repeater's allowPacketForward / isLooped.
//
// Deliberately omitted: transport-code/region (scope) enforcement. A dedicated
// repeater drops un-scoped floods of unknown region; for a general car repeater
// we WANT to carry un-scoped floods so it extends range for everyone. Companion
// has no region_map, so bringing that in would be a large, needless change.

#include "MyMesh.h"

#ifdef WITH_RELAY_POLICY

#include <Arduino.h>

// Max times THIS node may already appear in a flood packet's path before we
// treat it as a loop, indexed by path-hash entry size (1/2/3 bytes).
// Ported verbatim from simple_repeater.
static const uint8_t loop_minimal[]  = { 0, 4, 2, 1 };
static const uint8_t loop_moderate[] = { 0, 2, 1, 1 };
static const uint8_t loop_strict[]   = { 0, 1, 1, 1 };

bool MyMesh::relayPolicyAllows(const mesh::Packet* packet) {
  // Direct / path-routed packets where we're the next hop are always forwarded
  // (the path already says we belong on it). Only floods need filtering.
  if (!packet->isRouteFlood()) return true;

  // Limits are compile-time build flags (see MyMesh.h WITH_RELAY_POLICY block).
  const uint8_t hops = packet->getPathHashCount();
  if (hops >= RELAY_FLOOD_MAX) return false;
  if (packet->getRouteType() == ROUTE_TYPE_FLOOD && hops >= RELAY_FLOOD_MAX_UNSCOPED) return false;
  if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT && hops >= RELAY_FLOOD_MAX_ADVERT) return false;

#if RELAY_LOOP_DETECT != 0
  const uint8_t hash_size = packet->getPathHashSize();
  if (hash_size < (sizeof(loop_moderate) / sizeof(loop_moderate[0]))) {
    const uint8_t* maxima = (RELAY_LOOP_DETECT == 1) ? loop_minimal
                          : (RELAY_LOOP_DETECT == 3) ? loop_strict
                                                     : loop_moderate;
    uint8_t remaining = hops, seen = 0;
    const uint8_t* path = packet->path;
    while (remaining > 0) {
      if (self_id.isHashMatch(path, hash_size)) seen++;
      path += hash_size;
      remaining--;
    }
    if (seen >= maxima[hash_size]) return false; // already relayed too many times -> loop
  }
#endif

  return true;
}

#endif // WITH_RELAY_POLICY
