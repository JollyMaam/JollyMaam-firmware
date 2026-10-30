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
#endif
