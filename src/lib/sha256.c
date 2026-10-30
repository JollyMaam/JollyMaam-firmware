#include "sha256.h"
#include <string.h>

/* SHA-256 round constants from the algorithm specification. */
static const uint32_t K[] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};
static uint32_t rotate(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }
static void block(uint32_t state[8], const uint8_t data[64]) {
  uint32_t w[64];
  for (unsigned i = 0; i < 16; ++i)
    w[i] = (uint32_t)data[i*4] << 24 | (uint32_t)data[i*4+1] << 16 |
           (uint32_t)data[i*4+2] << 8 | data[i*4+3];
  for (unsigned i = 16; i < 64; ++i) {
    uint32_t x = w[i-15], y = w[i-2];
    w[i] = w[i-16] + (rotate(x,7) ^ rotate(x,18) ^ (x >> 3)) + w[i-7] +
           (rotate(y,17) ^ rotate(y,19) ^ (y >> 10));
  }
  uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
  uint32_t e=state[4], f=state[5], g=state[6], h=state[7];
  for (unsigned i = 0; i < 64; ++i) {
    uint32_t t = h + (rotate(e,6) ^ rotate(e,11) ^ rotate(e,25)) +
                 ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t u = (rotate(a,2) ^ rotate(a,13) ^ rotate(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
    h=g; g=f; f=e; e=d+t; d=c; c=b; b=a; a=t+u;
  }
  state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
  state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}
/* Compute a complete SHA-256 digest without allocation. The output may overlap
 * input because it is written only after every input byte has been consumed.
 * Hashes detect corruption; an unsigned manifest does not establish authenticity. */
void jm_sha256(const uint8_t *data, size_t size, uint8_t digest[32]) {
  uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  uint64_t bits = (uint64_t)size * 8;
  while (size >= 64) { block(state, data); data += 64; size -= 64; }
  uint8_t tail[128] = {0};
  if (size) memcpy(tail, data, size);
  tail[size] = 0x80;
  size_t total = size < 56 ? 64 : 128;
  for (unsigned i = 0; i < 8; ++i) tail[total - 1 - i] = (uint8_t)(bits >> (i * 8));
  block(state, tail);
  if (total == 128) block(state, tail + 64);
  for (unsigned i = 0; i < 32; ++i) digest[i] = (uint8_t)(state[i/4] >> (24 - 8*(i%4)));
}
#ifdef UNITTEST
#include <assert.h>
int main(void) {
  uint8_t digest[32];
  const uint8_t empty[32] = {0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
  jm_sha256(NULL, 0, digest); assert(!memcmp(digest, empty, 32));
  return 0;
}
#endif
