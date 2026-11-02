#include "proto_jollymaam.h"
#include "jieli_image/crypto.h"
#include "sha256.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Decode IDENTIFY, accepting the original 56-byte and extended 88-byte form.
 * Strings are terminated. Output is untouched on failure. */
int jm_identify_decode(const uint8_t *data, size_t size, struct jm_identity *out) {
  if (size < JM_ID_SIZE || memcmp(data, "C2PD", 4)) return 0;
  struct jm_identity id;
  memset(&id, 0, sizeof id);
  id.protocol = rd16(data + JM_ID_PROTOCOL_OFF); id.board = data[JM_ID_BOARD_OFF]; id.updater = data[JM_ID_UPDATER_OFF];
  memcpy(id.build, data + JM_ID_BUILD_OFF, JM_ID_BUILD_SIZE); memcpy(id.uid, data + JM_ID_UID_OFF, JM_ID_UID_SIZE);
  memcpy(id.product, data + JM_ID_PRODUCT_OFF, JM_PRODUCT_SIZE);
  if (size >= JM_ID_EXT_SIZE) memcpy(id.board_name, data + JM_ID_BOARD_NAME_OFF, JM_ID_BOARD_NAME_SIZE);
  *out = id;
  return 1;
}
/* SCHEMA is optional: an absent/unknown reply means use field discovery.
 * Output is unchanged on failure; trailing extensions are accepted. */
int jm_schema_info_decode(const uint8_t *data, size_t size, struct jm_schema_info *out) {
  if (!data || !out || size < JM_SCHEMA_INFO_SIZE || data[0] != JM_SCHEMA_FORMAT ||
      data[1] != JM_SCHEMA_HASH_SHA256_128 || !rd16(data + 2)) return 0;
  struct jm_schema_info info;
  info.fields = rd16(data + 2);
  memcpy(info.digest, data + JM_SCHEMA_DIGEST_OFF, sizeof info.digest);
  *out = info;
  return 1;
}
/* Match an app's bundled/cached schema to firmware before skipping discovery.
 * The caller owns both buffers. Hashing happens on the host only. Unknown
 * formats, protocols and fingerprints all fall back to discovery. */
int jm_schema_matches(const struct jm_schema_info *info, const uint8_t *schema, size_t size) {
  if (!info || !schema || size < 10 || size > 65536 || memcmp(schema, "JMSC", 4) ||
      rd16(schema + 4) != JM_SCHEMA_FORMAT || rd16(schema + 6) != JM_PROTOCOL ||
      rd16(schema + 8) != info->fields) return 0;
  uint8_t digest[32];
  jm_sha256(schema, size, digest);
  return !memcmp(digest, info->digest, sizeof info->digest);
}
/* Decode battery fields from the compatible 16-byte STATUS reply. Legacy
 * zero padding means unknown capabilities; retain its original voltage.
 * Output is untouched on malformed frames or an invalid valid percentage. */
int jm_battery_decode(const uint8_t *status, size_t size, struct jm_battery_status *out) {
  if (!status || !out || size < JM_STATUS_SIZE) return 0;
  struct jm_battery_status b = {rd16(status + JM_STATUS_BATTERY_MV_OFF), status[JM_STATUS_BATTERY_PERCENT_OFF], status[JM_STATUS_BATTERY_FLAGS_OFF]};
  if ((b.flags & JM_BATTERY_PERCENT_VALID) && b.percent > 100) return 0;
  *out = b;
  return 1;
}
/* Logical SCSI-style command. HID/GATT framing belongs to the transport layer. */
void jm_command(uint8_t cdb[16], uint8_t command, uint32_t argument) {
  memset(cdb, 0, 16); cdb[0] = JM_OP; cdb[1] = command;
  for (unsigned i = 0; i < 4; ++i) cdb[2 + i] = (uint8_t)(argument >> (i * 8));
}
/* Validate clear update.bin: MAIN commit/header CRC, format, exact size,
 * application CRC and optional product match. No device I/O. expected_product
 * may be NULL; it must otherwise be at most 16 bytes plus terminator. */
int jm_update_image_check(const uint8_t *data, size_t size, const char *product) {
  if (size < JM_MAIN_HEAD_SIZE || memcmp(data, "MAIN", 4) || rd32(data + JM_MAIN_FORMAT_OFF) != JM_MAIN_VERSION ||
      rd32(data + JM_MAIN_SIZE_OFF) != size - JM_MAIN_HEAD_SIZE ||
      rd32(data + JM_MAIN_HEADER_CRC_OFF) != jm_crc16(data + JM_MAIN_CRC_START, JM_MAIN_CRC_SIZE) ||
      rd32(data + JM_MAIN_CRC_OFF) != jm_crc16(data + JM_MAIN_HEAD_SIZE, size - JM_MAIN_HEAD_SIZE))
    return 0;
  if (product) {
    size_t n = 0;
    while (n < 17 && product[n]) ++n;
    if (n > 16) return 0;
    uint8_t expected[16] = {0};
    memcpy(expected, product, n);
    if (memcmp(expected, data + JM_MAIN_PRODUCT_OFF, 16)) return 0;
  }
  return 1;
}
#ifdef UNITTEST
#include <assert.h>
int main(void) {
  uint8_t data[88] = {0}, cdb[16];
  struct jm_identity id;
  memset(&id, 0xa5, sizeof id);
  assert(!jm_identify_decode(data, sizeof data, &id));
  memcpy(data, "C2PD", 4); data[4] = 1; data[7] = 1;
  memset(data + 40, 'X', 16);
  assert(!jm_identify_decode(data, 55, &id));
  assert(jm_identify_decode(data, 56, &id));
  assert(id.protocol == 1 && id.updater == 1 && id.product[16] == 0);
  assert(!id.board_name[0]);
  struct jm_battery_status battery = {0};
  uint8_t status[16] = {0};
  status[4] = 0x58; status[5] = 0x0e; /* legacy 3672 mV */
  assert(jm_battery_decode(status, sizeof status, &battery));
  assert(battery.mv == 3672 && !battery.flags);
  status[6] = JM_BATTERY_MV_VALID | JM_BATTERY_PERCENT_VALID | JM_BATTERY_CHARGE_VALID;
  status[7] = 73;
  assert(jm_battery_decode(status, sizeof status, &battery) && battery.percent == 73);
  status[7] = 101;
  assert(!jm_battery_decode(status, sizeof status, &battery) && battery.percent == 73);
  assert(!jm_battery_decode(status, 15, &battery));
  uint8_t schema[12] = {'J', 'M', 'S', 'C', 1, 0, 2, 0, 1, 0, 0, 0};
  uint8_t digest[32], schema_reply[20] = {1, 1, 1, 0};
  struct jm_schema_info info;
  jm_sha256(schema, sizeof schema, digest);
  memcpy(schema_reply + 4, digest, 16);
  assert(!jm_schema_info_decode(NULL, 0, &info));
  assert(!jm_schema_info_decode(schema_reply, 19, &info));
  assert(jm_schema_info_decode(schema_reply, 20, &info));
  assert(info.fields == 1 && jm_schema_matches(&info, schema, sizeof schema));
  schema[11] ^= 1;
  assert(!jm_schema_matches(&info, schema, sizeof schema));
  schema[11] ^= 1;
  schema[6] = 3;
  assert(!jm_schema_matches(&info, schema, sizeof schema));
  schema_reply[1] = 2;
  assert(!jm_schema_info_decode(schema_reply, 20, &info));
  schema[6] = 2;
  assert(jm_schema_matches(&info, schema, sizeof schema));
  jm_command(cdb, 0x43, 0x12345678);
  assert(cdb[0] == 0xf5 && cdb[1] == 0x43 && cdb[2] == 0x78 && cdb[5] == 0x12);
  assert(!jm_update_image_check(data, sizeof data, NULL));
  uint8_t update[0x121] = {0};
  memcpy(update, "MAIN", 4); update[4] = 1; update[16] = 1;
  memcpy(update + 24, "test", 4);
  uint16_t crc = jm_crc16(update + JM_MAIN_HEAD_SIZE, 1);
  update[8] = crc; update[9] = crc >> 8;
  crc = jm_crc16(update + 4, 36);
  update[40] = crc; update[41] = crc >> 8;
  assert(jm_update_image_check(update, sizeof update, "test"));
  assert(!jm_update_image_check(update, sizeof update, "wrong"));
  assert(!jm_update_image_check(update, sizeof update - 1, NULL));
  update[JM_MAIN_HEAD_SIZE] ^= 1;
  assert(!jm_update_image_check(update, sizeof update, NULL));

  return 0;
}
#endif
