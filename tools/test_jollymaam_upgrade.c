#include "../src/lib/upgrade_8bitdo.h"
#include "../src/lib/jieli_image/crypto.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void image_make(uint8_t image[8192]) {
  memset(image, 0xff, 8192);
  memcpy(image + 8, "KNLT", 4);
  wr32(image + 24, 32);
  uint32_t crc = 0xffffffff;
  for (unsigned i = 0; i < 28; ++i) {
    crc ^= image[i];
    for (unsigned j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  wr32(image + 28, crc);
  memcpy(image + 4096, "MAIN", 4);
  memset(image + 4120, 0, 16);
  memcpy(image + 4120, "U2C_WIRELESS", 12);
  wr32(image + 4100, 4096 - 0x120);
  wr32(image + 4104, jm_crc16(image + 4096 + 0x120, 4096 - 0x120));
  wr32(image + 4112, 1);
  wr32(image + 4136, jm_crc16(image + 4100, 36));

}
static void simulate(int wrong_model, int corrupt_verify, int identical) {
  uint8_t image[8192], flash[8192], request[64], reply[64];
  image_make(image);
  memset(flash, 0, sizeof flash);
  if (identical) memcpy(flash, image, sizeof flash);
  struct jm_8bitdo_upgrade op;
  assert(jm_8bitdo_upgrade_init(&op, image, sizeof image));
  unsigned erases = 0, programs = 0, starts = 0, transfers = 0;
  int need_reply;
  enum jm_8bitdo_step step;
  while ((step = jm_8bitdo_upgrade_next(&op, request, &need_reply)) == JM_8BITDO_TRANSFER) {
    assert(++transfers < 2000);
    unsigned command = request[1], n = request[5];
    uint32_t address = rd32(request + 13);
    memset(reply, 0, sizeof reply);
    reply[1] = 5; reply[4] = command;
    wr32(reply + 14, address);
    if (command == 8) {
      reply[6] = 2; reply[18] = wrong_model ? 0x1c : 0x1b; reply[19] = 0x30;
    } else if (command == 7) {
      assert(!need_reply); ++starts;
    } else {
      assert(address >= 0x6000 && address < 0x8000);
      unsigned off = address - 0x6000;
      if (command == 4) {
        assert(address == (erases == 0 ? 0x7000 : 0x6000));
        assert(!(address & 4095));
        ++erases; memset(flash + off, 0xff, 4096);
      } else if (command == 3) {
        ++programs; memcpy(flash + off, request + 17, n);
      } else {
        assert(command == 5);
        reply[6] = n; memcpy(reply + 18, flash + off, n);
        if (corrupt_verify && programs && op.offset == 0) reply[18] ^= 1;
      }
    }
    jm_8bitdo_upgrade_complete(&op, 1, reply, sizeof reply);
  }
  if (wrong_model || corrupt_verify) {
    assert(step == JM_8BITDO_FAILED && !starts);
    if (wrong_model) assert(!erases && !programs);
  } else {
    assert(step == JM_8BITDO_DONE && starts == 1 && op.completed == 2);
    assert(!memcmp(flash, image, sizeof flash));
    assert(erases == (identical ? 0 : 2));
  }
}
int main(void) {
  simulate(0, 0, 0);
  simulate(0, 0, 1);
  simulate(1, 0, 0);
  simulate(0, 1, 0);
  uint8_t image[8192], report[64]; int need_reply;
  image_make(image);
  struct jm_8bitdo_upgrade op;
  assert(jm_8bitdo_upgrade_init(&op, image, sizeof image));
  assert(jm_8bitdo_upgrade_next(&op, report, &need_reply) == JM_8BITDO_TRANSFER);
  jm_8bitdo_upgrade_complete(&op, 0, NULL, 0);
  assert(jm_8bitdo_upgrade_next(&op, report, &need_reply) == JM_8BITDO_FAILED);
  image[8] ^= 1;
  assert(!jm_8bitdo_upgrade_init(&op, image, sizeof image));
  puts("8BitDo upgrade simulation passed");
  return 0;
}
