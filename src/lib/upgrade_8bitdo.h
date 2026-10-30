#ifndef JM_UPGRADE_8BITDO_H
#define JM_UPGRADE_8BITDO_H
#include "proto_8bitdo.h"

/* Caller-owned operation. Treat fields as private. Image storage must stay
 * alive and unchanged through completion. No allocation or transport calls. */
struct jm_8bitdo_upgrade {
  const uint8_t *image;
  size_t size;
  uint32_t sectors, sector, offset, completed;
  unsigned phase, pending, different;
  int error;
};
enum jm_8bitdo_step { JM_8BITDO_FAILED = -1, JM_8BITDO_DONE = 0, JM_8BITDO_TRANSFER = 1 };
int jm_8bitdo_upgrade_init(struct jm_8bitdo_upgrade *op,
                           const uint8_t *image, size_t size);
enum jm_8bitdo_step jm_8bitdo_upgrade_next(struct jm_8bitdo_upgrade *op,
                                          uint8_t report[64], int *reply_required);
enum jm_8bitdo_result jm_8bitdo_upgrade_complete(struct jm_8bitdo_upgrade *op,
                                                int transport_ok,
                                                const uint8_t *reply, size_t size);
#endif
