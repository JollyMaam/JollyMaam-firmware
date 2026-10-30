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
int jm_identify_decode(const uint8_t *data, size_t size, struct jm_identity *out);
void jm_command(uint8_t cdb[16], uint8_t command, uint32_t argument);
int jm_update_image_check(const uint8_t *data, size_t size,
                           const char *expected_product);
#endif
