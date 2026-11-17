#ifndef JM_UPGRADE_JOLLYMAAM_H
#define JM_UPGRADE_JOLLYMAAM_H
#include "proto_jollymaam.h"

struct jm_update_info {
  uint8_t state, error, valid;
  uint32_t at, room, written, begun, key, data_max;
};
struct jm_update_request {
  uint8_t cdb[16];
  const uint8_t *data;
  size_t out_size, in_size;
  unsigned timeout_ms;
};
struct jm_update {
  const uint8_t *image;
  size_t size, chunk, offset, sent;
  unsigned phase, pending, retries;
  int error;
  struct jm_update_info info;
};
enum jm_update_step { JM_UPDATE_FAILED = -1, JM_UPDATE_DONE, JM_UPDATE_TRANSFER };
int jm_update_info_decode(const uint8_t *data, size_t size, struct jm_update_info *out);
int jm_update_init(struct jm_update *op, const uint8_t *image, size_t size,
                     size_t transport_chunk);
enum jm_update_step jm_update_next(struct jm_update *op, struct jm_update_request *request);
int jm_update_complete(struct jm_update *op, int transport_ok, const uint8_t *data, size_t size);
#endif
