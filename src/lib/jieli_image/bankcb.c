#include "bankcb.h"
#include "crypto.h"
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
/* Transform BANKCB in place, checking each bank header CRC and data bounds.
 * False means invalid input; the buffer may already be partially transformed.
 * The caller must discard it on failure. No allocations or device access. */
bool jm_jieli_scramble_bankcb(uint8_t *p, size_t n) {
  if (n < 16)
    return false;
  unsigned count = rd16(p);
  if (!count || count > 256 || (size_t)count * 16 > n)
    return false;
  for (unsigned i = 0; i < count; i++) {
    uint8_t *h = p + i * 16;
    uint32_t off = rd32(h + 8);
    uint16_t size = rd16(h + 2);
    if ((uint64_t)off + size > n || jm_crc16(h, 14) != rd16(h + 14))
      return false;
    jm_jieli_enc(p + off, size, 0xffff);
    jm_jieli_enc(h, 16, 0xffff);
  }
  return true;
}
bool jm_jieli_descramble_bankcb(uint8_t *p, size_t n) {
  unsigned count = 1;
  for (unsigned i = 0; i < count; i++) {
    if ((size_t)(i + 1) * 16 > n)
      return false;
    uint8_t *h = p + i * 16;
    jm_jieli_enc(h, 16, 0xffff);
    if (jm_crc16(h, 14) != rd16(h + 14))
      return false;
    if (!i) {
      count = rd16(h);
      if (!count || count > 256)
        return false;
    }
    uint32_t off = rd32(h + 8);
    uint16_t size = rd16(h + 2);
    if ((uint64_t)off + size > n)
      return false;
    jm_jieli_enc(p + off, size, 0xffff);
  }
  return true;
}

#ifdef UNITTEST
#include <assert.h>
#include <string.h>
int main(void) {
  uint8_t image[20] = {1, 0, 4, 0}, original[20];
  image[8] = 16;
  memcpy(image + 16, "test", 4);
  uint16_t crc = jm_crc16(image, 14);
  image[14] = crc;
  image[15] = crc >> 8;
  memcpy(original, image, sizeof image);
  assert(jm_jieli_scramble_bankcb(image, sizeof image));
  assert(memcmp(image, original, sizeof image));
  assert(jm_jieli_descramble_bankcb(image, sizeof image));
  assert(!memcmp(image, original, sizeof image));
  assert(!jm_jieli_scramble_bankcb(image, 15));
  assert(!jm_jieli_scramble_bankcb(image, 19));
  image[14] ^= 1;
  assert(!jm_jieli_scramble_bankcb(image, sizeof image));
  memset(image, 0, sizeof image);
  assert(!jm_jieli_scramble_bankcb(image, sizeof image));
  assert(!jm_jieli_descramble_bankcb(image, 0));
  return 0;
}
#endif
