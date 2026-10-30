#include "key.h"
#include "crypto.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint8_t record_threshold(const uint8_t *d) {
  uint8_t sum = 0;
  for (unsigned j = 0; j < 16; j++)
    sum += d[j];
  return sum >= 0xe0 ? 0xaa : sum <= 0x10 ? 0x55 : sum;
}
bool jm_jieli_record_key(const uint8_t d[34], uint16_t *out) {
  if (jm_crc16(d, 32) != rd16(d + 32))
    return false;
  uint8_t t = record_threshold(d);
  uint16_t key = 0;
  for (unsigned j = 0; j < 16; j++)
    if ((uint8_t)(d[16 + j] ^ d[15 - j]) < t)
      key |= (uint16_t)(1u << j);
  *out = key;
  return true;
}
/* Only the bytes whose bit must change are changed, to some value on the
 * right side of the threshold. */
void jm_jieli_set_record_key(uint8_t d[34], uint16_t key) {
  uint8_t t = record_threshold(d);
  for (unsigned j = 0; j < 16; j++) {
    uint8_t v = d[16 + j] ^ d[15 - j];
    bool want = key >> j & 1;
    if ((v < t) != want)
      d[16 + j] = d[15 - j] ^ (uint8_t)(want ? v % t : t + v % (256 - t));
  }
  wr16(d + 32, jm_crc16(d, 32));
}


#ifdef UNITTEST
#include <assert.h>
#include <string.h>
int main(void) {
  uint8_t record[34] = {0};
  uint16_t key;
  /* Exercise every possible key under the low, high and middle thresholds. */
  const uint8_t sums[] = {0, 0x80, 0xf0};
  for (unsigned t = 0; t < sizeof sums; ++t) {
    memset(record, 0, sizeof record);
    record[0] = sums[t];
    for (unsigned value = 0; value <= 0xffff; ++value) {
      jm_jieli_set_record_key(record, (uint16_t)value);
      assert(jm_jieli_record_key(record, &key) && key == value);
    }
  }
  record[33] ^= 1;
  key = 42;
  assert(!jm_jieli_record_key(record, &key) && key == 42);
  return 0;
}
#endif
