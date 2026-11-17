#include "upgrade_jieli.h"
#include "jieli_image/image.h"
#include <string.h>

enum { PRESERVE, ERASE, WRITE, VERIFY, DONE, FAILED };
static void be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
/* Prepare a flash operation after RAM-loader startup and product probing.
 * Validate the raw image in workspace before overwriting it with the raw bytes.
 * Device product must come from a verified device query, not USB PID alone.
 * The image key must match device.key: rekey the image separately if needed.
 * Workspace covers the sector-rounded maximum of image size and keep ranges.
 * Keep ranges are caller-selected board policy (for example Nova stock data);
 * they are read before any erase and can override image bytes. Do not preserve
 * firmware headers or code. Workspace must not overlap source or keep records.
 * Keep records are borrowed through completion. No allocation or device I/O. */
int jm_jieli_flash_init(struct jm_jieli_flash *op, const struct jm_jieli_device *device,
                          const char *device_product, const uint8_t *image, size_t size,
                          uint8_t *workspace, size_t capacity,
                          const struct jm_jieli_keep *keep, size_t keep_count) {
  if (!device || !device_product || !*device_product || !image || !workspace ||
      image == workspace || !size || size > device->flash_size || keep_count > 16 ||
      (keep_count && !keep) || device->io < 64 || device->io > 0x8000 ||
      (device->flash_size & 4095)) return 0;
  uint64_t end = size;
  for (size_t i = 0; i < keep_count; ++i) {
    uint64_t top = (uint64_t)keep[i].offset + keep[i].size;
    if (!keep[i].size || top > device->flash_size) return 0;
    if (top > end) end = top;
  }
  end = (end + 4095) & ~(uint64_t)4095;
  if (end > capacity || end > device->flash_size) return 0;
  struct jm_jieli_image view;
  if (jm_jieli_image_open(&view, image, size, workspace, capacity) != JM_JIELI_IMAGE_OK ||
      view.key != device->key) return 0;
  char product[17];
  jm_jieli_name_string(product, image + view.base + 16);
  if (strcmp(product, device_product)) return 0;
  memset(op, 0, sizeof *op);
  memcpy(workspace, image, size);
  memset(workspace + size, 0xff, (size_t)end - size);
  op->buffer = workspace; op->size = (size_t)end; op->device = *device;
  op->keep = keep; op->keep_count = keep_count;
  op->phase = keep_count ? PRESERVE : ERASE;
  return 1;
}
int jm_jieli_flash_hole(struct jm_jieli_flash *op, uint32_t from, uint32_t to) {
  if (!op || op->phase > ERASE || (from & 4095) || (to & 4095) || from >= to) return 0;
  op->hole_from = from; op->hole_to = to;
  return 1;
}
static int in_hole(const struct jm_jieli_flash *op, size_t at, size_t n) {
  return op->hole_to && at < op->hole_to && at + n > op->hole_from;
}
/* A transfer of up to n bytes at the offset, past the hole and stopping at
 * its start; 0 when the phase is done. */
static size_t span(struct jm_jieli_flash *op, size_t n) {
  if (op->hole_to && op->offset >= op->hole_from && op->offset < op->hole_to) op->offset = op->hole_to;
  if (op->offset >= op->size) return 0;
  if (n > op->size - op->offset) n = op->size - op->offset;
  if (op->hole_to && op->offset < op->hole_from && op->offset + n > op->hole_from) n = op->hole_from - op->offset;
  return n;
}
/* Yield one SCSI request. The caller must check SCSI status and actual lengths
 * before completing it. Preservation reads finish before the first erase.
 * Erase uses aligned 64 KiB blocks where possible, otherwise 4 KiB sectors.
 * Blank write chunks are skipped; readback still verifies them. No retry occurs
 * after a transport failure. DONE means verified, with the device left in its
 * RAM loader; starting firmware is a separate explicit command (FC0C, arg 1). */
enum jm_jieli_step jm_jieli_flash_next(struct jm_jieli_flash *op,
                                        struct jm_jieli_request *request) {
  if (op->phase == FAILED || op->pending) return JM_JIELI_FAILED;
  if (op->phase == DONE) return JM_JIELI_DONE;
  memset(request, 0, sizeof *request); request->timeout_ms = 20000;
  request->direction = JM_SCSI_IN;
  /* Each phase from the start, past the hole; WRITE skips blank chunks. */
  while (op->phase >= ERASE && op->phase <= VERIFY) {
    size_t n = span(op, op->phase == ERASE ? 0x1000 : op->device.io), i = 0;
    if (!n) {
      op->offset = 0;
      if (++op->phase == DONE) return JM_JIELI_DONE;
      continue;
    }
    if (op->phase != WRITE) break;
    while (i < n && op->buffer[op->offset + i] == 0xff) ++i;
    if (i != n) { op->sent = n; break; }
    op->offset += n;
  }
  uint32_t at = (uint32_t)op->offset;
  if (op->phase == PRESERVE) {
    const struct jm_jieli_keep *range = &op->keep[op->keep_index];
    at = range->offset + (uint32_t)op->keep_offset;
    op->sent = range->size - op->keep_offset;
    if (op->sent > op->device.io) op->sent = op->device.io;
    op->command = 0xfd05;
    jm_jieli_read_cdb(request->cdb, op->command, at, op->sent);
    request->size = op->sent;
  } else if (op->phase == ERASE) {
    op->sent = !(at & 0xffff) && op->size - at >= 0x10000 && !in_hole(op, at, 0x10000) ? 0x10000 : 0x1000;
    op->command = op->sent == 0x10000 ? 0xfb00 : 0xfb01;
    uint8_t args[4]; be32(args, at);
    jm_jieli_cdb(request->cdb, op->command, args, 4); request->size = 16;
  } else if (op->phase == WRITE) {
    op->command = 0xfb04; request->direction = JM_SCSI_OUT;
    request->data = op->buffer + op->offset; request->size = op->sent;
    jm_jieli_write_cdb(request->cdb, op->command, at, request->data, op->sent);
  } else if (op->phase == VERIFY) {
    op->sent = span(op, op->device.io);
    op->command = 0xfd05;
    jm_jieli_read_cdb(request->cdb, op->command, at, op->sent); request->size = op->sent;
  } else { op->phase = FAILED; return JM_JIELI_FAILED; }
  op->pending = 1;
  return JM_JIELI_TRANSFER;
}
/* For OUT transfers size is the actual transferred count; data is ignored.
 * For IN transfers provide the actual response. Errors stop immediately and
 * leave the device in its loader for recovery. Stop scheduling to cancel; once
 * ERASE starts, cancellation can leave firmware incomplete. */
int jm_jieli_flash_complete(struct jm_jieli_flash *op, int transport_ok,
                              const uint8_t *data, size_t size) {
  if (!op->pending) return 0;
  op->pending = 0;
  if (!transport_ok) { op->error = -1; goto fail; }
  if (op->phase == ERASE) {
    if (!data || !jm_jieli_exec_reply(data, size, op->command, NULL)) { op->error = -2; goto fail; }
  } else if (size != op->sent || (op->phase != WRITE && !data)) { op->error = -3; goto fail; }
  if (op->phase == PRESERVE) {
    const struct jm_jieli_keep *range = &op->keep[op->keep_index];
    memcpy(op->buffer + range->offset + op->keep_offset, data, size);
    op->keep_offset += size;
    if (op->keep_offset == range->size) {
      op->keep_offset = 0;
      if (++op->keep_index == op->keep_count) op->phase = ERASE;
    }
    return 1;
  }
  if (op->phase == VERIFY && memcmp(data, op->buffer + op->offset, size)) { op->error = -4; goto fail; }
  op->offset += op->sent;   /* the phase moves on in next, past the hole */
  return 1;
fail:
  op->phase = FAILED; return 0;
}
