#ifndef JM_JIELI_IMAGE_H
#define JM_JIELI_IMAGE_H
#include "entry.h"

enum jm_jieli_image_result {
  JM_JIELI_IMAGE_OK, JM_JIELI_IMAGE_ARGUMENT, JM_JIELI_IMAGE_HEADER,
  JM_JIELI_IMAGE_BOUNDS, JM_JIELI_IMAGE_CRC, JM_JIELI_IMAGE_BANKCB
};
struct jm_jieli_image {
  uint8_t *data;
  size_t size;
  uint32_t base, appbase, cipher_size;
  size_t top_count;
  uint16_t key;
};
enum jm_jieli_image_result jm_jieli_image_open(struct jm_jieli_image *out,
                                               const uint8_t *data, size_t size,
                                               uint8_t *workspace, size_t capacity);
enum jm_jieli_image_result jm_jieli_image_pack(const struct jm_jieli_image *image,
                                               uint16_t key, uint8_t *out,
                                               size_t capacity);
#endif
