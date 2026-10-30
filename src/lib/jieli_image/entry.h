#ifndef JM_JIELI_ENTRY_H
#define JM_JIELI_ENTRY_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct jm_jieli_entry {
  uint16_t data_crc;
  uint32_t offset, size;
  uint8_t flags, reserved;
  uint16_t index;
  uint8_t name[16];
};

/* Entries are clear 32-byte JLFS records. Decode checks the header CRC and
 * leaves output untouched on failure. Names need not be NUL-terminated. */
void jm_jieli_encode_entry(uint8_t out[32], const struct jm_jieli_entry *entry);
bool jm_jieli_decode_entry(const uint8_t *data, struct jm_jieli_entry *entry);
void jm_jieli_name_string(char out[17], const uint8_t raw[16]);
/* Decode an encrypted flash header at offset, checking bounds and CRC. */
bool jm_jieli_header_at(const uint8_t *data, size_t size, uint32_t offset,
                        uint8_t clear[32]);
/* Resolve the active flash base, including an EOFFSET redirect. On failure
 * base is untouched; clear is scratch storage and may have changed. */
bool jm_jieli_find_base(const uint8_t *data, size_t size, uint8_t clear[32],
                        uint32_t *base);
/* EOFFSET value in 4 KiB units, or zero when absent or invalid. */
uint32_t jm_jieli_eoffset(const uint8_t *data, size_t size, uint32_t offset);
#endif
