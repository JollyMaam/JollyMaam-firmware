#ifndef JM_TRANSPORT_CUSTOM_H
#define JM_TRANSPORT_CUSTOM_H
#include "upgrade_jollymaam.h"
typedef int (*jm_custom_visitor)(void *user, const char *path, const struct jm_identity *identity);
int jm_hid_custom_devices(jm_custom_visitor visit, void *user);
int jm_hid_custom_update(const char *path, const uint8_t expected_uid[16], const uint8_t *image, size_t size,
                           void (*progress)(void *, size_t, size_t), void *user);
#endif
