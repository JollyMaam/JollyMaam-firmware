#ifndef JM_JIELI_IMAGE_H
#define JM_JIELI_IMAGE_H
#include "entry.h"

enum jm_jieli_image_result {
  JM_JIELI_IMAGE_OK, JM_JIELI_IMAGE_ARGUMENT, JM_JIELI_IMAGE_HEADER,
  JM_JIELI_IMAGE_BOUNDS, JM_JIELI_IMAGE_CRC, JM_JIELI_IMAGE_BANKCB,
  JM_JIELI_IMAGE_VISITOR
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
enum jm_jieli_file_kind { JM_JIELI_FILE_TOP, JM_JIELI_FILE_APP, JM_JIELI_FILE_RESOURCE };
struct jm_jieli_file {
  char name[17];
  const uint8_t *data;
  size_t size;
  enum jm_jieli_file_kind kind;
};
typedef int (*jm_jieli_file_visitor)(void *user, const struct jm_jieli_file *file);
enum jm_jieli_image_result jm_jieli_image_visit(const struct jm_jieli_image *image,
                                                jm_jieli_file_visitor visit, void *user);
#endif
