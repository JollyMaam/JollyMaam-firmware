#ifndef JM_TRANSPORT_HID_H
#define JM_TRANSPORT_HID_H
#include "upgrade_8bitdo.h"
/* Optional hidapi desktop backend. Enumeration does not open or modify devices.
 * Path is borrowed for the duration of visit. Return nonzero to stop visiting. */
typedef int (*jm_hid_visit)(void *user, const char *path, unsigned vid, unsigned pid);
int jm_hid_enumerate_8bitdo(jm_hid_visit visit, void *user);
/* Blocking desktop convenience driver. Path must be explicitly selected.
 * Returns zero on success, negative on transport failure, positive on protocol
 * failure. Image is validated before opening the device. Progress may be NULL.
 * Caller must obtain user authorization before invoking this function. */
int jm_hid_upgrade_8bitdo(const char *path, const uint8_t *image, size_t size,
                          void (*progress)(void *, unsigned, unsigned), void *user);
#endif
