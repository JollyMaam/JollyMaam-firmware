#include "crypto.h"

uint16_t jm_crc16_seed(const uint8_t *p, size_t n, uint16_t crc) {
  while (n--) {
    crc ^= (uint16_t)*p++ << 8;
    for (unsigned i = 0; i < 8; i++)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
  }
  return crc;
}
uint16_t jm_crc16(const uint8_t *p, size_t n) {
  return jm_crc16_seed(p, n, 0);
}

/* Symmetric JieLi ENC stream and the flash-controller block variant. */
void jm_jieli_enc(uint8_t *p, size_t n, uint16_t key) {
  while (n--) {
    *p++ ^= key;
    key = (uint16_t)((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0));
  }
}
void jm_jieli_sfc(uint8_t *image, uint32_t off, uint32_t n, uint32_t base,
                   uint16_t key) {
  for (uint32_t i = 0; i < n; i += 32) {
    uint32_t m = n - i < 32 ? n - i : 32;
    jm_jieli_enc(image + off + i, m, (uint16_t)(key ^ ((off + i - base) >> 2)));
  }
}


#ifdef UNITTEST
#include <assert.h>
#include <string.h>

int main(void) {
  const uint8_t text[] = "123456789";
  assert(jm_crc16(text, 9) == 0x31c3);
  assert(jm_crc16_seed(text + 4, 5, jm_crc16(text, 4)) == 0x31c3);
  uint8_t data[65], original[65], split[65];
  for (unsigned i = 0; i < sizeof data; ++i)
    data[i] = (uint8_t)i;
  memcpy(original, data, sizeof data);
  jm_jieli_enc(data, sizeof data, 0xffff);
  assert(data[0] == 0xff && data[1] == 0xde);
  jm_jieli_enc(data, sizeof data, 0xffff);
  assert(memcmp(data, original, sizeof data) == 0);
  memcpy(split, data, sizeof data);
  jm_jieli_sfc(data, 0, sizeof data, 0, 0x1234);
  jm_jieli_sfc(split, 0, 32, 0, 0x1234);
  jm_jieli_sfc(split, 32, 33, 0, 0x1234);
  assert(memcmp(data, split, sizeof data) == 0);
  jm_jieli_sfc(data, 0, sizeof data, 0, 0x1234);
  assert(memcmp(data, original, sizeof data) == 0);
  return 0;
}
#endif
