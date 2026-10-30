#ifndef JM_SESSION_H
#define JM_SESSION_H
#include "manifest.h"
#include "transport_scsi.h"
#include "proto_jollymaam.h"
enum jm_session_kind { JM_SESSION_CUSTOM, JM_SESSION_8BITDO, JM_SESSION_JIELI };
struct jm_session_io {
  int (*scsi)(void *, const uint8_t *, size_t, enum jm_scsi_direction, uint8_t *, size_t, unsigned, struct jm_scsi_result *);
  int (*hid_write)(void *, const uint8_t *, size_t);
  int (*hid_read)(void *, uint8_t *, size_t, unsigned);
  int (*feature_set)(void *, const uint8_t *, size_t);
  int (*feature_get)(void *, uint8_t *, size_t);
  void (*wait_ms)(void *, unsigned);
  int (*reconnect)(void *, const uint8_t uid[16]);
  size_t (*feature_size)(void *);
};
typedef void (*jm_session_progress)(void *, size_t, size_t);
int jm_session_run(const struct jm_firmware_target *target, enum jm_session_kind kind,
                     const uint8_t expected_uid[16], const struct jm_session_io *io, void *context,
                     jm_session_progress progress, void *user);
int jm_session_identify(const struct jm_session_io *io, void *context, struct jm_identity *identity);
int jm_session_jieli(const struct jm_session_io *io, void *context, const uint8_t *loader,
                       size_t loader_size, const uint8_t *image, size_t size);
int jm_session_8bitdo(const struct jm_session_io *io, void *context, const uint8_t *image,
                        size_t size, jm_session_progress progress, void *user);
int jm_session_custom(const struct jm_session_io *io, void *context, const uint8_t uid[16],
                        const uint8_t *image, size_t size, jm_session_progress progress, void *user);
#endif
