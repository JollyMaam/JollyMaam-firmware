#include "proto_jollymaam.h"
#include "jieli_image/crypto.h"
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
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
/* update.bin from an app (for the updater, src/updater/): the MAIN header
 * (its size, CRC of the app, entry, format, its offset above the updater,
 * the product, the header's own CRC; 0xff to JM_MAIN_HEAD_SIZE), then the
 * app.  Into out (room bytes); 0 on a product over 16 bytes or no room. */
int jm_update_image_make(const uint8_t *app, size_t size, const char *product, uint32_t at,
                         uint32_t entry, uint8_t *out, size_t room) {
  size_t n = 0;
  if (!app || !product || !out || size > 0xffffffffu - JM_MAIN_HEAD_SIZE || room < JM_MAIN_HEAD_SIZE + size)
    return 0;
  while (n < 17 && product[n]) ++n;
  if (!n || n > 16) return 0;
  memmove(out + JM_MAIN_HEAD_SIZE, app, size);
  memset(out, 0xff, JM_MAIN_HEAD_SIZE);
  memcpy(out, "MAIN", 4);
  wr32(out + JM_MAIN_SIZE_OFF, (uint32_t)size);
  wr32(out + JM_MAIN_CRC_OFF, jm_crc16(out + JM_MAIN_HEAD_SIZE, size));
  wr32(out + JM_MAIN_ENTRY_OFF, entry);
  wr32(out + JM_MAIN_FORMAT_OFF, JM_MAIN_VERSION);
  wr32(out + JM_MAIN_AT_OFF, at);
  memset(out + JM_MAIN_PRODUCT_OFF, 0, JM_PRODUCT_SIZE);
  memcpy(out + JM_MAIN_PRODUCT_OFF, product, n);
  wr32(out + JM_MAIN_HEADER_CRC_OFF, jm_crc16(out + JM_MAIN_CRC_START, JM_MAIN_CRC_SIZE));
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
  {
    uint8_t app[3] = {1, 2, 3}, made[JM_MAIN_HEAD_SIZE + 3];
    assert(!jm_update_image_make(app, 3, "test", 0x10000, 0x16120, made, sizeof made - 1));
    assert(!jm_update_image_make(app, 3, "seventeen-chars!!", 0x10000, 0x16120, made, sizeof made));
    assert(jm_update_image_make(app, 3, "test", 0x10000, 0x16120, made, sizeof made));
    assert(jm_update_image_check(made, sizeof made, "test"));
    assert(rd32(made + JM_MAIN_AT_OFF) == 0x10000 && rd32(made + JM_MAIN_ENTRY_OFF) == 0x16120);
    assert(made[44] == 0xff && made[JM_MAIN_HEAD_SIZE + 2] == 3);
  }

  return 0;
}
#endif
