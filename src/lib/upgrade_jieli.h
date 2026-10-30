#ifndef JM_UPGRADE_JIELI_H
#define JM_UPGRADE_JIELI_H
#include "proto_jieli.h"
#include "transport_scsi.h"

enum jm_jieli_chip { JM_JIELI_BR23, JM_JIELI_BD19 };
enum jm_jieli_step { JM_JIELI_FAILED = -1, JM_JIELI_DONE, JM_JIELI_TRANSFER };
struct jm_jieli_request {
  uint8_t cdb[16];
  enum jm_scsi_direction direction;
  const uint8_t *data;
  size_t size;
  unsigned timeout_ms, delay_ms;
};
struct jm_jieli_device {
  uint32_t flash_id, flash_size, io;
  uint16_t key;
};
struct jm_jieli_loader {
  const uint8_t *blob;
  size_t size, offset, sent;
  uint32_t address;
  unsigned phase, pending, polls;
  int error;
  struct jm_jieli_device device;
};
int jm_jieli_loader_init(struct jm_jieli_loader *op, enum jm_jieli_chip chip,
                           const uint8_t *blob, size_t size);
enum jm_jieli_step jm_jieli_loader_next(struct jm_jieli_loader *op,
                                         struct jm_jieli_request *request);
int jm_jieli_loader_complete(struct jm_jieli_loader *op, int transport_ok,
                               const uint8_t *data, size_t size);
#endif
