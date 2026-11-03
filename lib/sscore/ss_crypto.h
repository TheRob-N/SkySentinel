// Small, dependency-free crypto for pairing: SHA-256, HMAC-SHA256 and X25519
// (Curve25519 Diffie-Hellman, RFC 7748; field arithmetic after TweetNaCl, public
// domain). Same code on every board and on the host, checked against the RFC test
// vectors in test/host. Not constant-time-audited beyond what TweetNaCl gives;
// it runs once per pairing.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {

struct Sha256 {
  void begin();
  void update(const void* data, size_t len);
  void finish(uint8_t out[32]);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  uint64_t total_ = 0;
  size_t n_ = 0;
};

void sha256(const void* data, size_t len, uint8_t out[32]);
void hmacSha256(const uint8_t* key, size_t key_len, const void* data, size_t len, uint8_t out[32]);

// out = scalar * point (both 32 bytes, little-endian, as in RFC 7748)
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
// Public key from a private key (scalar * 9)
void x25519Base(uint8_t pub[32], const uint8_t priv[32]);

// AES-128 / AES-256 encryption of one block (FIPS-197), and CTR mode with a big-endian
// counter over the whole 16-byte block (as mbedtls does): Meshtastic's channel cipher.
// key_len 16 or 32.
void aesEncryptBlock(const uint8_t* key, size_t key_len, const uint8_t in[16], uint8_t out[16]);
void aesCtr(const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n);

// Constant-time compare
bool equal(const uint8_t* a, const uint8_t* b, size_t n);

}  // namespace ss
