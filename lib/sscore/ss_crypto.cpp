#include "ss_crypto.h"

#include <string.h>

namespace ss {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

static const uint32_t K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256::begin() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h_, init, sizeof(h_));
  total_ = 0;
  n_ = 0;
}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + S1 + ch + K256[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  total_ += len;
  while (len) {
    const size_t take = len < 64 - n_ ? len : 64 - n_;
    memcpy(buf_ + n_, p, take);
    n_ += take; p += take; len -= take;
    if (n_ == 64) { block(buf_); n_ = 0; }
  }
}

void Sha256::finish(uint8_t out[32]) {
  const uint64_t bits = total_ * 8;
  const uint8_t one = 0x80, zero = 0;
  update(&one, 1);
  while (n_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = (uint8_t)(h_[i] >> 24); out[4 * i + 1] = (uint8_t)(h_[i] >> 16);
    out[4 * i + 2] = (uint8_t)(h_[i] >> 8); out[4 * i + 3] = (uint8_t)h_[i];
  }
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
  Sha256 s;
  s.begin();
  s.update(data, len);
  s.finish(out);
}

void hmacSha256(const uint8_t* key, size_t key_len, const void* data, size_t len, uint8_t out[32]) {
  uint8_t k[64] = {0}, pad[64], inner[32];
  if (key_len > 64) sha256(key, key_len, k); else memcpy(k, key, key_len);
  Sha256 s;
  for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
  s.begin(); s.update(pad, 64); s.update(data, len); s.finish(inner);
  for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
  s.begin(); s.update(pad, 64); s.update(inner, 32); s.finish(out);
}

bool equal(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
  return d == 0;
}

// ---------------------------------------------------------------------------
// X25519 (TweetNaCl's field arithmetic: 16 limbs of 16 bits in int64)

typedef int64_t gf[16];
static const gf k121665 = {0xDB41, 1};

static void car(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (int64_t)1 << 16;
    const int64_t c = o[i] >> 16;             // arithmetic shift (floor)
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * 65536;
  }
}
static void sel(gf p, gf q, int b) {
  const int64_t c = ~((int64_t)b - 1);
  for (int i = 0; i < 16; ++i) {
    const int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}
static void pack(uint8_t* o, const gf n) {
  gf m, t;
  for (int i = 0; i < 16; ++i) t[i] = n[i];
  car(t); car(t); car(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = (uint8_t)(t[i] & 0xff);
    o[2 * i + 1] = (uint8_t)(t[i] >> 8);
  }
}
static void unpack(gf o, const uint8_t* n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}
static void add(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
static void sub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
static void mul(gf o, const gf a, const gf b) {
  int64_t t[31] = {0};
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  car(o);
  car(o);
}
static void sqr(gf o, const gf a) { mul(o, a, a); }
static void inv(gf o, const gf in) {
  gf c;
  for (int a = 0; a < 16; ++a) c[a] = in[a];
  for (int a = 253; a >= 0; --a) {
    sqr(c, c);
    if (a != 2 && a != 4) mul(c, c, in);
  }
  for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
  uint8_t z[32];
  gf x, a, b, c, d, e, f;
  for (int i = 0; i < 31; ++i) z[i] = scalar[i];
  z[31] = (uint8_t)((scalar[31] & 127) | 64);
  z[0] &= 248;
  unpack(x, point);
  for (int i = 0; i < 16; ++i) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
  a[0] = d[0] = 1;
  for (int i = 254; i >= 0; --i) {
    const int r = (z[i >> 3] >> (i & 7)) & 1;
    sel(a, b, r);
    sel(c, d, r);
    add(e, a, c);
    sub(a, a, c);
    add(c, b, d);
    sub(b, b, d);
    sqr(d, e);
    sqr(f, a);
    mul(a, c, a);
    mul(c, b, e);
    add(e, a, c);
    sub(a, a, c);
    sqr(b, a);
    sub(c, d, f);
    mul(a, c, k121665);
    add(a, a, d);
    mul(c, c, a);
    mul(a, d, f);
    mul(d, b, x);
    sqr(b, e);
    sel(a, b, r);
    sel(c, d, r);
  }
  inv(c, c);
  mul(a, a, c);
  pack(out, a);
}

void x25519Base(uint8_t pub[32], const uint8_t priv[32]) {
  uint8_t nine[32] = {9};
  x25519(pub, priv, nine);
}


// ---------------------------------------------------------------------------
// AES (encryption only; CTR needs nothing else)

static const uint8_t kSbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

// Expanded key: 4 * (rounds + 1) words, as bytes.
static int aesExpand(const uint8_t* key, size_t key_len, uint8_t rk[240]) {
  const int nk = key_len == 32 ? 8 : 4, rounds = nk + 6, total = 4 * (rounds + 1);
  memcpy(rk, key, (size_t)nk * 4);
  uint8_t rcon = 1;
  for (int i = nk; i < total; ++i) {
    uint8_t t[4] = {rk[4 * (i - 1)], rk[4 * (i - 1) + 1], rk[4 * (i - 1) + 2], rk[4 * (i - 1) + 3]};
    if (i % nk == 0) {
      const uint8_t a = t[0];
      t[0] = (uint8_t)(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]]; t[2] = kSbox[t[3]]; t[3] = kSbox[a];
      rcon = xtime(rcon);
    } else if (nk == 8 && i % nk == 4) {
      for (auto& b : t) b = kSbox[b];
    }
    for (int j = 0; j < 4; ++j) rk[4 * i + j] = (uint8_t)(rk[4 * (i - nk) + j] ^ t[j]);
  }
  return rounds;
}

static void aesBlock(const uint8_t* rk, int rounds, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(in[i] ^ rk[i]);
  for (int r = 1; r <= rounds; ++r) {
    uint8_t t[16];
    for (int i = 0; i < 16; ++i) t[i] = kSbox[s[(i + 4 * (i % 4)) % 16]];   // SubBytes + ShiftRows
    if (r != rounds) {
      for (int c = 0; c < 4; ++c) {                                      // MixColumns
        uint8_t* col = t + 4 * c;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
        col[0] = (uint8_t)(a0 ^ x ^ xtime((uint8_t)(a0 ^ a1)));
        col[1] = (uint8_t)(a1 ^ x ^ xtime((uint8_t)(a1 ^ a2)));
        col[2] = (uint8_t)(a2 ^ x ^ xtime((uint8_t)(a2 ^ a3)));
        col[3] = (uint8_t)(a3 ^ x ^ xtime((uint8_t)(a3 ^ a0)));
      }
    }
    for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(t[i] ^ rk[16 * r + i]);
  }
  memcpy(out, s, 16);
}

void aesEncryptBlock(const uint8_t* key, size_t key_len, const uint8_t in[16], uint8_t out[16]) {
  uint8_t rk[240];
  aesBlock(rk, aesExpand(key, key_len, rk), in, out);
}

void aesCtr(const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n) {
  uint8_t rk[240], ctr[16], ks[16];
  const int rounds = aesExpand(key, key_len, rk);
  memcpy(ctr, iv, 16);
  for (size_t off = 0; off < n; off += 16) {
    aesBlock(rk, rounds, ctr, ks);
    for (size_t i = 0; i < 16 && off + i < n; ++i) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    for (int i = 15; i >= 0; --i) if (++ctr[i]) break;
  }
}
}  // namespace ss
