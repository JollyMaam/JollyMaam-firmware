#include "../src/lib/jollymaam.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb"); assert(f);
  assert(!fseek(f, 0, SEEK_END)); long length = ftell(f); assert(length > 0);
  rewind(f); size_t n = (size_t)length;
  uint8_t *data = malloc(n), *plain = malloc(n), *packed = malloc(n), *again = malloc(n);
  assert(data && plain && packed && again);
  assert(fread(data, 1, n, f) == n); fclose(f);
  struct jm_jieli_image view, changed;
  int r = jm_jieli_image_open(&view, data, n, plain, n);
  if (r) fprintf(stderr, "open failed: %d: %s\n", r, argv[1]);
  assert(r == JM_JIELI_IMAGE_OK);
  assert(jm_jieli_image_pack(&view, view.key, packed, n) == JM_JIELI_IMAGE_OK);
  assert(!memcmp(data, packed, n));
  uint16_t newkey = view.key ^ 0x1234;
  assert(jm_jieli_image_pack(&view, newkey, packed, n) == JM_JIELI_IMAGE_OK);
  assert(jm_jieli_image_open(&changed, packed, n, again, n) == JM_JIELI_IMAGE_OK);
  assert(changed.key == newkey);
  assert(!memcmp(plain + view.appbase, again + changed.appbase, view.cipher_size));
  assert(jm_jieli_image_pack(&changed, view.key, packed, n) == JM_JIELI_IMAGE_OK);
  assert(!memcmp(data, packed, n));
  assert(jm_jieli_image_open(&changed, data, 31, again, n) != JM_JIELI_IMAGE_OK);
  assert(jm_jieli_image_open(&changed, data, n, again, n - 1) == JM_JIELI_IMAGE_ARGUMENT);
  uint32_t flash_size = 0;
  for (size_t i = 0; i < view.top_count; ++i) {
    struct jm_jieli_entry e;
    assert(jm_jieli_decode_entry(plain + view.base + 32 + i * 32, &e));
    char name[17]; jm_jieli_name_string(name, e.name);
    if (!strcmp(name, "key_mac")) flash_size = e.offset + e.size;
  }
  if (flash_size > n && flash_size <= 8 * 1024 * 1024) {
    uint8_t *full = malloc(flash_size), *scratch = malloc(flash_size), *result = malloc(flash_size);
    assert(full && scratch && result);
    memset(full, 0xa5, flash_size); memcpy(full, data, n);
    struct jm_jieli_image dump;
    assert(jm_jieli_image_open(&dump, full, flash_size, scratch, flash_size) == JM_JIELI_IMAGE_OK);
    assert(jm_jieli_image_pack(&dump, newkey, result, flash_size) == JM_JIELI_IMAGE_OK);
    size_t boundary = dump.appbase + dump.cipher_size;
    assert(boundary < flash_size);
    assert(!memcmp(full + boundary, result + boundary, flash_size - boundary));
    free(full); free(scratch); free(result);
  }
  printf("Byte-exact repack and rekey round trip: %s\n", argv[1]);
  free(data); free(plain); free(packed); free(again);
  return 0;
}
