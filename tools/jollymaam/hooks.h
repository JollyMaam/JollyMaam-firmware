#ifndef JOLLYMAAM_HOOKS_H
#define JOLLYMAAM_HOOKS_H
/* The platform's device hooks for libjollymaam: hooks_linux.c,
 * hooks_win32.c or hooks_hidapi.c, one per build.  Fills in list, open,
 * close, write, read, set_feature, scsi and sleep. */
#include "../../src/lib/jollymaam.h"
void hooks_device(struct jm_hooks *h);
#endif
