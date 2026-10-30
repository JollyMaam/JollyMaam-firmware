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
struct jm_jieli_keep { uint32_t offset, size; };
struct jm_jieli_flash {
  uint8_t *buffer;
  size_t size, offset, sent, keep_index, keep_offset;
  const struct jm_jieli_keep *keep;
  size_t keep_count;
  struct jm_jieli_device device;
  unsigned phase, pending;
  uint16_t command;
  int error;
};
int jm_jieli_flash_init(struct jm_jieli_flash *op, const struct jm_jieli_device *device,
                          const char *device_product, const uint8_t *image, size_t size,
                          uint8_t *workspace, size_t capacity,
                          const struct jm_jieli_keep *keep, size_t keep_count);
enum jm_jieli_step jm_jieli_flash_next(struct jm_jieli_flash *op,
                                        struct jm_jieli_request *request);
int jm_jieli_flash_complete(struct jm_jieli_flash *op, int transport_ok,
                              const uint8_t *data, size_t size);
#endif
