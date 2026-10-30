#ifndef JM_PROTO_JOLLYMAAM_H
#define JM_PROTO_JOLLYMAAM_H
#include <stddef.h>
#include <stdint.h>

struct jm_identity {
  uint16_t protocol;
  uint8_t board, updater;
  char build[17], product[17], board_name[33];
  uint8_t uid[16];
};
/* Decode IDENTIFY, accepting the original 56-byte and extended 88-byte form.
 * Strings are terminated. Output is untouched on failure. */
int jm_identify_decode(const uint8_t *data, size_t size, struct jm_identity *out);
/* Logical SCSI-style command. HID/GATT framing belongs to the transport layer. */
void jm_command(uint8_t cdb[16], uint8_t command, uint32_t argument);
/* Validate clear update.bin: MAIN commit/header CRC, format, exact size,
 * application CRC and optional product match. No device I/O. expected_product
 * may be NULL; it must otherwise be at most 16 bytes plus terminator. */
int jm_update_image_check(const uint8_t *data, size_t size,
                           const char *expected_product);
#endif
