#ifndef JM_JIELI_UFW_H
#define JM_JIELI_UFW_H
#include <stddef.h>
#include <stdint.h>

enum jm_jieli_ufw_result {
  JM_JIELI_UFW_RAW = 0,
  JM_JIELI_UFW_OK,
  JM_JIELI_UFW_BAD_LIST,
  JM_JIELI_UFW_TRUNCATED,
  JM_JIELI_UFW_BAD_CRC,
  JM_JIELI_UFW_NO_FLASH
};
enum jm_jieli_ufw_result jm_jieli_ufw_open(const uint8_t *data, size_t size,
                                         const uint8_t **flash, size_t *flash_size);
#endif
