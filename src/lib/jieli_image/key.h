#ifndef JM_JIELI_KEY_H
#define JM_JIELI_KEY_H
#include <stdint.h>
#include <stdbool.h>
bool jm_jieli_record_key(const uint8_t data[34], uint16_t *key);
void jm_jieli_set_record_key(uint8_t data[34], uint16_t key);
#endif
