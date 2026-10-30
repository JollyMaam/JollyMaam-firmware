#include "upgrade_8bitdo.h"
#include "proto_jollymaam.h"
#include <string.h>

enum { IDENTIFY, COMPARE, ERASE, CHECK_ERASE, PROGRAM, VERIFY, START, DONE, FAILED };
static uint32_t address(const struct jm_8bitdo_upgrade *op) {
  return 0x6000 + op->sector * 4096 + op->offset;
}
static unsigned chunk(const struct jm_8bitdo_upgrade *op) {
  unsigned left = 4096 - op->offset;
  return left < 46 ? left : 46;
}
static uint8_t image_byte(const struct jm_8bitdo_upgrade *op, unsigned i) {
  size_t off = (size_t)op->sector * 4096 + op->offset + i;
  return off < op->size ? op->image[off] : 0xff;
}
static void advance_sector(struct jm_8bitdo_upgrade *op) {
  ++op->completed;
  op->offset = 0;
  op->different = 0;
  if (op->completed == op->sectors) {
    op->phase = START;
  } else {
    op->sector = op->sector + 1 < op->sectors ? op->sector + 1 : 0;
    op->phase = COMPARE;
  }
}
/* Validates full pad image and bounds before producing any device requests.
 * Receiver images are rejected. No device changes occur until model checked. */
int jm_8bitdo_upgrade_init(struct jm_8bitdo_upgrade *op,
                           const uint8_t *image, size_t size) {
  char product[17];
  if (!size || size > 0x6f000 - 0x6000 ||
      !jm_8bitdo_image_product(image, size, product) || strcmp(product, "U2C_WIRELESS"))
    return 0;
  /* A product string alone is insufficient: validate the application's MAIN
   * header and payload before scheduling any erases. */
  size_t main_at;
  for (main_at = 4096; main_at + 0x120 <= size; main_at += 4096)
    if (!memcmp(image + main_at, "MAIN", 4)) break;
  if (main_at + 0x120 > size ||
      !jm_update_image_check(image + main_at, size - main_at, "U2C_WIRELESS"))
    return 0;
  memset(op, 0, sizeof *op);
  op->image = image;
  op->size = size;
  op->sectors = (uint32_t)((size + 4095) / 4096);
  op->sector = op->sectors > 1 ? 1 : 0;
  op->phase = IDENTIFY;
  return 1;
}
/* Produce the next unnumbered 64-byte HID report. reply_required is false
 * only for application start; complete that transfer with NULL,0 on success.
 * Call complete exactly once per request, except STALE replies may be retried.
 * Stop scheduling to cancel. After any erase the device may need recovery.
 * completed counts sectors verified/skipped; sectors includes header sector. */
enum jm_8bitdo_step jm_8bitdo_upgrade_next(struct jm_8bitdo_upgrade *op,
                                          uint8_t report[64], int *reply_required) {
  if (op->phase == FAILED || op->pending)
    return JM_8BITDO_FAILED;
  if (op->phase == DONE)
    return JM_8BITDO_DONE;
  uint16_t command;
  uint32_t at = address(op);
  size_t n = 0;
  uint8_t data[46];
  const uint8_t *payload = NULL;
  switch (op->phase) {
  case IDENTIFY: command = 8; at = 0; n = 4; break;
  case COMPARE: case CHECK_ERASE: case VERIFY: command = 5; n = chunk(op); break;
  case ERASE: command = 4; break;
  case PROGRAM:
    command = 3; n = chunk(op); payload = data;
    for (unsigned i = 0; i < n; ++i) data[i] = image_byte(op, i);
    break;
  case START: command = 7; at = 0; break;
  default: op->phase = FAILED; return JM_8BITDO_FAILED;
  }
  if (jm_8bitdo_request(report, command, at, payload, n) != JM_8BITDO_OK) {
    op->phase = FAILED;
    return JM_8BITDO_FAILED;
  }
  op->pending = 1;
  *reply_required = op->phase != START;
  return JM_8BITDO_TRANSFER;
}
/* transport_ok false ends the operation without retrying destructive commands.
 * Returns STALE to keep waiting for the outstanding read, OK otherwise.
 * Malformed/status replies end the operation. Inspect next for final status. */
enum jm_8bitdo_result jm_8bitdo_upgrade_complete(struct jm_8bitdo_upgrade *op,
                                                int transport_ok,
                                                const uint8_t *reply, size_t size) {
  if (!op->pending)
    return JM_8BITDO_ARGUMENT;
  if (!transport_ok) {
    op->pending = 0; op->phase = FAILED; op->error = -1;
    return JM_8BITDO_ARGUMENT;
  }
  if (op->phase == START) {
    op->pending = 0; op->phase = DONE;
    return JM_8BITDO_OK;
  }
  uint16_t command = op->phase == IDENTIFY ? 8 : op->phase == ERASE ? 4 : op->phase == PROGRAM ? 3 : 5;
  const uint8_t *data;
  size_t n;
  enum jm_8bitdo_result result = jm_8bitdo_reply(reply, size, command,
                                                address(op), &data, &n);
  if (result == JM_8BITDO_STALE)
    return result;
  op->pending = 0;
  if (result != JM_8BITDO_OK) {
    op->phase = FAILED; op->error = result;
    return result;
  }
  if (op->phase == IDENTIFY) {
    if (n != 2 || data[0] != 0x1b || data[1] != 0x30)
      goto invalid;
    op->phase = COMPARE;
    return JM_8BITDO_OK;
  }
  if (op->phase == ERASE) {
    op->phase = CHECK_ERASE;
    return JM_8BITDO_OK;
  }
  unsigned count = chunk(op);
  if (op->phase != PROGRAM) {
    if (n != count)
      goto invalid;
    for (unsigned i = 0; i < count; ++i) {
      uint8_t expected = op->phase == CHECK_ERASE ? 0xff : image_byte(op, i);
      if (data[i] != expected) {
        if (op->phase == COMPARE) op->different = 1;
        else goto invalid;
      }
    }
  }
  op->offset += count;
  if (op->offset == 4096) {
    op->offset = 0;
    switch (op->phase) {
    case COMPARE:
      if (!op->different) advance_sector(op);
      else op->phase = ERASE;
      break;
    case CHECK_ERASE: op->phase = PROGRAM; break;
    case PROGRAM: op->phase = VERIFY; break;
    case VERIFY: advance_sector(op); break;
    }
  }
  return JM_8BITDO_OK;
invalid:
  op->phase = FAILED;
  op->error = JM_8BITDO_LENGTH;
  return JM_8BITDO_LENGTH;
}
