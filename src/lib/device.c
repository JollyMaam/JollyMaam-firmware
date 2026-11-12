/* Devices through the application's hooks (jollymaam.h): finding them by
 * USB port, our firmware's tool commands and updater, the vendor loaders,
 * stock firmware's loader entry, and the operations on top. */
#include "jollymaam.h"
#include "proto_8bitdo.h"
#include "proto_jieli.h"
#include "upgrade_8bitdo.h"
#include "upgrade_jieli.h"
#include "upgrade_jollymaam.h"
#include "jieli_image/entry.h"
#include "jieli_image/image.h"
#include "jieli_image/ufw.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOADER_VID 0x2dc8
#define LOADER_PID 0x3208
#define FOUND_MAX 32

static void say(struct jm *jm, const char *fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  jm->h->say(jm->h->ctx, line);
}

static void progress(struct jm *jm, const char *what, size_t done, size_t total) {
  if (jm->h->progress)
    jm->h->progress(jm->h->ctx, what, done, total);
}

static void *dev_open(struct jm *jm, const struct jm_device_info *info) {
  return jm->h->open(jm->h->ctx, info);
}
static void dev_close(struct jm *jm, void *dev) {
  if (dev)
    jm->h->close(jm->h->ctx, dev);
}

/* ---- our tool device ----------------------------------------------------------------------
 * A command is a feature report (the CDB, u16 length, flags, tag, u32
 * client, data); its answer comes on input reports addressed to this
 * program's client (client, tag, state, u16 offset, 56 bytes), others'
 * passing by.  REFUSED: another program's command runs; sent again.  On a
 * device with 64-byte reports a command's data goes on in further reports
 * with the same head, which it gathers (JM_HID_GATHER). */

static uint32_t client_of(struct jm *jm) {
  if (!jm->client) {
    uintptr_t mix = (uintptr_t)jm ^ (uintptr_t)&mix << 9 ^ (uintptr_t)jm->h;
    jm->client = ((uint32_t)mix ^ (uint32_t)((uint64_t)mix >> 32)) * 0x9e3779b9u | 1;
  }
  return jm->client;
}

/* The most data a command takes to the device. */
static size_t tool_room(size_t report) {
  return report == JM_HID_REPORT_SMALL ? JM_HID_GATHER : report - JM_HID_CLIENT_HEAD_SIZE;
}

static int tool(struct jm *jm, void *dev, size_t report, const struct jm_update_request *r, uint8_t *answer) {
  uint8_t buffer[JM_HID_REPORT_JIELI + 1], in[128];
  uint32_t client = client_of(jm);
  size_t n = r->out_size ? r->out_size : r->in_size, piece = report - JM_HID_CLIENT_HEAD_SIZE;
  if (report < 64 || report > JM_HID_REPORT_JIELI || r->out_size > tool_room(report) || r->in_size > 512)
    return 0;
  for (unsigned tries = 0; tries < 2000; tries++) {
    memset(buffer, 0, sizeof buffer);
    memcpy(buffer + 1, r->cdb, 16);
    buffer[17] = (uint8_t)n;
    buffer[18] = (uint8_t)(n >> 8);
    buffer[19] = (uint8_t)(JM_HID_INPUT | (r->out_size ? JM_HID_OUT : 0));
    buffer[20] = ++jm->tag;
    for (int i = 0; i < 4; i++) buffer[21 + i] = (uint8_t)(client >> 8 * i);
    for (int k = 0; k < 256 && jm->h->read(jm->h->ctx, dev, in, sizeof in, 0) > 0; k++)
      ;
    int sent;
    size_t at = 0;
    do {                                /* the data, a report's room at a time */
      size_t k = r->out_size - at < piece ? r->out_size - at : piece;
      memset(buffer + 1 + JM_HID_CLIENT_HEAD_SIZE, 0, piece);
      if (k) memcpy(buffer + 1 + JM_HID_CLIENT_HEAD_SIZE, r->data + at, k);
      at += k;
      sent = jm->h->set_feature(jm->h->ctx, dev, buffer, report + 1);
    } while (sent == (int)report + 1 && at < r->out_size);
    /* These restart the device: their answer may never come. */
    if (r->cdb[1] == JM_CMD_REBOOT || r->cdb[1] == JM_CMD_UPDATER || r->cdb[1] == JM_CMD_REBOOT_LOADER)
      return 1;
    if (sent != (int)report + 1) {
      say(jm, "command %u: the tool device did not take the request (%d of %zu bytes)", r->cdb[1], sent, report + 1);
      return 0;
    }
    size_t got = 0;
    int refused = 0;
    for (unsigned waits = 0; waits < 150 && !refused;) {
      int k = jm->h->read(jm->h->ctx, dev, in, sizeof in, 100);
      if (k < 0) {
        say(jm, "command %u: reading its answer failed", r->cdb[1]);
        return 0;
      }
      if (k == 0) { waits++; continue; }          /* 15 s without a piece of ours */
      if (k < JM_HID_INPUT_HEAD_SIZE) continue;
      uint32_t whose = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
      if (whose != client || in[4] != jm->tag) continue;
      if (in[5] == JM_HID_REFUSED) { refused = 1; break; }
      size_t offset = in[6] | (size_t)in[7] << 8, length = (size_t)k - JM_HID_INPUT_HEAD_SIZE;
      if ((in[5] != JM_HID_MORE && in[5] != JM_HID_LAST) || offset != got) {
        say(jm, "command %u: its answer came out of order (status %u at %zu, expected %zu)", r->cdb[1], in[5], offset, got);
        return 0;
      }
      if (length > r->in_size - got) length = r->in_size - got;
      memcpy(answer + got, in + JM_HID_INPUT_HEAD_SIZE, length);
      got += length;
      if (in[5] == JM_HID_LAST) return got == r->in_size;
    }
    if (!refused) {
      say(jm, "command %u: no answer in 15 s (%zu of %zu bytes)", r->cdb[1], got, r->in_size);
      return 0;
    }
    jm->h->sleep(jm->h->ctx, 1 + (client >> (tries % 24) & 7));
  }
  say(jm, "command %u: refused 2000 times (the tool device stayed busy with other clients)", r->cdb[1]);
  return 0;
}

static int tool_simple(struct jm *jm, void *dev, size_t report, uint8_t command, uint8_t *answer, size_t n) {
  struct jm_update_request r;
  memset(&r, 0, sizeof r);
  jm_command(r.cdb, command, 0);
  r.in_size = n;
  return tool(jm, dev, report, &r, answer);
}

static int identify(struct jm *jm, void *dev, size_t report, struct jm_identity *id) {
  uint8_t reply[JM_ID_EXT_SIZE];
  return tool_simple(jm, dev, report, 0, reply, sizeof reply) && jm_identify_decode(reply, sizeof reply, id);
}

/* ---- the 8BitDo boot loader and stock 8BitDo firmware --------------------------------------- */

static int report_write(struct jm *jm, void *dev, uint8_t id, const uint8_t report[64]) {
  uint8_t out[65];
  out[0] = id;
  memcpy(out + 1, report, 64);
  return jm->h->write(jm->h->ctx, dev, out, sizeof out) == (int)sizeof out;
}

static void drain(struct jm *jm, void *dev) {
  uint8_t in[128];
  for (int k = 0; k < 64 && jm->h->read(jm->h->ctx, dev, in, sizeof in, 0) > 0; k++)
    ;
}

static uint16_t loader_model(struct jm *jm, void *dev) {
  uint8_t request[64], reply[128];
  const uint8_t *data;
  size_t n;
  jm_8bitdo_request(request, 8, 0, NULL, 4);
  drain(jm, dev);
  if (!report_write(jm, dev, 0, request))
    return 0;
  for (int k = 0; k < 8; k++) {
    int got = jm->h->read(jm->h->ctx, dev, reply, sizeof reply, 1000);
    if (got <= 0) return 0;
    if (jm_8bitdo_reply(reply, (size_t)got, 8, 0, &data, &n) == JM_8BITDO_OK && n >= 2)
      return (uint16_t)(data[0] | data[1] << 8);
  }
  return 0;
}

/* Stock: the loader's identify on output report 0x81, answered on input
 * report 0x02. */
static uint16_t stock_model(struct jm *jm, void *dev) {
  uint8_t out[64] = {0x05, 0x08}, in[128];
  drain(jm, dev);
  if (!report_write(jm, dev, 0x81, out)) return 0;
  for (int k = 0; k < 16; k++) {
    int got = jm->h->read(jm->h->ctx, dev, in, sizeof in, 500);
    if (got <= 0) return 0;
    if (got >= 20 && in[0] == 0x02 && in[1] == 0x05 && !in[2] && !in[3] && in[4] == 8 && !in[5] && in[6] >= 2)
      return (uint16_t)(in[18] | in[19] << 8);
  }
  return 0;
}

static const struct jm_product *product_of_model(uint16_t model) {
  const struct jm_product *p;
  for (size_t i = 0; model && (p = jm_product_at(i)); i++)
    if (p->model == model) return p;
  return NULL;
}

static int run_8bitdo(struct jm *jm, void *dev, struct jm_8bitdo_upgrade *op) {
  uint8_t report[64], reply[128];
  int need, n;
  enum jm_8bitdo_step step;
  while ((step = jm_8bitdo_upgrade_next(op, report, &need)) == JM_8BITDO_TRANSFER) {
    drain(jm, dev);
    if (!report_write(jm, dev, 0, report)) return -1;
    if (!need) {
      jm_8bitdo_upgrade_complete(op, 1, NULL, 0);
      continue;
    }
    enum jm_8bitdo_result r = JM_8BITDO_STALE;
    for (int stale = 0; r == JM_8BITDO_STALE && stale < 8; stale++) {
      n = jm->h->read(jm->h->ctx, dev, reply, sizeof reply, jm_8bitdo_upgrade_timeout(op));
      if (n <= 0) return -1;
      r = jm_8bitdo_upgrade_complete(op, 1, reply, (size_t)n);
    }
    if (r != JM_8BITDO_OK) return r;
    progress(jm, "writing and reading back", op->completed, op->sectors);
  }
  return step == JM_8BITDO_DONE ? 0 : op->error ? op->error : -1;
}

/* The loader's flash, read: count bytes from address. */
static int loader_read(struct jm *jm, void *dev, uint32_t address, uint8_t *out, size_t count) {
  uint8_t request[64], reply[128];
  const uint8_t *data;
  size_t n;
  while (count) {
    size_t want = count < JM_8BITDO_CHUNK ? count : JM_8BITDO_CHUNK;
    jm_8bitdo_request(request, 5, address, NULL, want);
    drain(jm, dev);
    if (!report_write(jm, dev, 0, request)) return 0;
    enum jm_8bitdo_result r = JM_8BITDO_STALE;
    for (int k = 0; r == JM_8BITDO_STALE && k < 8; k++) {
      int got = jm->h->read(jm->h->ctx, dev, reply, sizeof reply, 2000);
      if (got <= 0) return 0;
      r = jm_8bitdo_reply(reply, (size_t)got, 5, address, &data, &n);
    }
    if (r != JM_8BITDO_OK || n != want) return 0;
    memcpy(out, data, n);
    out += n, address += (uint32_t)n, count -= n;
  }
  return 1;
}

/* ---- Jieli ROM loaders (USB disks) --------------------------------------------------------- */

static int scsi(struct jm *jm, void *dev, const struct jm_jieli_request *r, uint8_t *reply, size_t *n) {
  if (r->delay_ms) jm->h->sleep(jm->h->ctx, r->delay_ms);
  uint8_t *data = r->direction == JM_SCSI_OUT ? (uint8_t *)r->data : reply;
  return !jm->h->scsi(jm->h->ctx, dev, r->cdb, 16, r->direction, data, r->size, r->timeout_ms, n);
}

/* INQUIRY: 1 a ROM loader, 2 Jieli firmware, 0 neither; its chip. */
static int inquiry(struct jm *jm, void *dev, enum jm_chip *chip) {
  uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0}, q[36];
  size_t n = 0;
  if (!jm->h->scsi || jm->h->scsi(jm->h->ctx, dev, cdb, 6, JM_SCSI_IN, q, sizeof q, 2000, &n) || n != 36)
    return 0;
  if (!memcmp(q + 8, "BR23", 4)) *chip = JM_CHIP_BR23;
  else if (!memcmp(q + 8, "BD19", 4)) *chip = JM_CHIP_BD19;
  else return 0;
  return memcmp(q + 16, "UBOOT1.00", 9) ? 2 : 1;
}

/* The RAM loader running: the device's flash size, transfer size and key. */
static int jieli_open(struct jm *jm, void *dev, enum jm_chip chip, const struct jm_firmware *fw,
                      struct jm_jieli_device *device) {
  const struct jm_firmware_file *blob = jm_firmware_find(fw, jm_chip_name(chip), "loader");
  struct jm_jieli_loader op;
  struct jm_jieli_request r;
  static uint8_t reply[32768];
  size_t n;
  enum jm_jieli_step step;
  if (!blob) {
    say(jm, "the package has no %s RAM loader (--package)", jm_chip_name(chip));
    return 0;
  }
  if (!jm_jieli_loader_init(&op, chip == JM_CHIP_BR23 ? JM_JIELI_BR23 : JM_JIELI_BD19, blob->data, blob->size))
    return 0;
  while ((step = jm_jieli_loader_next(&op, &r)) == JM_JIELI_TRANSFER) {
    n = 0;
    int ok = scsi(jm, dev, &r, reply, &n);
    jm_jieli_loader_complete(&op, ok, reply, n);
  }
  if (step != JM_JIELI_DONE) {
    say(jm, "its RAM loader did not start");
    return 0;
  }
  *device = op.device;
  return 1;
}

static int jieli_read(struct jm *jm, void *dev, const struct jm_jieli_device *d, uint32_t address,
                      uint8_t *out, size_t size) {
  while (size) {
    struct jm_jieli_request r;
    size_t n = 0;
    memset(&r, 0, sizeof r);
    r.direction = JM_SCSI_IN, r.timeout_ms = 20000, r.size = size < d->io ? size : d->io;
    if (!jm_jieli_read_cdb(r.cdb, 0xfd05, address, r.size) || !scsi(jm, dev, &r, out, &n) || n != r.size)
      return 0;
    size -= n, out += n, address += (uint32_t)n;
  }
  return 1;
}

/* The PID its flash's image header names, or "". */
static void jieli_installed(struct jm *jm, void *dev, const struct jm_jieli_device *d, char pid[17]) {
  size_t size = d->flash_size < 0x11000 ? d->flash_size : 0x11000;
  uint8_t *head = malloc(size), header[32];
  uint32_t base;
  pid[0] = 0;
  if (head && jieli_read(jm, dev, d, 0, head, size) && jm_jieli_find_base(head, size, header, &base))
    jm_jieli_name_string(pid, head + base + 16);
  free(head);
}

/* image written from 0, the flash after it erased to end (0: as far as the
 * image), the hole from..to (a unit's factory data; to 0: none) left alone;
 * read back. */
static int jieli_write(struct jm *jm, void *dev, const struct jm_jieli_device *d, const char *pid,
                       const uint8_t *image, size_t size, size_t end, uint32_t from, uint32_t to) {
  if (end < size) end = size;
  end = (end + 4095) & ~(size_t)4095;
  uint8_t *workspace = malloc(end);
  static uint8_t reply[32768];
  struct jm_jieli_flash op;
  struct jm_jieli_request r;
  enum jm_jieli_step step = JM_JIELI_FAILED;
  int ok = workspace && jm_jieli_flash_init(&op, d, pid, image, size, workspace, end, NULL, 0);
  /* The image's bytes over to the end: init takes it to the image's size. */
  if (ok && end > op.size) memset(workspace + op.size, 0xff, end - op.size), op.size = end;
  if (ok && to && !jm_jieli_flash_hole(&op, from, to)) ok = 0;
  if (!ok)
    say(jm, "the image does not fit: %zu bytes (to 0x%zx) for a flash of 0x%x, key 0x%04x, %u-byte transfers",
        size, end, (unsigned)d->flash_size, d->key, (unsigned)d->io);
  while (ok && (step = jm_jieli_flash_next(&op, &r)) == JM_JIELI_TRANSFER) {
    size_t n = 0;
    int good = scsi(jm, dev, &r, reply, &n);
    unsigned was = op.phase;
    size_t at = op.offset, sent = op.sent;
    if (!jm_jieli_flash_complete(&op, good, reply, n)) {
      static const char *const phase[] = {"keeping (reading)", "erasing", "writing", "reading back"};
      static const char *const why[] = {"", "the transfer failed", "the loader refused it", "a short reply", "it reads back different"};
      say(jm, "\n%s at 0x%zx (%04x, %zu bytes) failed: %s (%zu bytes back)", was < 4 ? phase[was] : "?",
          at, op.command, sent, op.error <= 0 && op.error >= -4 ? why[-op.error] : "?", n);
    }
    /* erasing, writing, reading back: a third each */
    progress(jm, "erasing, writing and reading back",
             op.phase >= 1 && op.phase <= 3 ? (op.phase - 1) * op.size + op.offset : op.phase == 4 ? 3 * op.size : 0,
             3 * op.size);
  }
  if (ok && step != JM_JIELI_DONE)
    say(jm, "\nstopped: phase %u at 0x%zx, error %d, pending %u, transfers of %u", op.phase, op.offset, op.error,
        op.pending, (unsigned)d->io);
  ok = ok && step == JM_JIELI_DONE;
  free(workspace);
  return ok;
}

/* The loader's "run the firmware" (FC0C, 1): the chip starts it and leaves
 * USB, usually before it answers, so a failed reply says nothing (the
 * Python flasher, padflash.py, took it the same way). */
static void jieli_start(struct jm *jm, void *dev) {
  uint8_t args[4] = {0, 0, 0, 1}, cdb[16], reply[16];
  size_t n;
  jm_jieli_cdb(cdb, 0xfc0c, args, sizeof args);
  jm->h->scsi(jm->h->ctx, dev, cdb, 16, JM_SCSI_IN, reply, sizeof reply, 2000, &n);
}

/* ---- finding devices ----------------------------------------------------------------------------- */

void jm_hid_descriptor(const uint8_t *d, size_t n, struct jm_device_info *info) {
  unsigned size = 0, count = 0, id = 0, bits[2][256] = {{0}};
  info->usage_page = 0, info->feature_size = info->output_size = 0;
  for (size_t i = 0; i < n;) {
    if (d[i] == 0xfe) {          /* a long item */
      i += 3 + (i + 1 < n ? d[i + 1] : 0);
      continue;
    }
    size_t k = (size_t[]){0, 1, 2, 4}[d[i] & 3];
    unsigned v = 0;
    for (size_t j = 0; j < k && i + 1 + j < n; j++) v |= (unsigned)d[i + 1 + j] << 8 * j;
    switch (d[i] & 0xfc) {
    case 0x04: if (!info->usage_page) info->usage_page = (uint16_t)v; break;
    case 0x74: size = v; break;
    case 0x94: count = v; break;
    case 0x84: id = v & 255; break;
    case 0xb0: bits[0][id] += size * count; break;
    case 0x90: bits[1][id] += size * count; break;
    }
    i += 1 + k;
  }
  for (int r = 0; r < 256; r++) {
    if (bits[0][r] / 8 > info->feature_size) info->feature_size = (uint16_t)(bits[0][r] / 8);
    if (bits[1][r] / 8 > info->output_size) info->output_size = (uint16_t)(bits[1][r] / 8);
  }
}

struct listing { struct jm_device_info d[64]; int n; };
static int add(void *user, const struct jm_device_info *info) {
  struct listing *l = user;
  if (l->n < 64) l->d[l->n++] = *info;
  return 0;
}

static int restarting(struct jm *jm, const struct jm_device_info *info) {
  for (unsigned i = 0; info->id[0] && i < jm->n_asked; i++)
    if (!strcmp(jm->asked[i], info->id)) return 1;
  return 0;
}

static const struct jm_stock *stock_of(const struct jm_device_info *d) {
  const struct jm_stock *s;
  for (size_t i = 0; d->kind == JM_DEVICE_HID && (s = jm_stock_at(i)); i++) {
    if (d->vid != s->vid || d->pid != s->product || (s->bcd && d->bcd != s->bcd) ||
        (s->manufacturer && strcmp(d->manufacturer, s->manufacturer)) ||
        (s->product_name && strcmp(d->product, s->product_name)))
      continue;
    if (s->model ? !d->output_size : s->usage_page && d->usage_page != s->usage_page)
      continue;
    return s;
  }
  return NULL;
}

/* What one device is (0: none of ours to know). */
static int identify_device(struct jm *jm, const struct jm_device_info *d, struct jm_found *f) {
  memset(f, 0, sizeof *f);
  f->info = *d;
  f->mode = "";
  const struct jm_stock *s = stock_of(d);
  if (d->kind == JM_DEVICE_HID && d->usage_page != 0xffc2 && !(d->vid == LOADER_VID && d->pid == LOADER_PID) && !s)
    return 0;
  if (d->kind == JM_DEVICE_HID && s && !s->model) {
    f->kind = JM_FOUND_STOCK, f->product = jm_product(s->pid), f->mode = s->mode;
    f->can_enter = s->usage_page != 0;
    return f->product != NULL;
  }
  void *dev = dev_open(jm, d);
  int ok = 0;
  if (!dev) return 0;
  if (d->kind == JM_DEVICE_DISK) {
    int k = inquiry(jm, dev, &f->chip);
    f->kind = k == 1 ? JM_FOUND_JIELI_LOADER : JM_FOUND_JIELI;
    ok = k != 0;
  } else if (d->usage_page == 0xffc2) {
    ok = identify(jm, dev, d->feature_size, &f->id);
    f->kind = f->id.updater ? JM_FOUND_UPDATER : JM_FOUND_OURS;
    f->product = jm_product(f->id.product);
    f->can_enter = 1;
  } else if (d->vid == LOADER_VID && d->pid == LOADER_PID) {
    f->kind = JM_FOUND_8BITDO_LOADER;
    ok = (f->product = product_of_model(loader_model(jm, dev))) != NULL;
  } else {
    f->kind = JM_FOUND_STOCK, f->can_enter = 1;
    ok = (f->product = product_of_model(stock_model(jm, dev))) != NULL;
  }
  if (f->product) f->chip = f->product->chip;
  dev_close(jm, dev);
  return ok;
}

/* Everything connected, one entry a device (stock pads show several
 * interfaces: the one taking the command). */
static int survey(struct jm *jm, struct jm_found *out, int max) {
  static struct listing l;
  int n = 0;
  l.n = 0;
  if (jm->h->list(jm->h->ctx, add, &l) < 0) return -1;
  for (int i = 0; i < l.n && n < max; i++) {
    if (restarting(jm, &l.d[i]) || !identify_device(jm, &l.d[i], &out[n])) continue;
    int dup = 0;
    for (int k = 0; k < n && !dup; k++)
      if (out[k].kind == JM_FOUND_STOCK && out[n].kind == JM_FOUND_STOCK && out[k].info.port[0] &&
          !strcmp(out[k].info.port, out[n].info.port)) {
        if (!out[k].can_enter && out[n].can_enter) out[k] = out[n];
        dup = 1;
      }
    n += !dup;
  }
  return n;
}

int jm_scan(struct jm *jm, jm_found_visit visit, void *user) {
  struct jm_found f[FOUND_MAX];
  int n = survey(jm, f, FOUND_MAX);
  for (int i = 0; i < n; i++)
    if (visit(user, &f[i])) break;
  return n;
}

static int chosen(const struct jm_found *f, const struct jm_select *sel) {
  if (sel->device && strcmp(sel->device, f->info.path) && (!f->info.port[0] || strcmp(sel->device, f->info.port)))
    return 0;
  return !sel->pid || (f->product ? !strcmp(f->product->pid, sel->pid) : f->kind == JM_FOUND_JIELI_LOADER);
}

const char *jm_found_what(const struct jm_found *f) {
  static const char *const names[] = {"our firmware", "our updater", "Jieli ROM loader", "Jieli firmware",
                                      "8BitDo boot loader", "stock firmware"};
  return names[f->kind];
}

static const char *where(const struct jm_found *f) {
  return f->info.port[0] ? f->info.port : f->info.path;
}

/* Devices for sel that fit (fits NULL: any), waiting up to sel->wait_s for
 * one; 0 with a message when there is none. */
static int find(struct jm *jm, const struct jm_select *sel, int (*fits)(const struct jm_found *), struct jm_found *out) {
  for (unsigned t = 0;; t++) {
    struct jm_found all[FOUND_MAX];
    int n = survey(jm, all, FOUND_MAX), k = 0;
    for (int i = 0; i < n; i++)
      if (chosen(&all[i], sel) && (!fits || fits(&all[i]))) out[k++] = all[i];
    if (k) return k;
    if (t >= sel->wait_s * 2) break;
    if (!t) say(jm, "waiting up to %u s for a device", sel->wait_s);
    jm->h->sleep(jm->h->ctx, 500);
  }
  say(jm, "no device for that%s (jollymaam scan lists what is connected)", sel->device ? " there" : "");
  return 0;
}

static int is_loader(const struct jm_found *f) {
  return f->kind == JM_FOUND_JIELI_LOADER || f->kind == JM_FOUND_8BITDO_LOADER;
}

/* f restarted into its vendor loader (if not in it), which is waited for on
 * its port: into f.  0 with a message on failure. */
static int enter(struct jm *jm, struct jm_found *f, unsigned wait_s) {
  if (is_loader(f)) return 1;
  const struct jm_product *p = f->product;
  if (!f->can_enter) {
    say(jm, "%s: %s%s%s has no command for its loader%s%s", where(f), jm_found_what(f), f->mode[0] ? ", " : "", f->mode,
        p && p->by_hand ? ": " : "", p && p->by_hand ? p->by_hand : "");
    return 0;
  }
  say(jm, "%s: %s, %s: restarting into its loader", where(f), jm_found_what(f), p ? p->pid : "?");
  void *dev = dev_open(jm, &f->info);
  int sent = 0;
  if (dev && (f->kind == JM_FOUND_OURS || f->kind == JM_FOUND_UPDATER)) {
    uint8_t none[1];
    struct jm_update_request r;
    memset(&r, 0, sizeof r);
    jm_command(r.cdb, JM_CMD_REBOOT_LOADER, 0);
    sent = tool(jm, dev, f->info.feature_size, &r, none);
  } else if (dev) {
    const struct jm_stock *s = stock_of(&f->info);
    uint8_t report[65] = {0};
    memcpy(report, s->report, sizeof s->report);
    jm->h->write(jm->h->ctx, dev, report, s->len);
    sent = 1;                    /* the restart may beat the write's completion */
  }
  dev_close(jm, dev);
  if (!sent) return 0;
  if (jm->n_asked < 16) snprintf(jm->asked[jm->n_asked++], sizeof jm->asked[0], "%s", f->info.id);
  if (!wait_s) return 1;
  struct jm_select there = {f->info.port[0] ? f->info.port : NULL, NULL, wait_s, 0};
  for (unsigned t = 0; t < wait_s * 2; t++) {
    struct jm_found all[FOUND_MAX];
    int n = survey(jm, all, FOUND_MAX);
    for (int i = 0; i < n; i++)
      if (is_loader(&all[i]) && chosen(&all[i], &there) &&
          (all[i].kind == JM_FOUND_JIELI_LOADER ? all[i].chip == f->chip : all[i].product == p)) {
        *f = all[i];
        if (!f->product) f->product = p;
        return 1;
      }
    jm->h->sleep(jm->h->ctx, 500);
  }
  say(jm, "%s: its loader did not come up%s%s", where(f), p && p->by_hand ? "; by hand: " : "",
      p && p->by_hand ? p->by_hand : "");
  return 0;
}

/* ---- a pad, step by step --------------------------------------------------------------------- */

static void helper_get(const struct jm_pad *p, struct jm_jieli_device *d) {
  d->flash_id = p->helper.flash_id, d->flash_size = p->helper.flash_size, d->io = p->helper.io, d->key = p->helper.key;
}

void jm_pad_init(struct jm_pad *pad, struct jm *jm, const struct jm_firmware *fw, const struct jm_found *found) {
  memset(pad, 0, sizeof *pad);
  pad->jm = jm, pad->fw = fw, pad->found = *found;
}

/* In its vendor loader, readied: a Jieli ROM loader's RAM loader runs (the
 * one upload of a session) and the product its flash names is read; an
 * 8BitDo loader's product is its model's. */
int jm_pad_ready(struct jm_pad *pad) {
  struct jm *jm = pad->jm;
  struct jm_found *f = &pad->found;
  if (pad->ready) return 1;
  if (pad->gone) {
    say(jm, "%s: it restarted since: scan again", where(f));
    return 0;
  }
  if (f->kind == JM_FOUND_JIELI) {
    say(jm, "%s: Jieli firmware we do not know: no way into its loader from here", where(f));
    return 0;
  }
  if (!enter(jm, f, 30)) return 0;
  if (f->kind == JM_FOUND_JIELI_LOADER) {
    struct jm_jieli_device d;
    void *dev = dev_open(jm, &f->info);
    int ok = dev && jieli_open(jm, dev, f->chip, pad->fw, &d);
    if (ok) {
      jieli_installed(jm, dev, &d, pad->product);
      pad->helper.flash_id = d.flash_id, pad->helper.flash_size = d.flash_size;
      pad->helper.io = d.io, pad->helper.key = d.key;
    }
    dev_close(jm, dev);
    if (!ok) return 0;
    /* A blank flash names nothing: the product it ran before, if known. */
    if (!pad->product[0] && f->product) snprintf(pad->product, sizeof pad->product, "%s", f->product->pid);
    if (pad->product[0]) f->product = jm_product(pad->product);
  } else if (f->product) {
    snprintf(pad->product, sizeof pad->product, "%s", f->product->pid);
  }
  say(jm, "%s: %s, %s", where(f), jm_found_what(f), pad->product[0] ? pad->product : "its flash names no product");
  pad->ready = 1;
  return 1;
}

/* Its restart: what it is now is not known until a scan finds it. */
static void pad_gone(struct jm_pad *pad) {
  pad->ready = 0;
  pad->gone = 1;
}

/* The whole flash, a block at a time to sink. */
int jm_pad_backup(struct jm_pad *pad, jm_sink sink, void *user) {
  struct jm *jm = pad->jm;
  struct jm_found *f = &pad->found;
  static uint8_t block[32768];
  if (!jm_pad_ready(pad)) return 0;
  void *dev = dev_open(jm, &f->info);
  int ok = dev != NULL;
  if (ok && f->kind == JM_FOUND_JIELI_LOADER) {
    struct jm_jieli_device d;
    helper_get(pad, &d);
    for (uint32_t at = 0; ok && at < d.flash_size; at += sizeof block) {
      ok = jieli_read(jm, dev, &d, at, block, sizeof block) && sink(user, at, block, sizeof block);
      progress(jm, "reading", at + sizeof block, d.flash_size);
    }
  } else if (ok) {
    /* Telink: the app and its data (the loader below keeps itself); Yichip:
     * the version page, the app slot and the data pages. */
    int yichip = f->product->chip == JM_CHIP_YC3121;
    uint32_t from = yichip ? 0x01017000 : JM_8BITDO_TELINK_START, to = yichip ? 0x01080000 : 0x80000;
    for (uint32_t at = from; ok && at < to; at += 4096) {
      ok = loader_read(jm, dev, at, block, 4096) && sink(user, at, block, 4096);
      progress(jm, "reading", at + 4096 - from, to - from);
    }
  }
  dev_close(jm, dev);
  say(jm, "%s: %s", where(f), ok ? "read whole" : "backup failed");
  return ok;
}

/* The firmware's install image for the product it holds, written, read
 * back and started (it leaves USB). */
int jm_pad_install(struct jm_pad *pad) {
  struct jm *jm = pad->jm;
  struct jm_found *f = &pad->found;
  if (!jm_pad_ready(pad)) return 0;
  const struct jm_product *p = pad->product[0] ? jm_product(pad->product) : NULL;
  const struct jm_firmware_file *img = p ? jm_firmware_find(pad->fw, p->pid, "install") : NULL;
  if (!pad->product[0]) {
    say(jm, "%s: its flash names no product (blank?)", where(f));
    return 0;
  }
  if (!img) {
    say(jm, "%s: no image for %s in the firmware", where(f), pad->product);
    return 0;
  }
  void *dev = dev_open(jm, &f->info);
  int ok = 0;
  if (!dev) return 0;
  if (f->kind == JM_FOUND_JIELI_LOADER) {
    struct jm_jieli_device d;
    struct jm_jieli_image view;
    uint8_t *work = malloc(img->size), *packed = malloc(img->size);
    helper_get(pad, &d);
    if (!work || !packed || jm_jieli_image_open(&view, img->data, img->size, work, img->size) ||
        jm_jieli_image_pack(&view, d.key, packed, img->size))
      say(jm, "%s: the image for %s is not a Jieli image", where(f), p->pid);
    else if (p->factory_size && p->factory_offset + p->factory_size <= d.flash_size) {
      say(jm, "%s: %s, writing it (key 0x%04x), the rest erased but the factory data (0x%x..0x%x)", where(f),
          p->pid, d.key, p->factory_offset, p->factory_offset + p->factory_size);
      ok = jieli_write(jm, dev, &d, p->pid, packed, img->size, d.flash_size, p->factory_offset,
                       p->factory_offset + p->factory_size);
    } else {
      say(jm, "%s: %s, writing it (key 0x%04x)", where(f), p->pid, d.key);
      ok = jieli_write(jm, dev, &d, p->pid, packed, img->size, 0, 0, 0);
    }
    if (ok) jieli_start(jm, dev);
    free(work), free(packed);
  } else {
    struct jm_8bitdo_upgrade op;
    if (p->chip == JM_CHIP_YC3121 ? !jm_8bitdo_yichip_init(&op, img->data, img->size)
                                  : !jm_8bitdo_upgrade_init(&op, img->data, img->size))
      say(jm, "%s: the image for %s does not fit its loader", where(f), p->pid);
    else {
      say(jm, "%s: %s, writing it", where(f), p->pid);
      int r = run_8bitdo(jm, dev, &op);
      ok = !r;
      if (r)
        say(jm, "%s: stopped in phase %u at 0x%08x+0x%x (%d)", where(f), op.phase,
            p->chip == JM_CHIP_YC3121 ? JM_8BITDO_YICHIP_BASE : 0, op.offset, r);
    }
  }
  dev_close(jm, dev);
  say(jm, "%s: %s", where(f), ok ? "written, read back and started" : "failed; it stays in its loader");
  if (ok) pad_gone(pad);
  return ok;
}

/* Out of its vendor loader: the firmware it holds starts (it leaves USB). */
int jm_pad_reboot(struct jm_pad *pad) {
  struct jm *jm = pad->jm;
  struct jm_found *f = &pad->found;
  uint8_t request[64];
  int ok = 0;
  if (pad->gone) {
    say(jm, "%s: it restarted since: scan again", where(f));
    return 0;
  }
  if (f->kind == JM_FOUND_OURS || f->kind == JM_FOUND_UPDATER) {
    void *dev = dev_open(jm, &f->info);
    uint8_t none[1];
    struct jm_update_request r;
    memset(&r, 0, sizeof r);
    jm_command(r.cdb, JM_CMD_REBOOT, 0);
    ok = dev && tool(jm, dev, f->info.feature_size, &r, none);
    dev_close(jm, dev);
    say(jm, "%s: %s", where(f), ok ? "restarting" : "failed");
    if (ok) pad_gone(pad);
    return ok;
  }
  if (!is_loader(f)) {
    say(jm, "%s: %s has no restart command", where(f), jm_found_what(f));
    return 0;
  }
  /* The Jieli start is the RAM loader's command. */
  if (f->kind == JM_FOUND_JIELI_LOADER && !jm_pad_ready(pad)) return 0;
  void *dev = dev_open(jm, &f->info);
  if (dev && f->kind == JM_FOUND_JIELI_LOADER) jieli_start(jm, dev), ok = 1;
  else if (dev) ok = !jm_8bitdo_request(request, 7, 0, NULL, 0) && report_write(jm, dev, 0, request);
  dev_close(jm, dev);
  say(jm, "%s: %s", where(f), ok ? "started its firmware" : "failed");
  if (ok) pad_gone(pad);
  return ok;
}

/* ---- operations ---------------------------------------------------------------------------------- */

static int vendor(const struct jm_found *f) {
  return f->kind != JM_FOUND_JIELI;
}

static int each(struct jm *jm, const struct jm_select *sel, int (*fits)(const struct jm_found *),
                int (*one)(struct jm *, const struct jm_firmware *, struct jm_found *, unsigned),
                const struct jm_firmware *fw) {
  struct jm_found f[FOUND_MAX];
  int n = find(jm, sel, fits, f), failed = 0;
  for (int i = 0; i < n; i++)
    failed += !one(jm, fw, &f[i], sel->ask_only ? 0 : sel->wait_s > 30 ? sel->wait_s : 30);
  return n && !failed ? 0 : -1;
}

static int oemflash(struct jm *jm, const struct jm_firmware *fw, struct jm_found *f, unsigned wait_s) {
  struct jm_pad pad;
  (void)wait_s;
  jm_pad_init(&pad, jm, fw, f);
  return jm_pad_install(&pad);
}

int jm_oemflash(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel) {
  return each(jm, sel, vendor, oemflash, fw);
}

static int loader(struct jm *jm, const struct jm_firmware *fw, struct jm_found *f, unsigned wait_s) {
  (void)fw;
  if (!enter(jm, f, wait_s)) return 0;
  say(jm, "%s: %s, %s", where(f), jm_found_what(f), f->product ? f->product->pid : jm_chip_name(f->chip));
  return 1;
}

int jm_loader(struct jm *jm, const struct jm_select *sel) {
  return each(jm, sel, vendor, loader, NULL);
}

static int start(struct jm *jm, const struct jm_firmware *fw, struct jm_found *f, unsigned wait_s) {
  struct jm_pad pad;
  (void)wait_s;
  jm_pad_init(&pad, jm, fw, f);
  return jm_pad_reboot(&pad);
}

int jm_start(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel) {
  return each(jm, sel, is_loader, start, fw);
}

/* ---- our updater ------------------------------------------------------------------------------------ */

static int ours(const struct jm_found *f) {
  return f->kind == JM_FOUND_OURS || f->kind == JM_FOUND_UPDATER;
}

/* Our firmware into its updater, update.bin through it, then its restart. */
static int update(struct jm *jm, const struct jm_firmware *fw, struct jm_found *f, unsigned wait_s) {
  const struct jm_firmware_file *img = f->product ? jm_firmware_find(fw, f->product->pid, "update") : NULL;
  if (!img || !jm_update_image_check(img->data, img->size, f->product->pid)) {
    say(jm, "%s: no update for %s in the firmware", where(f), f->id.product);
    return 0;
  }
  if (f->kind == JM_FOUND_OURS) {
    uint8_t uid[sizeof f->id.uid];
    memcpy(uid, f->id.uid, sizeof uid);
    void *dev = dev_open(jm, &f->info);
    uint8_t none[1];
    struct jm_update_request r;
    memset(&r, 0, sizeof r);
    jm_command(r.cdb, JM_CMD_UPDATER, 0);
    say(jm, "%s: %s %s, restarting into its updater", where(f), f->id.product, f->id.build);
    int sent = dev && tool(jm, dev, f->info.feature_size, &r, none);
    dev_close(jm, dev);
    if (!sent) return 0;
    if (jm->n_asked < 16) snprintf(jm->asked[jm->n_asked++], sizeof jm->asked[0], "%s", f->info.id);
    int found = 0;
    for (unsigned t = 0; t < wait_s * 2 && !found; t++) {
      struct jm_found all[FOUND_MAX];
      int n = survey(jm, all, FOUND_MAX);
      jm->h->sleep(jm->h->ctx, 500);
      for (int i = 0; i < n && !found; i++)
        if (all[i].kind == JM_FOUND_UPDATER && !memcmp(all[i].id.uid, uid, sizeof uid))
          *f = all[i], found = 1;
    }
    if (!found) {
      say(jm, "%s: its updater did not come up", where(f));
      return 0;
    }
  }
  void *dev = dev_open(jm, &f->info);
  struct jm_update op;
  struct jm_update_request r;
  uint8_t reply[512];
  enum jm_update_step step = JM_UPDATE_FAILED;
  if (dev && jm_update_init(&op, img->data, img->size, tool_room(f->info.feature_size))) {
    say(jm, "%s: %s, writing update.bin", where(f), f->product->pid);
    while ((step = jm_update_next(&op, &r)) == JM_UPDATE_TRANSFER) {
      jm_update_complete(&op, tool(jm, dev, f->info.feature_size, &r, reply), reply, r.in_size);
      progress(jm, "writing", op.offset, op.size);
    }
  }
  dev_close(jm, dev);
  say(jm, "%s: %s", where(f), step == JM_UPDATE_DONE ? "updated and restarted" : "failed; it stays in its updater");
  return step == JM_UPDATE_DONE;
}

int jm_update(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel) {
  return each(jm, sel, ours, update, fw);
}

/* ---- backup and restore --------------------------------------------------------------------------- */

static int one_vendor(struct jm *jm, const struct jm_select *sel, struct jm_found *f) {
  struct jm_found all[FOUND_MAX];
  int n = find(jm, sel, vendor, all);
  if (n > 1) say(jm, "more than one device: pick one with --device PORT");
  if (n != 1) return 0;
  *f = all[0];
  return 1;
}

int jm_backup(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel, jm_sink sink, void *user) {
  struct jm_found f;
  struct jm_pad pad;
  if (!one_vendor(jm, sel, &f)) return -1;
  jm_pad_init(&pad, jm, fw, &f);
  return jm_pad_backup(&pad, sink, user) ? 0 : -1;
}

/* A Jieli image onto the pad in its readied loader (see jm_pad_restore). */
static int restore_jieli(struct jm_pad *pad, const uint8_t *image, size_t size, int raw_write) {
  struct jm *jm = pad->jm;
  struct jm_found *f = &pad->found;
  const struct jm_product *p = pad->product[0] ? jm_product(pad->product) : NULL;
  const uint8_t *raw;
  size_t raw_size;
  struct jm_jieli_device d;
  struct jm_jieli_image view;
  char wanted[17];
  int ok = 0;
  helper_get(pad, &d);
  enum jm_jieli_ufw_result u = jm_jieli_ufw_open(image, size, &raw, &raw_size);
  uint8_t *work = malloc(size ? size : 1);
  if ((u != JM_JIELI_UFW_RAW && u != JM_JIELI_UFW_OK) || !work ||
      jm_jieli_image_open(&view, raw, raw_size, work, size)) {
    say(jm, "not a Jieli image, nor a backup of one");
    free(work);
    return 0;
  }
  free(work);
  jm_jieli_name_string(wanted, raw + view.base + 16);
  uint32_t from = p ? p->factory_offset : 0, to = p ? p->factory_offset + p->factory_size : 0;
  int whole = raw_size == d.flash_size;
  if (!p || strcmp(pad->product, wanted)) {
    say(jm, "the file is for %s, the pad holds %s", wanted, pad->product[0] ? pad->product : "nothing");
    return 0;
  }
  if (view.key != d.key) {
    say(jm, "the file is for chip key 0x%04x, the pad's is 0x%04x", view.key, d.key);
    return 0;
  }
  if (raw_write && !whole) {
    say(jm, "a raw write takes a whole-flash backup (0x%x bytes), not 0x%zx", (unsigned)d.flash_size, raw_size);
    return 0;
  }
  if (!raw_write && (!p->factory_size || to > d.flash_size)) {
    say(jm, "where %s keeps its factory data is not established: not restoring (a raw write could)", p->pid);
    return 0;
  }
  if (!raw_write && !whole && raw_size > from) {
    say(jm, "the image (0x%zx bytes) runs into the factory data at 0x%x: not restoring", raw_size, from);
    return 0;
  }
  void *dev = dev_open(jm, &f->info);
  if (!dev) return 0;
  if (raw_write) {
    say(jm, "%s: writing the whole flash as the file holds it, factory data included", where(f));
    ok = jieli_write(jm, dev, &d, p->pid, raw, raw_size, 0, 0, 0);
  } else if (whole) {
    uint8_t *mine = malloc(p->factory_size);
    int same = mine && jieli_read(jm, dev, &d, from, mine, p->factory_size) &&
               !memcmp(mine, raw + from, p->factory_size);
    free(mine);
    if (!same)
      say(jm, "%s: the backup is not of this pad (its factory data at 0x%x differs): not restoring", where(f), from);
    else {
      say(jm, "%s: a backup of this pad: writing all but its factory data (0x%x..0x%x)", where(f), from, to);
      ok = jieli_write(jm, dev, &d, p->pid, raw, raw_size, 0, from, to);
    }
  } else {
    say(jm, "%s: writing the image (0x%zx bytes), the rest erased but the factory data (0x%x..0x%x)", where(f),
        raw_size, from, to);
    ok = jieli_write(jm, dev, &d, p->pid, raw, raw_size, d.flash_size, from, to);
  }
  if (ok) jieli_start(jm, dev);
  dev_close(jm, dev);
  return ok;
}

/* A restore image (see jollymaam.h), written, read back and started. */
int jm_pad_restore(struct jm_pad *pad, const uint8_t *image, size_t size, int raw) {
  struct jm *jm = pad->jm;
  struct jm_found *f = &pad->found;
  int ok = 0;
  if (!jm_pad_ready(pad)) return 0;
  if (f->kind == JM_FOUND_JIELI_LOADER) {
    ok = restore_jieli(pad, image, size, raw);
  } else if (raw) {
    say(jm, "a raw write of %s's flash is not established", f->product->pid);
  } else if (f->product->chip == JM_CHIP_TLSR8208) {
    struct jm_8bitdo_upgrade op;
    void *dev;
    if (!jm_8bitdo_restore_init(&op, image, size, f->product->model)) say(jm, "not %s's stock app", f->product->pid);
    else if ((dev = dev_open(jm, &f->info))) {
      ok = !run_8bitdo(jm, dev, &op);
      dev_close(jm, dev);
    }
  } else if (f->product->chip == JM_CHIP_YC3121) {
    struct jm_8bitdo_upgrade op;
    const uint8_t *app;
    size_t app_size;
    void *dev;
    if (!jm_8bitdo_yichip_stock_app(image, size, &app, &app_size) || !jm_8bitdo_yichip_init(&op, app, app_size))
      say(jm, "not %s's stock app v1.01 (the app alone, or a loader dump from 0x01017000)", f->product->pid);
    else if ((dev = dev_open(jm, &f->info))) {
      ok = !run_8bitdo(jm, dev, &op);
      dev_close(jm, dev);
    }
  } else {
    say(jm, "restoring %s is not established", f->product ? f->product->pid : "?");
  }
  say(jm, "%s: %s", where(f), ok ? "restored, read back and started" : "not restored; it stays in its loader");
  if (ok) pad_gone(pad);
  return ok;
}
