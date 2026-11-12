/* libjollymaam's operations against a simulated pad behind fake hooks: our
 * firmware (U2C_WIRELESS) on a port, which a command restarts into the
 * 8BitDo boot loader or our updater on the same port.  Its tool device's
 * report is the B80's, then a 64-byte one (the YC3121's), which gathers a
 * command's data from several reports. */
#include "../../src/lib/jollymaam.h"
#include "../../src/lib/proto_8bitdo.h"
#include "../../src/lib/jieli_image/crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { APP, LOADER, UPDATER, GONE };
static struct {
  int state, next, generation, starts, reboots, committed;
  uint8_t flash[8192], update[600], queue[16][128];
  unsigned queued, taken, written, begun;
  uint32_t client;
  uint8_t tag;
  size_t report, gathered;
  uint8_t gather[25 + 512];
} pad = {.report = 356};

static void wr32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int list(void *ctx, int (*visit)(void *, const struct jm_device_info *), void *user) {
  struct jm_device_info d;
  (void)ctx;
  if (pad.state == GONE) return 0;
  memset(&d, 0, sizeof d);
  d.kind = JM_DEVICE_HID;
  snprintf(d.port, sizeof d.port, "5-2");
  snprintf(d.id, sizeof d.id, "5-2#%d", pad.generation);
  snprintf(d.path, sizeof d.path, "hidraw%d", pad.generation);
  if (pad.state == LOADER) d.vid = 0x2dc8, d.pid = 0x3208, d.output_size = 64;
  else d.vid = 0x16c0, d.pid = 0x05df, d.usage_page = 0xffc2, d.feature_size = (uint16_t)pad.report;
  visit(user, &d);
  return 1;
}
static void *open_dev(void *ctx, const struct jm_device_info *d) { (void)ctx, (void)d; return &pad; }
static void close_dev(void *ctx, void *dev) { (void)ctx, (void)dev; }
static void nap(void *ctx, unsigned ms) {
  (void)ctx, (void)ms;
  if (pad.state == GONE) pad.state = pad.next;     /* back on the bus */
}
static void say(void *ctx, const char *line) { (void)ctx, fprintf(stderr, "  %s\n", line); }

static void answer(const uint8_t *body, size_t n) {
  size_t at = 0;
  pad.queued = pad.taken = 0;
  do {
    uint8_t *r = pad.queue[pad.queued++];
    size_t k = n - at < 56 ? n - at : 56;
    memset(r, 0, 64);
    wr32(r, pad.client), r[4] = pad.tag, r[5] = at + k < n ? 2 : 3, r[6] = (uint8_t)at;
    memcpy(r + 8, body + at, k);
    at += k;
  } while (at < n);
}

static void restart(int into) {
  pad.state = GONE, pad.next = into, pad.generation++;
}

/* Our firmware and updater: the tool device's commands. */
static int set_feature(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)ctx, (void)dev;
  assert(n == pad.report + 1 && r[0] == 0 && r[1] == 0xf5 && (r[19] & 2));
  size_t len = r[17] | r[18] << 8, size = 0, room = pad.report - 24;
  if ((r[19] & 1) && len > room) {          /* a piece of the command's data */
    size_t k = len - pad.gathered < room ? len - pad.gathered : room;
    assert(pad.report == 64 && len <= 512 && (!pad.gathered || !memcmp(pad.gather, r, 25)));
    memcpy(pad.gather, r, 25);
    memcpy(pad.gather + 25 + pad.gathered, r + 25, k);
    if ((pad.gathered += k) < len) return (int)n;
    pad.gathered = 0, r = pad.gather;
  }
  uint8_t cmd = r[2], body[88] = {0};
  uint32_t arg = rd32(r + 3);
  pad.tag = r[20], pad.client = rd32(r + 21);
  switch (cmd) {
  case 0:
    memcpy(body, "C2PD", 4), body[4] = 1, body[7] = pad.state == UPDATER, body[24] = 42;
    memcpy(body + 8, "test", 4), memcpy(body + 40, "U2C_WIRELESS", 12), size = 88;
    break;
  case 0x11: restart(LOADER); return (int)n;
  case 0x40: restart(UPDATER); return (int)n;
  case 0x41:
    size = 32, body[0] = 1, body[1] = pad.committed ? 2 : pad.begun ? 1 : 0, body[3] = pad.committed;
    wr32(body + 8, 600), wr32(body + 12, pad.written), wr32(body + 16, pad.begun ? 600 : 0), wr32(body + 24, 128);
    break;
  case 0x42: assert(pad.state == UPDATER && arg == 600); pad.written = 0, pad.begun = 1; break;
  case 0x43:
    assert(arg == pad.written && len <= 512);
    memcpy(pad.update + pad.written, r + 25, len), pad.written += (unsigned)len;
    break;
  case 0x44: pad.committed = pad.written == 600; break;
  case 0x10: pad.reboots++, restart(APP); return (int)n;
  default: assert(0);
  }
  answer(body, size);
  return (int)n;
}

/* The 8BitDo boot loader (Telink): its commands on output reports. */
static int write(void *ctx, void *dev, const uint8_t *report, size_t n) {
  const uint8_t *r = report + 1;
  uint8_t reply[64] = {0, 5};
  uint32_t at = rd32(r + 13);
  unsigned cmd = r[1], k = r[5];
  (void)ctx, (void)dev;
  assert(pad.state == LOADER && n == 65 && report[0] == 0 && r[0] == 5);
  reply[4] = (uint8_t)cmd, wr32(reply + 14, at);
  if (cmd == 7) { pad.starts++, restart(APP); return (int)n; }
  if (cmd == 8) reply[6] = 2, reply[18] = 0x1b, reply[19] = 0x30;
  else {
    assert(at >= 0x6000 && at + (cmd == 4 ? 4096 : k) <= 0x8000);
    if (cmd == 4) memset(pad.flash + at - 0x6000, 0xff, 4096);
    if (cmd == 3) memcpy(pad.flash + at - 0x6000, r + 17, k);
    if (cmd == 5) reply[6] = (uint8_t)k, memcpy(reply + 18, pad.flash + at - 0x6000, k);
  }
  pad.queued = 1, pad.taken = 0;
  memcpy(pad.queue[0], reply, 64);
  return (int)n;
}

static int read(void *ctx, void *dev, uint8_t *out, size_t n, unsigned timeout) {
  (void)ctx, (void)dev, (void)timeout;
  if (pad.taken >= pad.queued) return 0;
  memcpy(out, pad.queue[pad.taken++], n < 64 ? n : 64);
  return 64;
}

/* firmware.img as the U2CW's: the updater (a Telink image), update.bin at 0x1000. */
static void image_make(uint8_t image[8192], uint8_t update[600]) {
  memset(image, 0xff, 8192);
  memcpy(image + 8, "KNLT", 4);
  wr32(image + 24, 32);
  uint32_t crc = 0xffffffff;
  for (unsigned i = 0; i < 28; i++) {
    crc ^= image[i];
    for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  wr32(image + 28, crc);
  uint8_t app[4096 - 0x120];
  memset(app, 0xa5, sizeof app);
  assert(jm_update_image_make(app, sizeof app, "U2C_WIRELESS", 0x1000, 0x7120, image + 4096, 4096));
  memset(app, 0x5a, 600 - 0x120);
  assert(jm_update_image_make(app, 600 - 0x120, "U2C_WIRELESS", 0x1000, 0x7120, update, 600));
}

static int count(void *user, const struct jm_found *f) {
  assert(f->kind == JM_FOUND_OURS && !strcmp(f->product->pid, "U2C_WIRELESS") && f->id.uid[0] == 42);
  ++*(int *)user;
  return 0;
}

static int pick_first(void *user, const struct jm_found *f) {
  *(struct jm_found *)user = *f;
  return 1;
}

int main(void) {
  struct jm_hooks h = {0};
  struct jm jm = {0};
  struct jm_firmware fw = {0};
  struct jm_select sel = {NULL, NULL, 0, 0};
  static uint8_t image[8192], update[600];
  int n = 0;
  h.list = list, h.open = open_dev, h.close = close_dev, h.write = write, h.read = read;
  h.set_feature = set_feature, h.sleep = nap, h.say = say;
  jm.h = &h;
  image_make(image, update);
  fw.count = 2;
  fw.files[0] = (struct jm_firmware_file){"U2C_WIRELESS", "install", "f.img", image, sizeof image};
  fw.files[1] = (struct jm_firmware_file){"U2C_WIRELESS", "update", "u.bin", update, sizeof update};

  assert(jm_scan(&jm, count, &n) == 1 && n == 1);
  /* oemflash: our firmware into the boot loader, on its port; written,
   * read back (header sector last), started. */
  memset(pad.flash, 0, sizeof pad.flash);
  assert(!jm_oemflash(&jm, &fw, &sel));
  assert(!memcmp(pad.flash, image, sizeof image) && pad.starts == 1 && pad.next == APP);
  /* A product the firmware has nothing for is left alone. */
  struct jm_firmware none = {0};
  sel.pid = "U2C_BLUETOOTH";
  assert(jm_oemflash(&jm, &none, &sel) && pad.starts == 1);
  sel.pid = NULL;
  /* update: into the updater, update.bin through it, its restart. */
  nap(NULL, 0);                             /* the app back on the bus */
  assert(!jm_update(&jm, &fw, &sel));
  assert(pad.committed && !memcmp(pad.update, update, sizeof update) && pad.reboots == 1);
  /* A selected port with nothing on it: nothing happens. */
  sel.device = "1-1";
  assert(jm_update(&jm, &fw, &sel) && pad.reboots == 1);
  sel.device = NULL;
  /* With 64-byte reports: the header and each 128 bytes in several. */
  nap(NULL, 0);
  pad.report = 64, pad.committed = 0;
  memset(pad.update, 0, sizeof pad.update);
  assert(!jm_update(&jm, &fw, &sel));
  assert(pad.committed && !memcmp(pad.update, update, sizeof update) && pad.reboots == 2);
  /* A pad step by step: our firmware into its loader once, installed,
   * then its loader again and out of it. */
  struct jm_found first;
  struct jm_pad steps;
  nap(NULL, 0);
  memset(pad.flash, 0, sizeof pad.flash);
  assert(jm_scan(&jm, pick_first, &first) == 1 && first.kind == JM_FOUND_OURS);
  jm_pad_init(&steps, &jm, &fw, &first);
  assert(jm_pad_ready(&steps) && steps.ready && !strcmp(steps.product, "U2C_WIRELESS"));
  assert(steps.found.kind == JM_FOUND_8BITDO_LOADER && jm_pad_ready(&steps));   /* once */
  assert(jm_pad_install(&steps) && steps.gone && !steps.ready && pad.starts == 2);
  assert(!memcmp(pad.flash, image, sizeof image));
  nap(NULL, 0);
  assert(!jm_pad_reboot(&steps));               /* gone: not in its loader any more */
  assert(!jm_loader(&jm, &sel) && pad.state == LOADER);
  assert(jm_scan(&jm, pick_first, &first) == 1 && first.kind == JM_FOUND_8BITDO_LOADER);
  jm_pad_init(&steps, &jm, &fw, &first);
  assert(jm_pad_reboot(&steps) && pad.starts == 3);
  puts("device operations simulation passed");
  return 0;
}
