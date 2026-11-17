#include "upgrade_jieli.h"
#include <string.h>

enum { UPLOAD, EXECUTE, POLL, IO_SIZE, CHIP_KEY, DONE, FAILED };
static void be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint32_t rd32be(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
/* Start an SDK RAM loader on an already-selected UBOOT1.00 device. The caller
 * must verify the chip using SCSI INQUIRY and provide its matching loader blob.
 * Blob bytes are sent unchanged in 512-byte blocks, matching the SDK format.
 * The blob is borrowed until completion. No allocation, device opening or I/O. */
int jm_jieli_loader_init(struct jm_jieli_loader *op, enum jm_jieli_chip chip,
                           const uint8_t *blob, size_t size) {
  if (!blob || !size || size > 0x10000 || (chip != JM_JIELI_BR23 && chip != JM_JIELI_BD19)) return 0;
  memset(op, 0, sizeof *op);
  op->blob = blob; op->size = size;
  op->address = chip == JM_JIELI_BR23 ? 0x12000 : 0x2000;
  op->device.io = 512;
  return 1;
}
/* Produce one SCSI request. Honor delay_ms before sending, notably between
 * readiness polls. OUT requests borrow the blob. IN replies are supplied to
 * complete; the caller owns their storage and checks SCSI/host/driver status.
 * No write/execute command is retried after an ambiguous transport failure. */
enum jm_jieli_step jm_jieli_loader_next(struct jm_jieli_loader *op,
                                         struct jm_jieli_request *request) {
  if (op->phase == FAILED || op->pending) return JM_JIELI_FAILED;
  if (op->phase == DONE) return JM_JIELI_DONE;
  memset(request, 0, sizeof *request);
  request->direction = JM_SCSI_IN; request->size = 16; request->timeout_ms = 20000;
  uint8_t args[6];
  switch (op->phase) {
  case UPLOAD:
    op->sent = op->size - op->offset;
    if (op->sent > 512) op->sent = 512;
    request->direction = JM_SCSI_OUT; request->size = op->sent;
    request->data = op->blob + op->offset;
    jm_jieli_write_cdb(request->cdb, 0xfb06, op->address + (uint32_t)op->offset, request->data, request->size);
    break;
  case EXECUTE:
    be32(args, op->address); args[4] = 0; args[5] = 1;
    jm_jieli_cdb(request->cdb, 0xfb08, args, 6); break;
  case POLL:
    request->delay_ms = op->polls ? 100 : 0;
    jm_jieli_cdb(request->cdb, 0xfc0a, NULL, 0); break;
  case IO_SIZE: jm_jieli_cdb(request->cdb, 0xfc14, NULL, 0); break;
  case CHIP_KEY:
    be32(args, 0xac6900); jm_jieli_cdb(request->cdb, 0xfc09, args, 4); break;
  default: op->phase = FAILED; return JM_JIELI_FAILED;
  }
  op->pending = 1;
  return JM_JIELI_TRANSFER;
}
/* Complete exactly one request. For OUT, size is the actual transferred byte
 * count and data is ignored. For IN, data is the actual reply. transport_ok must
 * include SCSI status success. Readiness and optional I/O-size probes tolerate
 * failed replies; all destructive requests fail closed. device is usable only
 * once next returns DONE. Error codes are negative host/protocol failures. */
int jm_jieli_loader_complete(struct jm_jieli_loader *op, int transport_ok,
                               const uint8_t *data, size_t size) {
  if (!op->pending) return 0;
  op->pending = 0;
  if (op->phase == UPLOAD) {
    if (!transport_ok || size != op->sent) { op->error = -1; goto fail; }
    op->offset += op->sent;
    if (op->offset == op->size) op->phase = EXECUTE;
    return 1;
  }
  uint16_t cmd = op->phase == EXECUTE ? 0xfb08 : op->phase == POLL ? 0xfc0a :
                   op->phase == IO_SIZE ? 0xfc14 : 0xfc09;
  uint8_t response[14];
  int valid = transport_ok && data && jm_jieli_exec_reply(data, size, cmd, response);
  if (!valid) {
    if (op->phase == POLL && ++op->polls < 20) return 1;
    if (op->phase == IO_SIZE) { op->phase = CHIP_KEY; return 1; }
    op->error = -2; goto fail;
  }
  if (op->phase == EXECUTE) op->phase = POLL;
  else if (op->phase == POLL) {
    op->device.flash_id = response[2] | (uint32_t)response[3] << 8 | (uint32_t)response[4] << 16;
    unsigned capacity = op->device.flash_id & 0x1f;
    if (capacity < 16 || capacity > 26) { op->error = -3; goto fail; }
    op->device.flash_size = (uint32_t)1 << capacity;
    op->phase = IO_SIZE;
  } else if (op->phase == IO_SIZE) {
    uint32_t io = rd32be(response);
    if (io >= 64 && io <= 0x8000) op->device.io = io;
    op->phase = CHIP_KEY;
  } else {
    uint8_t key[2] = {response[5], response[4]};
    jm_jieli_crc_cipher(key, 2, 0xffffffff);
    op->device.key = (uint16_t)(key[0] | key[1] << 8);
    op->phase = DONE;
  }
  return 1;
fail:
  op->phase = FAILED; return 0;
}
#ifdef UNITTEST
#include <assert.h>
static void simulate(int chip, int fail_upload, int never_ready) {
  uint8_t blob[1025] = {0}, reply[16];
  struct jm_jieli_loader op;
  assert(jm_jieli_loader_init(&op, (enum jm_jieli_chip)chip, blob, sizeof blob));
  unsigned uploads = 0, polls = 0;
  struct jm_jieli_request request;
  enum jm_jieli_step step;
  while ((step = jm_jieli_loader_next(&op, &request)) == JM_JIELI_TRANSFER) {
    unsigned cmd = (unsigned)request.cdb[0] << 8 | request.cdb[1];
    memset(reply, 0, sizeof reply); reply[0] = request.cdb[0]; reply[1] = request.cdb[1];
    if (cmd == 0xfb06) {
      ++uploads;
      assert(request.size == (uploads == 3 ? 1 : 512));
      assert(request.data == blob + (uploads - 1) * 512);
      jm_jieli_loader_complete(&op, !fail_upload, NULL, request.size);
    } else if (cmd == 0xfc0a) {
      ++polls;
      if (polls == 1 || never_ready) {
        jm_jieli_loader_complete(&op, 0, NULL, 0); continue;
      }
      assert(request.delay_ms == 100);
      reply[4] = 20; reply[5] = 0x40; reply[6] = 0xef;
      jm_jieli_loader_complete(&op, 1, reply, 16);
    } else if (cmd == 0xfc14) {
      if (chip == JM_JIELI_BD19) {
        reply[4] = 8;
        jm_jieli_loader_complete(&op, 1, reply, sizeof reply);
      } else jm_jieli_loader_complete(&op, 0, NULL, 0);
    } else {
      if (cmd == 0xfc09) {
        uint8_t key[2] = {0x34, 0x12}; jm_jieli_crc_cipher(key, 2, 0xffffffff);
        reply[7] = key[0]; reply[6] = key[1];
      }
      jm_jieli_loader_complete(&op, 1, reply, 16);
    }
  }
  if (fail_upload || never_ready) {
    assert(step == JM_JIELI_FAILED);
    if (fail_upload) assert(uploads == 1 && polls == 0);
    if (never_ready) assert(polls == 20);
  } else {
    assert(step == JM_JIELI_DONE && uploads == 3);
    assert(op.device.flash_size == 0x100000 && op.device.io == (chip == JM_JIELI_BD19 ? 2048u : 512u) && op.device.key == 0x1234);
  }
}
int main(void) {
  simulate(JM_JIELI_BR23, 0, 0); simulate(JM_JIELI_BD19, 0, 0);
  simulate(JM_JIELI_BR23, 1, 0); simulate(JM_JIELI_BR23, 0, 1);
  return 0;
}
#endif
