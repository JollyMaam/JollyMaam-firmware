#ifndef JM_JIELI_BUILDER_H
#define JM_JIELI_BUILDER_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define JM_JIELI_BUILD_FILES 32
struct jm_jieli_copy { uint32_t from, to, len; };
struct jm_jieli_build_options {
  const char *ini, *uboot;
  const char *files[JM_JIELI_BUILD_FILES], *res[JM_JIELI_BUILD_FILES];
  size_t nf, nr;
  bool have_key, have_at, have_mark;
  uint16_t key, mark;
  uint32_t at, flash_size, code_limit;
  struct jm_jieli_copy copies[16];
  size_t nc;
  const char *main_file;
  uint32_t main_at;
};
struct jm_jieli_build_result { uint8_t *image, *update; size_t image_size, update_size; uint16_t key; };
typedef int (*jm_jieli_read_file)(void *user, const char *name, const uint8_t **data, size_t *size);
typedef void (*jm_jieli_build_log)(void *user, const char *message);
int jm_jieli_build(const struct jm_jieli_build_options *options, jm_jieli_read_file read,
                     jm_jieli_build_log log, void *user,
                     struct jm_jieli_build_result *out, char error[256]);
void jm_jieli_build_free(struct jm_jieli_build_result *result);
#endif
