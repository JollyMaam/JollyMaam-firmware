#include "upgrade_jollymaam.h"
#include <string.h>

enum { IDENTIFY, INFO, BEGIN, BEGIN_INFO, DATA, DATA_INFO, COMMIT, COMMIT_INFO, REBOOT, DONE, FAILED };
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Decode version-1 UPDATE_INFO. Reject unknown states and invalid counters.
 * Output is untouched on failure. Additional future trailing fields are ignored. */
int jm_update_info_decode(const uint8_t *data, size_t size, struct jm_update_info *out) {
  if (size < JM_UPDATE_INFO_SIZE || data[JM_UPDATE_INFO_VERSION_OFF] != JM_UPDATE_INFO_VERSION || data[JM_UPDATE_INFO_STATE_OFF] > JM_UPDATE_COMMITTED) return 0;
  struct jm_update_info info;
  info.state = data[JM_UPDATE_INFO_STATE_OFF]; info.error = data[JM_UPDATE_INFO_ERROR_OFF]; info.valid = data[JM_UPDATE_INFO_VALID_OFF];
  info.at = rd32(data + JM_UPDATE_INFO_AT_OFF); info.room = rd32(data + JM_UPDATE_INFO_ROOM_OFF);
  info.written = rd32(data + JM_UPDATE_INFO_WRITTEN_OFF); info.begun = rd32(data + JM_UPDATE_INFO_BEGUN_OFF);
  info.key = rd32(data + JM_UPDATE_INFO_EXTRA_OFF); info.data_max = rd32(data + JM_UPDATE_INFO_DATA_MAX_OFF);
  if (info.written > info.begun || info.begun > info.room) return 0;
  *out = info; return 1;
}
/* Borrow a validated clear MAIN image and configure the transport payload limit.
 * The caller must connect to the updater first, handling app-to-updater entry
 * and re-enumeration itself. No allocation or I/O. The image must remain alive
 * and unchanged through completion. At least 44 output bytes are needed for
 * BEGIN; smaller UPDATE_DATA limits reported by the device are supported. */
int jm_update_init(struct jm_update *op, const uint8_t *image, size_t size,
                     size_t transport_chunk) {
  if (size > UINT32_MAX || !jm_update_image_check(image, size, NULL) ||
      size <= JM_MAIN_HEAD_SIZE || transport_chunk < JM_MAIN_CHECKED_SIZE || transport_chunk > UINT32_MAX) return 0;
  memset(op, 0, sizeof *op);
  op->image = image; op->size = size; op->chunk = transport_chunk;
  return 1;
}
/* Yield a logical command for HID, SCSI-style or BLE framing by the caller.
 * Call complete exactly once before requesting another command. Returned data
 * borrows the original image. Stop scheduling to cancel; after BEGIN the old
 * app can be erased. The final REBOOT has no protocol reply, but its transport
 * completion must still be supplied. No destructive command is auto-retried. */
enum jm_update_step jm_update_next(struct jm_update *op, struct jm_update_request *request) {
  if (op->phase == FAILED || op->pending) return JM_UPDATE_FAILED;
  if (op->phase == DONE) return JM_UPDATE_DONE;
  memset(request, 0, sizeof *request); request->timeout_ms = 10000;
  uint8_t cmd;
  uint32_t arg = 0;
  switch (op->phase) {
  case IDENTIFY: cmd = JM_CMD_IDENTIFY; request->in_size = JM_ID_EXT_SIZE; break;
  case INFO: case BEGIN_INFO: case DATA_INFO: case COMMIT_INFO:
    cmd = JM_CMD_UPDATE_INFO; request->in_size = JM_UPDATE_INFO_SIZE; break;
  case BEGIN:
    cmd = JM_CMD_UPDATE_BEGIN; arg = (uint32_t)op->size;
    request->data = op->image; request->out_size = JM_MAIN_CHECKED_SIZE; break;
  case DATA:
    cmd = JM_CMD_UPDATE_DATA; arg = (uint32_t)op->offset;
    op->sent = op->size - op->offset;
    if (op->sent > op->chunk) op->sent = op->chunk;
    request->data = op->image + op->offset; request->out_size = op->sent; break;
  case COMMIT: cmd = JM_CMD_UPDATE_COMMIT; break;
  case REBOOT: cmd = JM_CMD_REBOOT; break;
  default: op->phase = FAILED; return JM_UPDATE_FAILED;
  }
  jm_command(request->cdb, cmd, arg); op->pending = 1;
  return JM_UPDATE_TRANSFER;
}
/* Advance after a completed logical transfer. Nonzero means accepted.
 * A false transport result ends the operation: no blind command retry occurs.
 * UPDATE_INFO reconciles dropped DATA writes, with a finite retry budget.
 * Positive error values preserve device errors; negative values indicate host
 * validation/transport failures. Completion does not open/reconnect devices. */
int jm_update_complete(struct jm_update *op, int transport_ok, const uint8_t *data, size_t size) {
  if (!op->pending) return 0;
  op->pending = 0;
  if (!transport_ok) { op->error = -1; goto fail; }
  if (op->phase == IDENTIFY) {
    struct jm_identity id;
    if (!jm_identify_decode(data, size, &id) || id.updater != 1 ||
        memcmp(id.product, op->image + JM_MAIN_PRODUCT_OFF, JM_PRODUCT_SIZE)) { op->error = -2; goto fail; }
    op->phase = INFO;
  } else if (op->phase == INFO || op->phase == BEGIN_INFO ||
             op->phase == DATA_INFO || op->phase == COMMIT_INFO) {
    if (!jm_update_info_decode(data, size, &op->info)) { op->error = -3; goto fail; }
    if (op->phase != INFO && op->info.error) { op->error = op->info.error; goto fail; }
    if (op->phase == INFO) {
      if (op->size > op->info.room || (op->info.data_max && op->info.data_max < JM_MAIN_CHECKED_SIZE)) { op->error = -4; goto fail; }
      if (op->info.data_max && op->chunk > op->info.data_max) op->chunk = op->info.data_max;
      op->phase = BEGIN;
    } else if (op->phase == COMMIT_INFO) {
      if (op->info.state != JM_UPDATE_COMMITTED || !op->info.valid || op->info.written != op->size) {
        op->error = -5; goto fail;
      }
      op->phase = REBOOT;
    } else {
      if (op->info.state != JM_UPDATE_WRITING || op->info.begun != op->size ||
          op->info.written > op->offset + op->sent ||
          (op->phase == BEGIN_INFO && op->info.written)) { op->error = -6; goto fail; }
      if (op->phase == DATA_INFO && op->info.written != op->offset + op->sent && ++op->retries > 50) {
        op->error = -7; goto fail;
      }
      op->offset = op->info.written;
      op->phase = op->offset == op->size ? COMMIT : DATA;
    }
  } else if (op->phase == BEGIN) op->phase = BEGIN_INFO;
  else if (op->phase == DATA) op->phase = DATA_INFO;
  else if (op->phase == COMMIT) op->phase = COMMIT_INFO;
  else if (op->phase == REBOOT) op->phase = DONE;
  else { op->error = -8; goto fail; }
  return 1;
fail:
  op->phase = FAILED; return 0;
}

#ifdef UNITTEST
#include "jieli_image/crypto.h"
#include <assert.h>
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void simulate(int mode) {
  uint8_t image[600] = {0}, flash[600] = {0}, reply[88];
  memcpy(image, "MAIN", 4); wr32(image + 4, sizeof image - JM_MAIN_HEAD_SIZE);
  wr32(image + 16, 1); memcpy(image + 24, "test", 4);
  for (size_t i = JM_MAIN_HEAD_SIZE; i < sizeof image; ++i) image[i] = (uint8_t)i;
  wr32(image + 8, jm_crc16(image + JM_MAIN_HEAD_SIZE, sizeof image - JM_MAIN_HEAD_SIZE));
  wr32(image + 40, jm_crc16(image + 4, 36));
  struct jm_update op;
  assert(jm_update_init(&op, image, sizeof image, 128));
  unsigned state = 0, written = 0, begun = 0, commands = 0, erases = 0, rebooted = 0;
  int dropped = 0;
  struct jm_update_request request;
  enum jm_update_step step;
  while ((step = jm_update_next(&op, &request)) == JM_UPDATE_TRANSFER) {
    assert(++commands < 1000);
    memset(reply, 0, sizeof reply);
    uint8_t cmd = request.cdb[1];
    uint32_t arg = rd32(request.cdb + 2);
    if (cmd == 0) {
      memcpy(reply, "C2PD", 4); reply[7] = 1;
      memcpy(reply + 40, mode == 1 ? "wrong" : "test", mode == 1 ? 5 : 4);
    } else if (cmd == JM_CMD_UPDATE_INFO) {
      reply[0] = 1; reply[1] = (uint8_t)state; reply[3] = state == 2;
      wr32(reply + 8, mode == 2 ? 500 : sizeof flash);
      wr32(reply + 12, written); wr32(reply + 16, begun);
      wr32(reply + 24, 64);
      if (mode == 4 && state == 1) reply[2] = 4;
    } else if (cmd == JM_CMD_UPDATE_BEGIN) {
      assert(request.out_size == 44 && arg == sizeof image);
      ++erases; state = 1; begun = arg;
    } else if (cmd == JM_CMD_UPDATE_DATA) {
      assert(request.out_size <= 64 && arg == written);
      if (mode == 3 || (mode == 0 && !dropped)) dropped = 1;
      else {
        memcpy(flash + written, request.data, request.out_size);
        written += (unsigned)request.out_size;
      }
    } else if (cmd == JM_CMD_UPDATE_COMMIT) {
      assert(written == sizeof image && !memcmp(flash, image, sizeof image));
      state = 2;
    } else if (cmd == JM_CMD_REBOOT) {
      assert(state == 2); ++rebooted;
    } else assert(0);
    jm_update_complete(&op, 1, reply, request.in_size);
  }
  if (mode == 0) {
    assert(step == JM_UPDATE_DONE && erases == 1 && rebooted == 1 && op.retries == 1);
  } else {
    assert(step == JM_UPDATE_FAILED && !rebooted);
    if (mode == 1 || mode == 2) assert(!erases);
    if (mode == 3) assert(op.retries == 51);
    if (mode == 4) assert(op.error == 4);
  }
}
int main(void) {
  for (int mode = 0; mode < 5; ++mode) simulate(mode);
  struct jm_update_info info;
  uint8_t data[32] = {0};
  assert(!jm_update_info_decode(data, sizeof data, &info));
  data[0] = 1; data[12] = 1;
  assert(!jm_update_info_decode(data, sizeof data, &info));
  return 0;
}
#endif
