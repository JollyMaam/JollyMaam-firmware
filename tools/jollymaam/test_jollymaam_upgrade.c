#include "../../src/lib/upgrade_8bitdo.h"
#include "../../src/lib/jieli_image/crypto.h"
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
/* The Yichip loader as the pad's behaves: identify 1a 30 (length 2,
 * total 2); an erase at the base with the stream's length first, else
 * programs are lost; programs carry the cipher of the padded app, each with
 * the stream's length as total; reads give the clear flash. */
static void simulate_yichip(int wrong_model, int corrupt_verify) {
  static uint8_t app[40000], stream[65536], cipher[65536], flash[65536];
  uint8_t request[64], reply[64];
  for (unsigned i = 0; i < sizeof app; ++i) app[i] = (uint8_t)(i * 13 + 5);
  memset(stream, 0xff, sizeof stream);
  memcpy(stream, app, sizeof app);
  for (unsigned b = 0; b < sizeof stream / 512; ++b) jm_8bitdo_yichip_encrypt(b, stream + 512 * b, cipher + 512 * b);
  memset(flash, 0x5a, sizeof flash);
  struct jm_8bitdo_upgrade op;
  assert(!jm_8bitdo_yichip_init(&op, app, 0x64001));
  assert(jm_8bitdo_yichip_init(&op, app, sizeof app) && op.stream == 65536 && op.sectors == 32);
  unsigned erased = 0, programmed = 0, starts = 0, transfers = 0, last = 0;
  int need_reply;
  enum jm_8bitdo_step step;
  while ((step = jm_8bitdo_upgrade_next(&op, request, &need_reply)) == JM_8BITDO_TRANSFER) {
    assert(++transfers < 4000);
    unsigned command = request[1], n = request[5];
    uint32_t total = rd32(request + 9), address = rd32(request + 13);
    memset(reply, 0, sizeof reply);
    reply[1] = 5; reply[4] = command;
    wr32(reply + 14, address);
    if (command == 8) {
      assert(n == 2 && total == 2);
      reply[6] = 4; reply[18] = wrong_model ? 0x1b : 0x1a; reply[19] = 0x30;   /* as the pad's: 4 bytes */
    } else if (command == 7) {
      assert(!need_reply); ++starts;
    } else {
      assert(address >= 0x01018000 && address < 0x01018000 + sizeof flash);
      unsigned off = address - 0x01018000;
      if (command == 4) {
        assert(!off && total == 65536 && !programmed);
        memset(flash, 0xff, sizeof flash); erased = 1;
      } else if (command == 3) {
        assert(erased && total == 65536 && off == programmed && n == (65536 - off < 46 ? 65536 - off : 46));
        assert(!memcmp(request + 17, cipher + off, n));
        memcpy(flash + off, stream + off, n);   /* the loader decrypts */
        programmed += n;
        reply[6] = 0x77;                       /* status-only ack: its length is not one */
      } else {
        assert(command == 5 && programmed == 65536 && n <= 46 && off / 4096 == (off + n - 1) / 4096);
        reply[6] = n; memcpy(reply + 18, flash + off, n);
        if (corrupt_verify && off == 40960) reply[18] ^= 1;
      }
    }
    jm_8bitdo_upgrade_complete(&op, 1, reply, sizeof reply);
    assert(op.completed >= last && op.completed <= op.sectors);
    last = op.completed;
  }
  if (wrong_model || corrupt_verify) {
    assert(step == JM_8BITDO_FAILED && !starts);
    if (wrong_model) assert(!erased && !programmed);
  } else {
    assert(step == JM_8BITDO_DONE && starts == 1 && op.completed == op.sectors);
    assert(!memcmp(flash, stream, sizeof flash));
  }
}
int main(void) {
  simulate_yichip(0, 0);
  simulate_yichip(1, 0);
  simulate_yichip(0, 1);
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
