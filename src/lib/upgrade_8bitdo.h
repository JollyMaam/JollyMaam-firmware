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
/* Validates full pad image and bounds before producing any device requests.
 * Receiver images are rejected. No device changes occur until model checked. */
int jm_8bitdo_upgrade_init(struct jm_8bitdo_upgrade *op,
                           const uint8_t *image, size_t size);
/* Produce the next unnumbered 64-byte HID report. reply_required is false
 * only for application start; complete that transfer with NULL,0 on success.
 * Call complete exactly once per request, except STALE replies may be retried.
 * Stop scheduling to cancel. After any erase the device may need recovery.
 * completed counts sectors verified/skipped; sectors includes header sector. */
enum jm_8bitdo_step jm_8bitdo_upgrade_next(struct jm_8bitdo_upgrade *op,
                                          uint8_t report[64], int *reply_required);
/* transport_ok false ends the operation without retrying destructive commands.
 * Returns STALE to keep waiting for the outstanding read, OK otherwise.
 * Malformed/status replies end the operation. Inspect next for final status. */
enum jm_8bitdo_result jm_8bitdo_upgrade_complete(struct jm_8bitdo_upgrade *op,
                                                int transport_ok,
                                                const uint8_t *reply, size_t size);
#endif
