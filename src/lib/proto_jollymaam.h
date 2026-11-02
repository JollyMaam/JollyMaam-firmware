#ifndef JM_PROTO_JOLLYMAAM_H
#define JM_PROTO_JOLLYMAAM_H
#include <stddef.h>
#include <stdint.h>
#include "proto_jollymaam_wire.h"

struct jm_identity {
  uint16_t protocol;
  uint8_t board, updater;
  char build[JM_ID_BUILD_SIZE + 1], product[JM_PRODUCT_SIZE + 1],
       board_name[JM_ID_BOARD_NAME_SIZE + 1];
  uint8_t uid[JM_ID_UID_SIZE];
};
struct jm_schema_info {
  uint16_t fields;
  uint8_t digest[JM_SCHEMA_DIGEST_SIZE];
};
int jm_schema_info_decode(const uint8_t *data, size_t size, struct jm_schema_info *out);
int jm_schema_matches(const struct jm_schema_info *info, const uint8_t *schema, size_t size);
struct jm_battery_status { uint16_t mv; uint8_t percent, flags; };
int jm_battery_decode(const uint8_t *status, size_t size, struct jm_battery_status *out);
int jm_identify_decode(const uint8_t *data, size_t size, struct jm_identity *out);
void jm_command(uint8_t cdb[16], uint8_t command, uint32_t argument);
int jm_update_image_check(const uint8_t *data, size_t size,
                           const char *expected_product);
#endif
