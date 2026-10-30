#ifndef JM_TRANSPORT_HID_H
#define JM_TRANSPORT_HID_H
#include "upgrade_8bitdo.h"
typedef int (*jm_hid_visit)(void *user, const char *path, unsigned vid, unsigned pid);
int jm_hid_enumerate_8bitdo(jm_hid_visit visit, void *user);
int jm_hid_upgrade_8bitdo(const char *path, const uint8_t *image, size_t size,
                          void (*progress)(void *, unsigned, unsigned), void *user);
int jm_hid_8bitdo_model(const char *path, uint16_t *model);
int jm_hid_device_path(const char *path, char out[1024]);
#endif
