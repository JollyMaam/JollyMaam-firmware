#ifndef JM_JIELI_KEY_H
#define JM_JIELI_KEY_H
#include <stdint.h>
#include <stdbool.h>
/* Decode a complete 34-byte chip-key record. Output is unchanged on bad CRC. */
bool jm_jieli_record_key(const uint8_t data[34], uint16_t *key);
/* Rewrite only the necessary key bits and update the record CRC. */
void jm_jieli_set_record_key(uint8_t data[34], uint16_t key);
#endif
