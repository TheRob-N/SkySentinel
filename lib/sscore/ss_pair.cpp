#include "ss_pair.h"

#include <stdio.h>
#include <string.h>

#include "ss_crypto.h"

namespace ss {

static const char kLabel[] = "SkySentinel pair v1";

void pairDerive(const uint8_t shared[32], const uint8_t node_pub[32], const uint8_t master_pub[32], PairSecret& out) {
  uint8_t msg[sizeof(kLabel) + 64];
  memcpy(msg, kLabel, sizeof(kLabel));
  memcpy(msg + sizeof(kLabel), node_pub, 32);
  memcpy(msg + sizeof(kLabel) + 32, master_pub, 32);
  hmacSha256(shared, 32, msg, sizeof(msg), out.k);
  uint8_t c[32];
  hmacSha256(out.k, 32, "code", 4, c);
  const uint32_t v = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
  out.code = v % 1000000u;
}

void pairDeriveFrom(const uint8_t my_priv[32], const uint8_t peer_pub[32], const uint8_t node_pub[32],
                    const uint8_t master_pub[32], PairSecret& out) {
  uint8_t shared[32];
  x25519(shared, my_priv, peer_pub);
  pairDerive(shared, node_pub, master_pub, out);
  memset(shared, 0, sizeof(shared));
}

static void tagOf(const PairSecret& s, const PairKey& m, uint8_t tag[16]) {
  uint8_t msg[6 + 1 + kPairKeyMax];
  memcpy(msg, m.target, 6);
  msg[6] = m.len;
  memcpy(msg + 7, m.sealed, kPairKeyMax);
  uint8_t full[32];
  hmacSha256(s.k, 32, msg, sizeof(msg), full);
  memcpy(tag, full, 16);
}

static void padOf(const PairSecret& s, uint8_t pad[32]) { hmacSha256(s.k, 32, "seal", 4, pad); }

bool pairSeal(const PairSecret& s, const uint8_t target[6], const char* cluster_key, PairKey& out) {
  const size_t len = strlen(cluster_key);
  if (len == 0 || len > kPairKeyMax) return false;
  out = PairKey();
  memcpy(out.target, target, 6);
  out.len = (uint8_t)len;
  uint8_t pad[32];
  padOf(s, pad);
  for (size_t i = 0; i < kPairKeyMax; ++i) out.sealed[i] = (uint8_t)((i < len ? (uint8_t)cluster_key[i] : 0) ^ pad[i]);
  tagOf(s, out, out.tag);
  return true;
}

bool pairOpen(const PairSecret& s, const uint8_t me[6], const PairKey& in, char cluster_key[kPairKeyMax + 1]) {
  if (memcmp(in.target, me, 6) != 0 || in.len == 0 || in.len > kPairKeyMax) return false;
  uint8_t tag[16];
  tagOf(s, in, tag);
  if (!equal(tag, in.tag, 16)) return false;
  uint8_t pad[32];
  padOf(s, pad);
  for (size_t i = 0; i < kPairKeyMax; ++i) cluster_key[i] = (char)(in.sealed[i] ^ pad[i]);
  cluster_key[in.len] = '\0';
  for (size_t i = 0; i < in.len; ++i)
    if (cluster_key[i] < 0x20 || cluster_key[i] > 0x7e) return false;  // keys are printable text
  return true;
}

void pairCodeText(uint32_t code, char out[8]) {
  snprintf(out, 8, "%03lu %03lu", (unsigned long)(code / 1000 % 1000), (unsigned long)(code % 1000));
}

void clusterKeyFromRandom(const uint8_t rnd[16], char out[33]) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    out[2 * i] = hex[rnd[i] >> 4];
    out[2 * i + 1] = hex[rnd[i] & 15];
  }
  out[32] = '\0';
}

}  // namespace ss
