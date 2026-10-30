#include "../src/lib/transport_hid.h"
#include "../src/lib/proto_jollymaam.h"
#include "../src/lib/jieli_image/ufw.h"
#include "../src/lib/jieli_image/entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int visit(void *user, const char *path, unsigned vid, unsigned pid) {
  (void)user;
  printf("%04x:%04x %s (bootloader candidate; model not probed)\n", vid, pid, path);
  return 0;
}
static uint8_t *read_image(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  long n;
  if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 32 * 1024 * 1024 ||
      fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
  uint8_t *data = malloc(n ? (size_t)n : 1);
  if (!data) { fclose(f); return NULL; }
  if (fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
  fclose(f); *size = (size_t)n; return data;
}
static void progress(void *user, unsigned done, unsigned total) {
  unsigned *last = user;
  if (done != *last) { fprintf(stderr, "Verified %u/%u sectors\n", done, total); *last = done; }
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "scan")) {
    if (jm_hid_enumerate_8bitdo(visit, NULL) < 0) {
      fprintf(stderr, "HID enumeration failed\n"); return 1;
    }
    return 0;
  }
  if ((argc == 3 && !strcmp(argv[1], "inspect")) ||
      (argc == 5 && !strcmp(argv[1], "flash-8bitdo") && !strcmp(argv[2], "--yes"))) {
    size_t size;
    uint8_t *data = read_image(argv[argc - 1], &size);
    if (!data) { fprintf(stderr, "Cannot read image (limit 32 MiB)\n"); return 1; }
    int result = 1;
    if (!strcmp(argv[1], "inspect")) {
      char product[17];
      const uint8_t *flash;
      size_t flash_size;
      uint8_t header[32]; uint32_t base;
      enum jm_jieli_ufw_result ufw = jm_jieli_ufw_open(data, size, &flash, &flash_size);
      if (jm_update_image_check(data, size, NULL)) {
        printf("Validated MAIN update image: bytes=%zu\n", size); result = 0;
      } else if (jm_8bitdo_image_product(data, size, product)) {
        printf("Telink full image: product=%s bytes=%zu\n", product, size); result = 0;
      } else if ((ufw == JM_JIELI_UFW_RAW || ufw == JM_JIELI_UFW_OK) &&
                 jm_jieli_find_base(flash, flash_size, header, &base)) {
        printf("Jieli image: base=0x%x bytes=%zu wrapper=%s (header validated)\n",
               base, flash_size, ufw == JM_JIELI_UFW_OK ? "UFW" : "raw"); result = 0;
      } else fprintf(stderr, "Unrecognized or invalid image\n");
    } else {
      unsigned last = (unsigned)-1;
      result = jm_hid_upgrade_8bitdo(argv[3], data, size, progress, &last);
      if (result) fprintf(stderr, "Upgrade failed: %d; device may require recovery\n", result);
    }
    free(data); return result ? 1 : 0;
  }
  fprintf(stderr, "usage: jollymaam scan\n"
                  "       jollymaam inspect IMAGE\n"
                  "       jollymaam flash-8bitdo --yes HID_PATH IMAGE\n");
  return 2;
}
