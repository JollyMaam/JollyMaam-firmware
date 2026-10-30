#include "entry.h"
#include "crypto.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void wr16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void wr32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

void jm_jieli_encode_entry(uint8_t out[32], const struct jm_jieli_entry *e) {
  memset(out, 0xff, 32);
  wr16(out + 2, e->data_crc);
  wr32(out + 4, e->offset);
  wr32(out + 8, e->size);
  out[12] = e->flags;
  out[13] = e->reserved;
  wr16(out + 14, e->index);
  memcpy(out + 16, e->name, 16);
  wr16(out, jm_crc16(out + 2, 30));
}

bool jm_jieli_decode_entry(const uint8_t *p, struct jm_jieli_entry *e) {
  if (jm_crc16(p + 2, 30) != rd16(p))
    return false;
  e->data_crc = rd16(p + 2);
  e->offset = rd32(p + 4);
  e->size = rd32(p + 8);
  e->flags = p[12];
  e->reserved = p[13];
  e->index = rd16(p + 14);
  memcpy(e->name, p + 16, 16);
  return true;
}

void jm_jieli_name_string(char out[17], const uint8_t raw[16]) {
  size_t i;
  for (i = 0; i < 16 && raw[i] && raw[i] != 0xff; i++)
    out[i] = (char)raw[i];
  out[i] = 0;
}

bool jm_jieli_header_at(const uint8_t *p, size_t n, uint32_t o, uint8_t clear[32]) {
  if ((uint64_t)o + 32 > n)
    return false;
  memcpy(clear, p + o, 32);
  jm_jieli_enc(clear, 32, 0xffff);
  return rd16(clear) && jm_crc16(clear + 2, 30) == rd16(clear);
}

uint32_t jm_jieli_eoffset(const uint8_t *p, size_t n, uint32_t o) {
  for (uint64_t off = (uint64_t)o + 32; off + 32 <= n && off < (uint64_t)o + 0x400; off += 32) {
    uint8_t h[32];
    struct jm_jieli_entry e;
    memcpy(h, p + off, 32);
    jm_jieli_enc(h, 32, 0xffff);
    if (!jm_jieli_decode_entry(h, &e))
      return 0;
    char name[17];
    jm_jieli_name_string(name, e.name);
    if (!strcmp(name, "isd_config.ini") && (uint64_t)o + e.offset + e.size <= n) {
      const uint8_t *d = p + o + e.offset;
      for (uint32_t i = 0; i + 12 <= e.size; i++)
        if (!memcmp(d + i, "EOFFSET", 8))
          return rd32(d + i + 8);
      return 0;
    }
    if (e.index)
      return 0;
  }
  return 0;
}

bool jm_jieli_find_base(const uint8_t *p, size_t n, uint8_t clear[32], uint32_t *base) {
  static const uint32_t candidates[] = {0,       0x1000,   0x10000,
                                        0x80000, 0x100000, 0x180000};
  for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
    uint32_t o = candidates[i];
    if (!jm_jieli_header_at(p, n, o, clear))
      continue;
    uint32_t units = jm_jieli_eoffset(p, n, o);
    uint64_t e = (uint64_t)units * 0x1000;
    if (e && e != o && e < n && jm_jieli_header_at(p, n, e, clear))
      { *base = e; return true; }
    jm_jieli_header_at(p, n, o, clear);
    *base = o;
    return true;
  }
  return false;
}

#ifdef UNITTEST
#include <assert.h>
int main(void) {
  struct jm_jieli_entry e = {0}, decoded;
  uint8_t record[32], image[32], clear[32];
  e.data_crc = 0x1234;
  e.offset = 0x78563412;
  e.size = 42;
  e.index = 1;
  memset(e.name, 0xff, 16);
  memcpy(e.name, "test", 5);
  jm_jieli_encode_entry(record, &e);
  assert(record[4] == 0x12 && record[7] == 0x78);
  assert(jm_jieli_decode_entry(record, &decoded));
  assert(decoded.offset == e.offset && decoded.size == e.size);
  char name[17];
  jm_jieli_name_string(name, decoded.name);
  assert(!strcmp(name, "test"));
  record[8] ^= 1;
  decoded.size = 123;
  assert(!jm_jieli_decode_entry(record, &decoded));
  assert(decoded.size == 123);
  jm_jieli_encode_entry(record, &e);
  memcpy(image, record, 32);
  jm_jieli_enc(image, 32, 0xffff);
  assert(!jm_jieli_header_at(image, 31, 0, clear));
  assert(!jm_jieli_header_at(image, 32, UINT32_MAX, clear));
  uint32_t base = 99;
  assert(jm_jieli_find_base(image, 32, clear, &base) && base == 0);
  assert(!memcmp(clear, record, 32));
  image[0] ^= 1;
  base = 99;
  assert(!jm_jieli_find_base(image, 32, clear, &base) && base == 99);
  return 0;
}
#endif
