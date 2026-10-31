#ifndef JM_RESTORE_H
#define JM_RESTORE_H
#include "session.h"
struct jm_restore_backup { jm_session_sink write; int (*finish)(void *); void *user; };
int jm_restore_jieli(const struct jm_session_io *io, void *context, enum jm_jieli_chip chip,
                       const char *product, const uint8_t *loader, size_t loader_size,
                       const uint8_t *image, size_t size, const struct jm_restore_backup *backup,
                       jm_session_progress progress, void *progress_user);
int jm_backup_8bitdo(const struct jm_session_io *io, void *context, uint16_t model,
                       jm_session_sink sink, void *user, jm_session_progress progress, void *progress_user);
int jm_restore_8bitdo(const struct jm_session_io *io, void *context, uint16_t model,
                        const uint8_t *image, size_t size, const struct jm_restore_backup *backup,
                        jm_session_progress progress, void *progress_user);
#endif
