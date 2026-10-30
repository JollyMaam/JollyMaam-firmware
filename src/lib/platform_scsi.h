#ifndef JM_PLATFORM_SCSI_H
#define JM_PLATFORM_SCSI_H
#include "transport_scsi.h"
int jm_scsi_linux_transfer(int fd, const uint8_t *cdb, size_t cdb_size,
                            enum jm_scsi_direction direction, uint8_t *data,
                            size_t size, unsigned timeout_ms,
                            struct jm_scsi_result *result);
#ifdef _WIN32
int jm_scsi_win32_transfer(void *handle, const uint8_t *cdb, size_t cdb_size,
                            enum jm_scsi_direction direction, uint8_t *data,
                            size_t size, unsigned timeout_ms, struct jm_scsi_result *result);
#endif
#endif
