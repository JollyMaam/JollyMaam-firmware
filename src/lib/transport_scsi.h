#ifndef JM_TRANSPORT_SCSI_H
#define JM_TRANSPORT_SCSI_H
#include <stddef.h>
#include <stdint.h>

enum jm_scsi_direction { JM_SCSI_NONE, JM_SCSI_IN, JM_SCSI_OUT };
struct jm_scsi_result {
  size_t transferred;
  unsigned status, host_status, driver_status;
  uint8_t sense[32];
  size_t sense_size;
};
/* App-supplied synchronous adapter contract. Return zero for completed I/O,
 * negative for transport errors. A completed SCSI command can still have status
 * or short data; the caller must check the result. Async callers can use the
 * pure protocol CDB helpers without this convenience interface. */
struct jm_scsi_transport {
  void *user;
  int (*transfer)(void *user, const uint8_t *cdb, size_t cdb_size,
                   enum jm_scsi_direction direction, uint8_t *data, size_t size,
                   unsigned timeout_ms, struct jm_scsi_result *result);
};
/* Linux SG_IO using a caller-owned, already-open descriptor. Does not close fd.
 * Returns zero for a completed ioctl, -1 with errno on transport/argument error. */
int jm_scsi_linux_transfer(int fd, const uint8_t *cdb, size_t cdb_size,
                            enum jm_scsi_direction direction, uint8_t *data,
                            size_t size, unsigned timeout_ms,
                            struct jm_scsi_result *result);
#endif
