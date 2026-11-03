// Pairing a node with a master, the math part (the radio part lives in the firmware).
//
//   1. An unpaired node broadcasts MSG_PAIR_REQ with a fresh X25519 public key.
//   2. A master in pairing mode (ADD NODE, 2 minutes) that hears it close by answers
//      with MSG_PAIR_OFFER: its own fresh public key. Both sides now compute the same
//      shared secret, a 32-byte session key, and a 6-digit code from it.
//   3. The code shows on the master's page and on the node's screen. Someone in the
//      middle swapping keys would make the two codes differ. Screenless nodes blink
//      their LED on IDENTIFY instead.
//   4. ACCEPT: MSG_PAIR_KEY carries the cluster key, XORed with a pad from the session
//      key and tagged with an HMAC over who it's for. The node checks the tag, saves the
//      key and the master's address, and restarts into the encrypted cluster.
//
// Pure C++, host tested (including a key-swapping attacker).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_proto.h"

namespace ss {

struct PairSecret {
  uint8_t k[32] = {0};   // session key
  uint32_t code = 0;     // 0..999999, shown as "123 456"
};

// shared = X25519(my private, their public). node_pub / master_pub fix the order.
void pairDerive(const uint8_t shared[32], const uint8_t node_pub[32], const uint8_t master_pub[32], PairSecret& out);
// Convenience: both steps from my private key and the two public keys.
void pairDeriveFrom(const uint8_t my_priv[32], const uint8_t peer_pub[32], const uint8_t node_pub[32],
                    const uint8_t master_pub[32], PairSecret& out);

// Seal / open the cluster key for one node. Open fails (false) on a wrong tag, a key
// for someone else, or a bad length.
bool pairSeal(const PairSecret& s, const uint8_t target[6], const char* cluster_key, PairKey& out);
bool pairOpen(const PairSecret& s, const uint8_t me[6], const PairKey& in, char cluster_key[kPairKeyMax + 1]);

void pairCodeText(uint32_t code, char out[8]);   // "123 456"
// A new cluster key from 16 random bytes: 32 hex characters.
void clusterKeyFromRandom(const uint8_t rnd[16], char out[33]);

}  // namespace ss
