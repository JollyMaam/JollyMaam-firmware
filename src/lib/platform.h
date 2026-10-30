#ifndef JM_PLATFORM_H
#define JM_PLATFORM_H
#include "session.h"
struct jm_platform_connection;
struct jm_platform_connection *jm_platform_open(const char *path, enum jm_session_kind kind);
void jm_platform_close(struct jm_platform_connection *connection);
const struct jm_session_io *jm_platform_io(void);
#endif
