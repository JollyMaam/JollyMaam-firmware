#include "proto_jieli.h"
#include "jieli_image/crypto.h"
#include <string.h>

static void be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
int jm_jieli_cdb(uint8_t out[16], uint16_t command,
                  const uint8_t *arguments, size_t size) {
  if (size > 14 || (size && !arguments)) return 0;
  memset(out, 0xff, 16);
  out[0] = command >> 8; out[1] = command;
  if (size) memcpy(out + 2, arguments, size);
  return 1;
}
int jm_jieli_read_cdb(uint8_t out[16], uint16_t command,
                       uint32_t address, size_t size) {
  if (!size || size > 0xffff || size - 1 > UINT32_MAX - address) return 0;
  uint8_t args[6]; be32(args, address);
  args[4] = (uint8_t)(size >> 8); args[5] = (uint8_t)size;
  return jm_jieli_cdb(out, command, args, sizeof args);
}
int jm_jieli_write_cdb(uint8_t out[16], uint16_t command,
                        uint32_t address, const uint8_t *data, size_t size) {
  if (!data || !size || size > 0xffff || size - 1 > UINT32_MAX - address) return 0;
  uint8_t args[9]; be32(args, address);
  args[4] = (uint8_t)(size >> 8); args[5] = (uint8_t)size; args[6] = 0;
  uint16_t crc = jm_crc16(data, size);
  args[7] = crc; args[8] = crc >> 8;
  return jm_jieli_cdb(out, command, args, sizeof args);
}
int jm_jieli_exec_reply(const uint8_t *reply, size_t size, uint16_t command,
                         uint8_t out[14]) {
  if (size != 16 || reply[0] != (uint8_t)(command >> 8) || reply[1] != (uint8_t)command)
    return 0;
  if (out) memcpy(out, reply + 2, 14);
  return 1;
}
void jm_jieli_crc_cipher(uint8_t *data, size_t size, uint32_t key) {
  static const uint8_t magic[16] = {0xc3, 0xcf, 0xc0, 0xe8, 0xce, 0xd2,
    0xb0, 0xae, 0xc4, 0xe3, 0xa3, 0xac, 0xd3, 0xf1, 0xc1, 0xd6};
  uint8_t k[2] = {(uint8_t)(key >> 16), (uint8_t)(key >> 24)};
  uint16_t crc = jm_crc16_seed(k, 2, (uint16_t)key);
  for (size_t i = 0; i < size; ++i) {
    crc = jm_crc16_seed(magic + i % 16, 1, crc);
    data[i] ^= (uint8_t)crc;
  }
}
#ifdef UNITTEST
#include <assert.h>
int main(void) {
  uint8_t cdb[16], copy[16], data[] = {1, 2, 3}, reply[16] = {0xfc, 0x0a};
  assert(jm_jieli_write_cdb(cdb, 0xfb06, 0x12345678, data, 3));
  assert(cdb[0] == 0xfb && cdb[1] == 6 && cdb[2] == 0x12 && cdb[5] == 0x78);
  assert(cdb[6] == 0 && cdb[7] == 3 && cdb[8] == 0 && cdb[15] == 0xff);
  memcpy(copy, cdb, 16);
  assert(!jm_jieli_cdb(cdb, 0, data, 15) && !memcmp(copy, cdb, 16));
  assert(!jm_jieli_read_cdb(cdb, 0xfd05, UINT32_MAX, 2));
  assert(!jm_jieli_write_cdb(cdb, 0xfb06, 0, data, 65536));
  assert(jm_jieli_exec_reply(reply, sizeof reply, 0xfc0a, NULL));
  assert(!jm_jieli_exec_reply(reply, 15, 0xfc0a, NULL));
  assert(!jm_jieli_exec_reply(reply, 16, 0xfc09, NULL));
  jm_jieli_crc_cipher(data, 3, 0xffffffff);
  jm_jieli_crc_cipher(data, 3, 0xffffffff);
  assert(data[0] == 1 && data[1] == 2 && data[2] == 3);
  return 0;
}
#endif
