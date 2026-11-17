#include "ufw.h"
#include "crypto.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Detect a UFW wrapper and return its first type-0 flash member as a borrowed
 * view. RAW returns the original buffer. Errors leave both outputs untouched.
 * Input must remain alive while the returned view is used. No allocation occurs.
 * As in the original tool, a header without a valid UFW CRC is treated as raw. */
enum jm_jieli_ufw_result jm_jieli_ufw_open(const uint8_t *data, size_t size,
                                         const uint8_t **flash, size_t *flash_size) {
  uint8_t header[0x40];
  if (size < sizeof header)
    goto raw;
  memcpy(header, data, sizeof header);
  jm_jieli_enc(header, sizeof header, 0xffff);
  if (jm_crc16(header + 2, 62) != rd16(header))
    goto raw;
  uint16_t count = rd16(header + 8);
  size_t list_size = (size_t)count * 0x50;
  if (!count || list_size > size - sizeof header ||
      jm_crc16(data + sizeof header, list_size) != rd16(header + 2))
    return JM_JIELI_UFW_BAD_LIST;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t item[0x50];
    memcpy(item, data + sizeof header + (size_t)i * sizeof item, sizeof item);
    jm_jieli_enc(item, sizeof item, 0xffff);
    if (rd16(item) != 0)
      continue;
    uint32_t offset = rd32(item + 8), length = rd32(item + 12);
    if (offset > size || length > size - offset)
      return JM_JIELI_UFW_TRUNCATED;
    if (jm_crc16(data + offset, length) != rd16(item + 4))
      return JM_JIELI_UFW_BAD_CRC;
    *flash = data + offset;
    *flash_size = length;
    return JM_JIELI_UFW_OK;
  }
  return JM_JIELI_UFW_NO_FLASH;
raw:
  *flash = data;
  *flash_size = size;
  return JM_JIELI_UFW_RAW;
}

#ifdef UNITTEST
#include <assert.h>
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
/* Build a minimal transport package independently of the parser. */
static void fixture(uint8_t data[148], unsigned type, unsigned offset) {
  memset(data, 0, 148);
  memcpy(data + 144, "test", 4);
  wr16(data + 64, (uint16_t)type);
  wr16(data + 68, jm_crc16(data + 144, 4));
  data[72] = (uint8_t)offset;
  data[76] = 4;
  jm_jieli_enc(data + 64, 80, 0xffff);
  wr16(data + 2, jm_crc16(data + 64, 80));
  data[8] = 1;
  wr16(data, jm_crc16(data + 2, 62));
  jm_jieli_enc(data, 64, 0xffff);
}
int main(void) {
  uint8_t data[148];
  const uint8_t *flash = NULL;
  size_t size = 999;
  assert(jm_jieli_ufw_open(NULL, 0, &flash, &size) == JM_JIELI_UFW_RAW);
  assert(flash == NULL && size == 0);
  fixture(data, 0, 144);
  assert(jm_jieli_ufw_open(data, sizeof data, &flash, &size) == JM_JIELI_UFW_OK);
  assert(size == 4 && !memcmp(flash, "test", 4));
  flash = NULL; size = 999;
  assert(jm_jieli_ufw_open(data, 143, &flash, &size) == JM_JIELI_UFW_BAD_LIST);
  assert(flash == NULL && size == 999);
  assert(jm_jieli_ufw_open(data, 147, &flash, &size) == JM_JIELI_UFW_TRUNCATED);
  data[147] ^= 1;
  assert(jm_jieli_ufw_open(data, sizeof data, &flash, &size) == JM_JIELI_UFW_BAD_CRC);
  fixture(data, 1, 144);
  assert(jm_jieli_ufw_open(data, sizeof data, &flash, &size) == JM_JIELI_UFW_NO_FLASH);
  fixture(data, 0, 255);
  assert(jm_jieli_ufw_open(data, sizeof data, &flash, &size) == JM_JIELI_UFW_TRUNCATED);
  return 0;
}
#endif
