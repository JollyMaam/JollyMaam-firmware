/* libjollymaam's pad steps against a simulated Cyclone 2 (BR23, 1 MiB) in
 * its Jieli ROM loader, behind fake hooks: the RAM loader uploaded once a
 * session, a backup, our firmware installed (the rest erased), then
 * restores: the backup (all but the factory data), a backup of another pad
 * (refused), another product's image (refused), a vendor image (the rest
 * erased), and another pad's backup raw (all of it).  The factory data is
 * erased by the raw write only. */
#include "../../src/lib/jollymaam.h"
#include "../../src/lib/proto_jieli.h"
#include "../../src/lib/jieli_image/crypto.h"
#include "../../src/lib/jieli_image/entry.h"
#include "../../src/lib/jieli_image/key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH 0x100000
#define FACTORY 0xfe000
enum { ROM, HELPER, GONE };
static struct {
  int state, generation, uploads, starts, factory_erases;
  uint8_t flash[FLASH];
} pad;

static uint32_t rd32be(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static int list(void *ctx, int (*visit)(void *, const struct jm_device_info *), void *user) {
  struct jm_device_info d;
  (void)ctx;
  if (pad.state == GONE) return 0;
  memset(&d, 0, sizeof d);
  d.kind = JM_DEVICE_DISK;
  snprintf(d.port, sizeof d.port, "3-1");
  snprintf(d.id, sizeof d.id, "3-1#%d", pad.generation);
  snprintf(d.path, sizeof d.path, "sg%d", pad.generation);
  visit(user, &d);
  return 1;
}
static void *open_dev(void *ctx, const struct jm_device_info *d) { (void)ctx, (void)d; return &pad; }
static void close_dev(void *ctx, void *dev) { (void)ctx, (void)dev; }
static void nap(void *ctx, unsigned ms) { (void)ctx, (void)ms; }
static void say(void *ctx, const char *line) { (void)ctx, fprintf(stderr, "  %s\n", line); }

static int scsi(void *ctx, void *dev, const uint8_t *cdb, size_t cdb_size, enum jm_scsi_direction dir,
                uint8_t *data, size_t size, unsigned timeout, size_t *done) {
  unsigned command = cdb[0] << 8 | cdb[1];
  uint32_t at = rd32be(cdb + 2);
  (void)ctx, (void)dev, (void)cdb_size, (void)timeout;
  assert(pad.state != GONE);
  *done = size;
  if (cdb[0] == 0x12) {                        /* INQUIRY */
    memset(data, ' ', size);
    memcpy(data + 8, "BR23", 4), memcpy(data + 16, "UBOOT1.00", 9);
    return 0;
  }
  if (dir == JM_SCSI_IN && size == 16 && command != 0xfd05) {
    memset(data, 0, 16);
    data[0] = cdb[0], data[1] = cdb[1];        /* the echo of an executed command */
  }
  switch (command) {
  case 0xfb06: assert(pad.state == ROM && dir == JM_SCSI_OUT); return 0;      /* RAM loader upload */
  case 0xfb08: assert(pad.state == ROM); pad.uploads++, pad.state = HELPER; return 0;
  case 0xfc0a: assert(pad.state == HELPER); data[4] = 20; return 0;          /* flash ID: 1 MiB */
  case 0xfc14: assert(pad.state == HELPER); data[4] = 2; return 0;           /* 512-byte transfers */
  case 0xfc09: {                                                             /* chip key 0xffff */
    uint8_t key[2] = {0xff, 0xff};
    jm_jieli_crc_cipher(key, 2, 0xffffffff);
    data[7] = key[0], data[6] = key[1];
    return 0;
  }
  case 0xfd05:
    assert(pad.state == HELPER && at + size <= FLASH);
    memcpy(data, pad.flash + at, size);
    return 0;
  case 0xfb00: case 0xfb01: {
    uint32_t n = command == 0xfb00 ? 0x10000 : 0x1000;
    assert(pad.state == HELPER && at + n <= FLASH);
    if (at + n > FACTORY) pad.factory_erases++;
    memset(pad.flash + at, 0xff, n);
    return 0;
  }
  case 0xfb04:
    assert(pad.state == HELPER && dir == JM_SCSI_OUT && at + size <= FLASH);
    for (size_t i = 0; i < size; i++) pad.flash[at + i] &= data[i];
    return 0;
  case 0xfc0c: assert(pad.state == HELPER); pad.starts++, pad.state = GONE; return 0;
  }
  assert(0);
  return -1;
}

/* A Jieli image naming product, its app area filled with fill (as
 * test_jollymaam_jieli.c's fixture). */
static void entry(uint8_t *p, const char *name, unsigned offset, unsigned size, unsigned flags, unsigned last,
                  unsigned crc) {
  struct jm_jieli_entry e = {0};
  memcpy(e.name, name, strlen(name)), e.offset = offset, e.size = size;
  e.flags = (uint8_t)flags, e.index = (uint16_t)last, e.data_crc = (uint16_t)crc;
  jm_jieli_encode_entry(p, &e);
}
static void fixture(uint8_t *image, size_t size, const char *product, uint8_t fill) {
  memset(image, 0xff, size);
  memset(image + 16, 0, 16), memcpy(image + 16, product, strlen(product));
  jm_jieli_enc(image, 32, 0xffff), image[2] = 1;
  wr16(image, jm_crc16(image + 2, 30)), jm_jieli_enc(image, 32, 0xffff);
  memset(image + 128, 0, 34), jm_jieli_set_record_key(image + 128, 0xffff);
  entry(image + 32, "isd_config.ini", 128, 34, 2, 0, jm_crc16(image + 128, 34));
  entry(image + 64, "app_dir_head", 512, 0xffffffff, 0x81, 1, 0xffff);
  memset(image + 512 + 64, fill, 32);
  entry(image + 512 + 32, "app.bin", 64, 32, 0x82, 1, jm_crc16(image + 512 + 64, 32));
  entry(image + 512, "app_area_head", 0x1e00120, 96, 0x83, 1, jm_crc16(image + 512 + 32, 64));
  jm_jieli_enc(image + 32, 32, 0xffff), jm_jieli_enc(image + 64, 32, 0xffff);
  jm_jieli_sfc(image, 512, 4096 - 512, 512, 0xffff);
}

static uint8_t got[FLASH];
static int keep(void *user, uint32_t address, const uint8_t *data, size_t size) {
  (void)user;
  assert(address + size <= FLASH);
  memcpy(got + address, data, size);
  return 1;
}

static int pick(void *user, const struct jm_found *f) {
  *(struct jm_found *)user = *f;
  return 1;
}

/* A new session on the pad, back in its ROM loader. */
static void session(struct jm *jm, const struct jm_firmware *fw, struct jm_pad *p) {
  struct jm_found f;
  pad.state = ROM, pad.generation++;
  assert(jm_scan(jm, pick, &f) == 1 && f.kind == JM_FOUND_JIELI_LOADER);
  jm_pad_init(p, jm, fw, &f);
}

int main(void) {
  struct jm_hooks h = {0};
  struct jm jm = {0};
  struct jm_firmware fw = {0};
  struct jm_pad p;
  static uint8_t loader[1024], ours[4096], vendor[4096], other[4096], stock[FLASH], foreign[FLASH];
  h.list = list, h.open = open_dev, h.close = close_dev, h.scsi = scsi, h.sleep = nap, h.say = say;
  jm.h = &h;
  fixture(ours, sizeof ours, "GS_C2_ADC_DEVICE", 0x5a);
  fixture(vendor, sizeof vendor, "GS_C2_ADC_DEVICE", 0x3c);
  fixture(other, sizeof other, "AC695X_TRANS", 0x3c);
  fw.count = 2;
  fw.files[0] = (struct jm_firmware_file){"BR23", "loader", "br23loader.bin", loader, sizeof loader};
  fw.files[1] = (struct jm_firmware_file){"GS_C2_ADC_DEVICE", "install", "f.img", ours, sizeof ours};

  /* Stock: its image at 0, settings (VM) at 0xf7000, factory data at 0xfe000. */
  memset(pad.flash, 0xff, FLASH);
  fixture(pad.flash, 4096, "GS_C2_ADC_DEVICE", 0xa5);
  memset(pad.flash + 0xf7000, 0x11, 64);
  for (unsigned i = 0; i < 0x2000; i++) pad.flash[FACTORY + i] = (uint8_t)(i * 7 + 3);
  memcpy(stock, pad.flash, FLASH);

  /* Backup then install in one session: one RAM loader upload. */
  session(&jm, &fw, &p);
  assert(jm_pad_backup(&p, keep, NULL) && !memcmp(got, stock, FLASH) && pad.uploads == 1);
  assert(!strcmp(p.product, "GS_C2_ADC_DEVICE"));
  assert(jm_pad_install(&p) && p.gone && pad.uploads == 1 && pad.starts == 1);
  assert(!memcmp(pad.flash, ours, sizeof ours) && !memcmp(pad.flash + FACTORY, stock + FACTORY, 0x2000));
  for (unsigned i = sizeof ours; i < FACTORY; i++) assert(pad.flash[i] == 0xff);   /* stock's settings gone */
  assert(!pad.factory_erases);
  assert(!jm_pad_backup(&p, keep, NULL));                 /* gone: a new session first */

  /* Our firmware's settings, then the stock backup back: all of it. */
  memset(pad.flash + 0xfa000, 0x22, 64);
  session(&jm, &fw, &p);
  assert(jm_pad_restore(&p, stock, FLASH, 0) && pad.uploads == 2 && pad.starts == 2);
  assert(!memcmp(pad.flash, stock, FLASH) && !pad.factory_erases);

  /* Another pad's backup: refused, nothing written. */
  memcpy(foreign, stock, FLASH);
  foreign[FACTORY + 5] ^= 0xff;
  session(&jm, &fw, &p);
  assert(!jm_pad_restore(&p, foreign, FLASH, 0) && !memcmp(pad.flash, stock, FLASH) && pad.state == HELPER);

  /* Another product's image: refused. */
  assert(!jm_pad_restore(&p, other, sizeof other, 0) && !memcmp(pad.flash, stock, FLASH));

  /* A vendor image: at 0, the settings erased up to the factory data. */
  assert(jm_pad_restore(&p, vendor, sizeof vendor, 0) && pad.uploads == 3);
  assert(!memcmp(pad.flash, vendor, sizeof vendor) && !pad.factory_erases);
  for (unsigned i = sizeof vendor; i < FACTORY; i++) assert(pad.flash[i] == 0xff);
  assert(!memcmp(pad.flash + FACTORY, stock + FACTORY, 0x2000));

  /* Raw: a vendor image refused, another pad's backup written whole. */
  session(&jm, &fw, &p);
  assert(!jm_pad_restore(&p, vendor, sizeof vendor, 1) && pad.state == HELPER);
  assert(jm_pad_restore(&p, foreign, FLASH, 1) && !memcmp(pad.flash, foreign, FLASH) && pad.factory_erases);
  puts("pad steps simulation passed");
  return 0;
}
