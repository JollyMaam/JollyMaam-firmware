#include "../src/lib/transport_hid.h"
#include "../src/lib/package.h"
#include "../src/lib/manifest.h"
#include "../src/lib/proto_jollymaam.h"
#include "../src/lib/jieli_image/ufw.h"
#include "../src/lib/jieli_image/entry.h"
#include "../src/lib/jieli_image/image.h"
#include "../src/lib/transport_jieli.h"
#include "jollymaam_update.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int visit(void *user, const char *path, unsigned vid, unsigned pid) {
  (void)user;
  printf("%04x:%04x %s (bootloader candidate; model not probed)\n", vid, pid, path);
  return 0;
}
static int jieli_visit(void *user, const char *path, enum jm_jieli_chip chip) {
  (void)user;
  printf("Jieli %s %s (UBOOT candidate; product not probed)\n", chip == JM_JIELI_BR23 ? "BR23" : "BD19", path);
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
static int rekey(const char *input, const char *output, const char *text) {
  char *end;
  errno = 0;
  unsigned long key = strtoul(text, &end, 0);
  if (errno || !*text || *end || key > 0xffff) {
    fprintf(stderr, "Key must be a 16-bit integer\n"); return 1;
  }
  size_t size;
  uint8_t *data = read_image(input, &size);
  if (!data) { fprintf(stderr, "Cannot read input image\n"); return 1; }
  uint8_t *workspace = malloc(size ? size : 1), *packed = malloc(size ? size : 1);
  struct jm_jieli_image view;
  int result = 1;
  if (!workspace || !packed) goto done;
  int error = jm_jieli_image_open(&view, data, size, workspace, size);
  if (!error) error = jm_jieli_image_pack(&view, (uint16_t)key, packed, size);
  if (error) { fprintf(stderr, "Invalid Jieli image: %d\n", error); goto done; }
  FILE *file = fopen(output, "wb");
  if (!file) { perror(output); goto done; }
  int written = fwrite(packed, 1, size, file) == size;
  int closed = fclose(file) == 0;
  if (!written || !closed) { fprintf(stderr, "Cannot write output image\n"); goto done; }
  result = 0;
done:
  free(data); free(workspace); free(packed);
  return result;
}
int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "update")) return jollymaam_update_app(argv[2]);
  if (argc == 6 && !strcmp(argv[1], "flash-package") && !strcmp(argv[2], "--yes")) {
    size_t size = 0; uint8_t *data = read_image(argv[5], &size);
    struct jm_package package; struct jm_manifest manifest;
    const struct jm_firmware_target *target = NULL;
    int result = -1;
    if (data && jm_package_open(&package, data, size) == JM_PACKAGE_OK &&
        jm_manifest_open(&manifest, &package)) target = jm_manifest_target(&manifest, argv[4]);
    if (!target) fprintf(stderr, "Invalid package or unsupported product\n");
    else if (!strcmp(target->chip, "TLSR8208")) {
      unsigned last = (unsigned)-1;
      result = jm_hid_upgrade_8bitdo(argv[3], target->install.data, target->install.size, progress, &last);
    } else result = jm_linux_jieli_upgrade(argv[3], target->loader.data, target->loader.size,
                                           target->install.data, target->install.size);
    free(data);
    if (result) fprintf(stderr, "Package upgrade failed: %d\n", result);
    else puts("Written and verified (Jieli devices remain in their loader)");
    return result ? 1 : 0;
  }
  if (argc == 6 && !strcmp(argv[1], "flash-jieli") && !strcmp(argv[2], "--yes")) {
    size_t loader_size = 0, image_size = 0;
    uint8_t *loader = read_image(argv[4], &loader_size), *image = read_image(argv[5], &image_size);
    int result = loader && image ? jm_linux_jieli_upgrade(argv[3], loader, loader_size, image, image_size) : -1;
    free(loader); free(image);
    if (result) fprintf(stderr, "Jieli upgrade failed: %d; recovery may be required\n", result);
    else puts("Written and verified; device remains in its loader");
    return result ? 1 : 0;
  }
  if (argc == 5 && !strcmp(argv[1], "rekey-jieli"))
    return rekey(argv[2], argv[3], argv[4]);

  if (argc == 2 && !strcmp(argv[1], "scan")) {
    if (jm_hid_enumerate_8bitdo(visit, NULL) < 0) {
      fprintf(stderr, "HID enumeration failed\n"); return 1;
    }
    if (jm_linux_jieli_candidates(jieli_visit, NULL) < 0) {
      fprintf(stderr, "Jieli enumeration failed\n"); return 1;
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
      struct jm_jieli_image view;
      uint8_t *workspace = malloc(size ? size : 1);
      enum jm_jieli_ufw_result ufw = jm_jieli_ufw_open(data, size, &flash, &flash_size);
      struct jm_package package;
      if (size >= 4 && !memcmp(data, "PK\003\004", 4)) {
        int error = jm_package_open(&package, data, size);
        if (error) fprintf(stderr, "Invalid/unsupported ZIP container: %d\n", error);
        else {
          struct jm_manifest manifest;
          int verified = jm_manifest_open(&manifest, &package);
          if (!verified) fprintf(stderr, "Manifest validation failed\n");
          else printf("Verified firmware manifest: %u targets\n", manifest.count);
          printf("Validated ZIP container: %u entries\n", package.count);
          for (unsigned i = 0; i < package.count; ++i) {
            struct jm_package_entry entry;
            if (jm_package_entry(&package, i, &entry))
              printf("%.*s: %zu bytes\n", (int)entry.name_size, entry.name, entry.size);
          }
          result = verified ? 0 : 1;
        }
      } else if (jm_update_image_check(data, size, NULL)) {
        printf("Validated MAIN update image: bytes=%zu\n", size); result = 0;
      } else if (jm_8bitdo_image_product(data, size, product)) {
        printf("Telink full image: product=%s bytes=%zu\n", product, size); result = 0;
      } else if ((ufw == JM_JIELI_UFW_RAW || ufw == JM_JIELI_UFW_OK) &&
                 workspace && jm_jieli_image_open(&view, flash, flash_size, workspace, size) == JM_JIELI_IMAGE_OK) {
        printf("Jieli image: base=0x%x bytes=%zu wrapper=%s key=0x%04x (validated)\n",
               view.base, flash_size, ufw == JM_JIELI_UFW_OK ? "UFW" : "raw", view.key); result = 0;
      } else fprintf(stderr, "Unrecognized or invalid image\n");
      free(workspace);
    } else {
      unsigned last = (unsigned)-1;
      result = jm_hid_upgrade_8bitdo(argv[3], data, size, progress, &last);
      if (result) fprintf(stderr, "Upgrade failed: %d; device may require recovery\n", result);
    }
    free(data); return result ? 1 : 0;
  }
  fprintf(stderr, "usage: jollymaam update FIRMWARE.zip\n"
                  "       jollymaam scan\n"
                  "       jollymaam inspect IMAGE\n"
                  "       jollymaam rekey-jieli INPUT OUTPUT KEY\n"
                  "       jollymaam flash-8bitdo --yes HID_PATH IMAGE\n"
                  "       jollymaam flash-jieli --yes DEVICE LOADER IMAGE\n"
                  "       jollymaam flash-package --yes DEVICE PRODUCT ZIP\n");
  return 2;
}
