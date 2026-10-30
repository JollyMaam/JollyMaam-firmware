#include "platform_scsi.h"
#include "transport_scsi.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>

/* Linux SG_IO using a caller-owned, already-open descriptor. Does not close fd.
 * Returns zero for a completed ioctl, -1 with errno on transport/argument error. */
int jm_scsi_linux_transfer(int fd, const uint8_t *cdb, size_t cdb_size,
                            enum jm_scsi_direction direction, uint8_t *data,
                            size_t size, unsigned timeout_ms,
                            struct jm_scsi_result *result) {
  if (!cdb || !result || !cdb_size || cdb_size > 16 || size > INT_MAX ||
      !timeout_ms || (size && !data) || direction > JM_SCSI_OUT ||
      (direction != JM_SCSI_IN && direction != JM_SCSI_OUT && direction != JM_SCSI_NONE) ||
      (size && direction == JM_SCSI_NONE)) {
    errno = EINVAL; return -1;
  }
  struct jm_scsi_result r;
  memset(&r, 0, sizeof r);
  sg_io_hdr_t h;
  memset(&h, 0, sizeof h);
  h.interface_id = 'S';
  h.cmdp = (unsigned char *)cdb; h.cmd_len = (unsigned char)cdb_size;
  h.dxfer_direction = !size ? SG_DXFER_NONE : direction == JM_SCSI_IN ? SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;
  h.dxferp = data; h.dxfer_len = (unsigned)size;
  h.sbp = r.sense; h.mx_sb_len = sizeof r.sense; h.timeout = timeout_ms;
  if (ioctl(fd, SG_IO, &h) < 0) return -1;
  if (h.resid < 0 || (size_t)h.resid > size) { errno = EIO; return -1; }
  r.transferred = size - (size_t)h.resid;
  r.status = h.status; r.host_status = h.host_status; r.driver_status = h.driver_status;
  r.sense_size = h.sb_len_wr < sizeof r.sense ? h.sb_len_wr : sizeof r.sense;
  *result = r;
  return 0;
}
#ifdef UNITTEST
#include <assert.h>
int main(void) {
  uint8_t cdb[16] = {0x12}; struct jm_scsi_result result;
  assert(jm_scsi_linux_transfer(-1, cdb, 17, JM_SCSI_NONE, NULL, 0, 1, &result) == -1);
  assert(errno == EINVAL);
  assert(jm_scsi_linux_transfer(-1, cdb, 6, JM_SCSI_IN, NULL, 36, 1, &result) == -1);
  assert(errno == EINVAL);
  assert(jm_scsi_linux_transfer(-1, cdb, 6, JM_SCSI_NONE, NULL, 0, 1, &result) == -1);
  assert(errno == EBADF);
  return 0;
}
#endif
