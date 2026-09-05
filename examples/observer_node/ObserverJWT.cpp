#include "MyMesh.h"

#ifdef WITH_OBSERVER_EXTRAS

#include "ObserverJWT.h"
#include <MeshCore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// base64url, no padding (RFC 7515 §2). Deliberately hand-rolled rather than
// pulling in mbedtls_base64_encode + a translate pass: this is ~15 lines, has
// no ESP32 dependency (so the file still builds for a non-ESP observer), and
// avoids a second full-size scratch buffer for the standard-alphabet form.
static const char B64URL[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url(const uint8_t* in, size_t in_len, char* out, size_t out_size) {
  size_t need = (in_len * 4 + 2) / 3;          // unpadded length
  if (need + 1 > out_size) return 0;
  size_t o = 0;
  for (size_t i = 0; i < in_len; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    size_t rem = in_len - i;
    if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
    if (rem > 2) v |= (uint32_t)in[i + 2];
    out[o++] = B64URL[(v >> 18) & 0x3F];
    out[o++] = B64URL[(v >> 12) & 0x3F];
    if (rem > 1) out[o++] = B64URL[(v >> 6) & 0x3F];   // 2 bytes -> 3 chars
    if (rem > 2) out[o++] = B64URL[v & 0x3F];          // 3 bytes -> 4 chars
  }
  out[o] = 0;
  return o;
}

static void pubkeyHexUpper(const mesh::LocalIdentity& identity, char* out /* >=65 */) {
  mesh::Utils::toHex(out, identity.pub_key, PUB_KEY_SIZE);
  for (int i = 0; out[i]; i++) {
    if (out[i] >= 'a' && out[i] <= 'f') out[i] = (char)(out[i] - 'a' + 'A');
  }
}

bool observerJwtUsername(const mesh::LocalIdentity& identity, char* out, size_t out_size) {
  if (out == NULL || out_size < 3 + (PUB_KEY_SIZE * 2) + 1) return false;
  char hex[(PUB_KEY_SIZE * 2) + 1];
  pubkeyHexUpper(identity, hex);
  snprintf(out, out_size, "v1_%s", hex);
  return true;
}

size_t observerJwtCreate(const mesh::LocalIdentity& identity,
                         const char* audience,
                         uint32_t issued_at,
                         char* token, size_t token_size) {
  if (audience == NULL || audience[0] == 0 || token == NULL || token_size == 0) return 0;

  if (issued_at == 0) issued_at = (uint32_t)time(NULL);
  // A node that never got an SNTP sync would mint a token dated to the RTC
  // seed; every collector would reject it. Fail loudly here instead, so the
  // caller can retry once the clock is disciplined.
  if (issued_at < 1600000000UL) return 0;   // ~2020-09; before that is not a real clock

  char pk_hex[(PUB_KEY_SIZE * 2) + 1];
  pubkeyHexUpper(identity, pk_hex);

  // header.payload, each base64url'd, built straight into `token`.
  static const char HEADER_JSON[] = "{\"alg\":\"EdDSA\",\"typ\":\"JWT\"}";
  size_t o = b64url((const uint8_t*)HEADER_JSON, sizeof(HEADER_JSON) - 1, token, token_size);
  if (o == 0) return 0;

  char payload[320];
  int pl = snprintf(payload, sizeof(payload),
                    "{\"publicKey\":\"%s\",\"aud\":\"%s\",\"iat\":%lu,\"exp\":%lu}",
                    pk_hex, audience,
                    (unsigned long)issued_at,
                    (unsigned long)(issued_at + OBS_JWT_EXPIRY_S));
  if (pl <= 0 || pl >= (int)sizeof(payload)) return 0;   // audience too long

  if (o + 1 >= token_size) return 0;
  token[o++] = '.';
  size_t pn = b64url((const uint8_t*)payload, (size_t)pl, token + o, token_size - o);
  if (pn == 0) return 0;
  o += pn;

  // Sign the ASCII "header.payload" exactly as assembled so far.
  uint8_t sig[SIGNATURE_SIZE];
  identity.sign(sig, (const uint8_t*)token, (int)o);

  if (o + 1 >= token_size) return 0;
  token[o++] = '.';
  size_t sn = b64url(sig, sizeof(sig), token + o, token_size - o);
  if (sn == 0) return 0;
  return o + sn;
}

#endif  // WITH_OBSERVER_EXTRAS
