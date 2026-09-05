// Ed25519-signed JWT for MQTT authentication, minted on the node.
//
// Collectors in the wider MeshCore observer ecosystem (CoreScope and the
// letsmesh/meshmapper/cascadiamesh family) authenticate a node by proof of
// possession of its MESH IDENTITY KEY rather than by an account credential:
// there is no token to register for or fetch. The node signs a short JWT with
// the same Ed25519 key that is its mesh identity, connects with username
// `v1_<UPPERCASE_PUBKEY>` and passes the token as the password; the collector
// verifies the signature against that public key.
//
// Wire-compatible with agessaman/MeshCore `observer-firmware` (JWTHelper):
//   header   {"alg":"EdDSA","typ":"JWT"}
//   payload  {"publicKey":"<64 hex, uppercase>","aud":"<host>","iat":…,"exp":…}
//   token    base64url(header).base64url(payload).base64url(sig)   -- no padding
//
// Signing is over the ASCII "header.payload", per RFC 7515. We use
// LocalIdentity::sign() directly rather than exporting the keypair.
//
// NOTE this needs a correct clock: `iat`/`exp` are absolute unix seconds and a
// collector will reject a token from a node whose RTC never got an SNTP sync.

#pragma once

#include <Identity.h>
#include <stddef.h>
#include <stdint.h>

// A signed token is ~350 bytes for a bare publicKey/aud/iat/exp payload. 768
// matches the buffer the reference implementation reserves, which leaves room
// for a longer audience without a surprise truncation.
#define OBS_JWT_MAX_LEN 768

// Seconds a minted token stays valid. Re-minted on every MQTT (re)connect, so
// this only has to outlast a single session, but keep it comfortably long so a
// node with modest clock skew is still inside the window.
#ifndef OBS_JWT_EXPIRY_S
  #define OBS_JWT_EXPIRY_S (24 * 3600UL)
#endif

// Mint a token for `audience`, signed by `identity`.
//   issued_at  unix seconds; 0 = use time(nullptr)
//   returns    token length, or 0 on failure (bad args, no clock, overflow)
size_t observerJwtCreate(const mesh::LocalIdentity& identity,
                         const char* audience,
                         uint32_t issued_at,
                         char* token, size_t token_size);

// The MQTT username that pairs with the token: `v1_` + uppercase hex pubkey.
// Returns false if the buffer is too small (needs 3 + 64 + 1 bytes).
bool observerJwtUsername(const mesh::LocalIdentity& identity,
                         char* out, size_t out_size);
