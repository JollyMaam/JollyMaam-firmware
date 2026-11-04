#ifndef JM_SESSION_H
#define JM_SESSION_H
#include "manifest.h"
#include "transport_scsi.h"
#include "proto_jollymaam.h"
#include "upgrade_jieli.h"
enum jm_session_kind { JM_SESSION_CUSTOM, JM_SESSION_8BITDO, JM_SESSION_JIELI };
struct jm_session_io {
  int (*scsi)(void *, const uint8_t *, size_t, enum jm_scsi_direction, uint8_t *, size_t, unsigned, struct jm_scsi_result *);
  int (*hid_write)(void *, const uint8_t *, size_t);
  int (*hid_read)(void *, uint8_t *, size_t, unsigned);
  int (*feature_set)(void *, const uint8_t *, size_t);
  void (*wait_ms)(void *, unsigned);
  int (*reconnect)(void *, const uint8_t uid[16]);
  size_t (*feature_size)(void *);
  /* Optional: this program's client number in the tool device's answers
   * (unique among programs on one device); without it one is made up. */
  uint32_t (*client)(void *);
};
typedef void (*jm_session_progress)(void *, size_t, size_t);
int jm_session_run(const struct jm_firmware_target *target, enum jm_session_kind kind,
                     const uint8_t expected_uid[16], const struct jm_session_io *io, void *context,
                     jm_session_progress progress, void *user);
int jm_session_identify(const struct jm_session_io *io, void *context, struct jm_identity *identity);
int jm_session_stock_loader(const struct jm_session_io *io, void *context, const uint8_t expected_uid[16]);
int jm_session_jieli_open(const struct jm_session_io *io, void *context, const uint8_t *loader,
                            size_t loader_size, struct jm_jieli_device *device);
int jm_session_jieli_read(const struct jm_session_io *io, void *context,
                            const struct jm_jieli_device *device, uint32_t address, uint8_t *data, size_t size);
typedef int (*jm_session_sink)(void *, uint32_t, const uint8_t *, size_t);
int jm_session_jieli_backup(const struct jm_session_io *io, void *context,
                              const struct jm_jieli_device *device, jm_session_sink sink, void *user,
                              jm_session_progress progress, void *progress_user);
int jm_session_jieli_start(const struct jm_session_io *io, void *context);
int jm_session_jieli(const struct jm_session_io *io, void *context, const uint8_t *loader,
                       size_t loader_size, const uint8_t *image, size_t size);
int jm_session_8bitdo(const struct jm_session_io *io, void *context, const uint8_t *image,
                        size_t size, jm_session_progress progress, void *user);
int jm_session_8bitdo_yichip(const struct jm_session_io *io, void *context, const uint8_t *app, size_t size,
                              jm_session_progress progress, void *user);
int jm_session_8bitdo_stock(const struct jm_session_io *io, void *context, const uint8_t *image,
                              size_t size, uint16_t model, jm_session_progress progress, void *user);
int jm_session_custom(const struct jm_session_io *io, void *context, const uint8_t uid[16],
                        const uint8_t *image, size_t size, jm_session_progress progress, void *user);
#endif
