#include "upgrade_8bitdo.h"
#include "proto_jollymaam.h"
#include <string.h>

enum { IDENTIFY, COMPARE, ERASE, CHECK_ERASE, PROGRAM, VERIFY, START, DONE, FAILED };

/* ---- Yichip: one stream (JM_8BITDO_YICHIP_* in proto_8bitdo.h) ------------
 * IDENTIFY (model 301a), ERASE at the base with the stream's length, PROGRAM
 * it in encrypted 46-byte pieces, VERIFY reading the image back (46 bytes
 * at a time within each 4 KiB, as the reads are known to work), START.
 * offset counts the stream's bytes in the phase; progress is in 4 KiB,
 * programmed then verified.
 *
 * The padding is not verified: the stream's last 512-byte block reads back
 * wrong (0x01047e00 for a stream to 0x01048000, 2026-11-09; 28 99 ff ff ff
 * ff 28 99 ...), also after a stream past it read it right.  Hence too the
 * stream keeps its last block for padding. */
static uint32_t yichip_verified(const struct jm_8bitdo_upgrade *op) {
  return (uint32_t)((op->size + JM_8BITDO_YICHIP_CIPHER_BLOCK - 1) / JM_8BITDO_YICHIP_CIPHER_BLOCK *
                    JM_8BITDO_YICHIP_CIPHER_BLOCK);
}
static uint8_t yichip_plain(const struct jm_8bitdo_upgrade *op, uint32_t at) {
  return at < op->size ? op->image[at] : 0xff;
}
static uint8_t yichip_cipher(struct jm_8bitdo_upgrade *op, uint32_t at) {
  uint32_t block = at / JM_8BITDO_YICHIP_CIPHER_BLOCK;
  if (block != op->cipher_block) {
    uint8_t plain[JM_8BITDO_YICHIP_CIPHER_BLOCK];
    for (uint32_t i = 0; i < sizeof plain; ++i)
      plain[i] = yichip_plain(op, block * JM_8BITDO_YICHIP_CIPHER_BLOCK + i);
    jm_8bitdo_yichip_encrypt(block, plain, op->cipher);
    op->cipher_block = block;
  }
  return op->cipher[at % JM_8BITDO_YICHIP_CIPHER_BLOCK];
}
static unsigned yichip_chunk(const struct jm_8bitdo_upgrade *op) {
  uint32_t left = (op->phase == VERIFY ? yichip_verified(op) : op->stream) - op->offset;
  if (op->phase == VERIFY && 4096 - op->offset % 4096 < left) left = 4096 - op->offset % 4096;
  return left < JM_8BITDO_CHUNK ? (unsigned)left : JM_8BITDO_CHUNK;
}
/* A raw app for the Yichip pads' slot (from JM_8BITDO_YICHIP_BASE): it
 * must fit the slot in whole 32 KiB blocks.  The image has no product tag;
 * the loader's model (301a) is checked before the erase. */
int jm_8bitdo_yichip_init(struct jm_8bitdo_upgrade *op, const uint8_t *app, size_t size) {
  const uint32_t room = (JM_8BITDO_YICHIP_END - JM_8BITDO_YICHIP_BASE) / JM_8BITDO_YICHIP_BLOCK *
                        JM_8BITDO_YICHIP_BLOCK;
  if (!op || !app || !size || size + JM_8BITDO_YICHIP_CIPHER_BLOCK > room)
    return 0;
  memset(op, 0, sizeof *op);
  op->yichip = 1;
  op->image = app;
  op->size = size;
  op->stream = (uint32_t)((size + JM_8BITDO_YICHIP_CIPHER_BLOCK + JM_8BITDO_YICHIP_BLOCK - 1) /
                          JM_8BITDO_YICHIP_BLOCK * JM_8BITDO_YICHIP_BLOCK);
  op->sectors = op->stream / 4096 + (yichip_verified(op) + 4095) / 4096;
  op->cipher_block = 0xffffffffu;
  op->phase = IDENTIFY;
  return 1;
}
static enum jm_8bitdo_step yichip_next(struct jm_8bitdo_upgrade *op, uint8_t report[64], int *reply_required) {
  uint16_t command;
  uint32_t at = JM_8BITDO_YICHIP_BASE + op->offset, total = op->stream;
  size_t n = 0;
  uint8_t data[JM_8BITDO_CHUNK];
  const uint8_t *payload = NULL;
  switch (op->phase) {
  case IDENTIFY: command = 8; at = 0; n = 2; total = 2; break;
  case ERASE: command = 4; at = JM_8BITDO_YICHIP_BASE; break;
  case PROGRAM:
    command = 3; n = yichip_chunk(op); payload = data;
    for (unsigned i = 0; i < n; ++i) data[i] = yichip_cipher(op, op->offset + i);
    break;
  case VERIFY: command = 5; n = yichip_chunk(op); total = (uint32_t)n; break;
  case START: command = 7; at = 0; total = 0; break;
  default: op->phase = FAILED; return JM_8BITDO_FAILED;
  }
  if (jm_8bitdo_request_total(report, command, at, payload, n, total) != JM_8BITDO_OK) {
    op->phase = FAILED;
    return JM_8BITDO_FAILED;
  }
  op->pending = 1;
  *reply_required = op->phase != START;
  return JM_8BITDO_TRANSFER;
}
static enum jm_8bitdo_result yichip_complete(struct jm_8bitdo_upgrade *op, const uint8_t *reply, size_t size) {
  uint16_t command = op->phase == IDENTIFY ? 8 : op->phase == ERASE ? 4 : op->phase == PROGRAM ? 3 : 5;
  const uint8_t *data;
  size_t n;
  unsigned count = op->phase == PROGRAM || op->phase == VERIFY ? yichip_chunk(op) : 0;
  enum jm_8bitdo_result result = jm_8bitdo_reply(reply, size, command, JM_8BITDO_YICHIP_BASE + op->offset, &data, &n);
  if (result == JM_8BITDO_STALE)
    return result;
  if (result == JM_8BITDO_LENGTH && (command == 3 || command == 4))
    result = JM_8BITDO_OK;       /* its acks carry the status alone */
  op->pending = 0;
  if (result != JM_8BITDO_OK) {
    op->phase = FAILED; op->error = result;
    return result;
  }
  switch (op->phase) {
  case IDENTIFY:                 /* the pad's answers 4 bytes: 1a 30 00 00 */
    if (n < 2 || data[0] != (uint8_t)JM_8BITDO_MODEL_U2C_BLUETOOTH || data[1] != JM_8BITDO_MODEL_U2C_BLUETOOTH >> 8)
      goto invalid;
    op->phase = ERASE;
    return JM_8BITDO_OK;
  case ERASE:
    op->phase = PROGRAM;
    return JM_8BITDO_OK;
  case VERIFY:
    if (n != count)
      goto invalid;
    for (unsigned i = 0; i < count; ++i)
      if (data[i] != yichip_plain(op, op->offset + i))
        goto invalid;
    break;
  }
  op->offset += count;
  op->completed = (op->phase == VERIFY ? op->stream : 0) / 4096 + (op->offset + 4095) / 4096;
  if (op->offset == (op->phase == VERIFY ? yichip_verified(op) : op->stream)) {
    op->offset = 0;
    op->phase = op->phase == PROGRAM ? VERIFY : START;
  }
  return JM_8BITDO_OK;
invalid:
  op->phase = FAILED;
  op->error = JM_8BITDO_LENGTH;
  return JM_8BITDO_LENGTH;
}
/* How long the pending request's reply may take, in ms: the Yichip erase
 * of the whole stream is the slow one. */
unsigned jm_8bitdo_upgrade_timeout(const struct jm_8bitdo_upgrade *op) {
  return op && op->yichip && op->phase == ERASE ? 30000 : 10000;
}
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
/* Initialize a stock packed Telink app at 0x6000. The caller explicitly
 * supplies its independently established model: raw stock images contain no
 * custom MAIN product tag. Only pad model 301b is supported; live IDENTIFY is
 * still checked before erasing. Require exact declared length and CRC, refuse
 * factory flash dumps; DAT containers are checked/unwrapped. Leave settings
 * at 0x6f000 intact. */
int jm_8bitdo_restore_init(struct jm_8bitdo_upgrade *op,const uint8_t *image,size_t size,uint16_t model) {
  const uint8_t *app;size_t length;
  if(!op || !jm_8bitdo_stock_image(image,size,model,&app,&length)) return 0;
  image=app;size=length;
  memset(op,0,sizeof *op);op->image=image;op->size=size;
  op->sectors=(uint32_t)((size+4095)/4096);op->sector=op->sectors>1?1:0;op->phase=IDENTIFY;
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
  if (op->yichip)
    return yichip_next(op, report, reply_required);
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
  if (op->yichip)
    return yichip_complete(op, reply, size);
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
