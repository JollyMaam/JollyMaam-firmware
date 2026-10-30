#ifndef JM_TRANSPORT_JIELI_H
#define JM_TRANSPORT_JIELI_H
#include "upgrade_jieli.h"

typedef int (*jm_jieli_candidate_visitor)(void *user, const char *path, enum jm_jieli_chip chip);
int jm_jieli_candidates(jm_jieli_candidate_visitor visit, void *user);
int jm_jieli_upgrade(const char *path, const uint8_t *loader, size_t loader_size,
                             const uint8_t *image, size_t size);
#endif
