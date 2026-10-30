#ifndef JM_JIELI_BANKCB_H
#define JM_JIELI_BANKCB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool jm_jieli_scramble_bankcb(uint8_t *data, size_t size);
bool jm_jieli_descramble_bankcb(uint8_t *data, size_t size);
#endif
