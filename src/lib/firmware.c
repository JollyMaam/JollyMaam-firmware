#include "jollymaam.h"
#include "package.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

#define MAGIC "jollymaam-firmware 2"

static void hex(char out[65], const uint8_t digest[32]) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++)
    out[2 * i] = digits[digest[i] >> 4], out[2 * i + 1] = digits[digest[i] & 15];
  out[64] = 0;
}

int jm_firmware_line(char *out, size_t room, const char *name, const char *role, const char *path,
                     const uint8_t *data, size_t size) {
  uint8_t digest[32];
  char sum[65];
  jm_sha256(data, size, digest);
  hex(sum, digest);
  int n = snprintf(out, room, "%s %s %s %zu %s\n", name, role, path, size, sum);
  return n > 0 && (size_t)n < room && strlen(name) < 17 && strlen(role) < 8 && strlen(path) < 96;
}

/* The manifest's lines: every file read, its size and SHA-256 as stated.
 * error says what is wrong. */
int jm_firmware_open(struct jm_firmware *fw, jm_read read, void *user, char error[128]) {
  const uint8_t *text;
  size_t size;
  memset(fw, 0, sizeof *fw);
  if (!read(user, "manifest.txt", &text, &size)) {
    snprintf(error, 128, "no manifest.txt");
    return 0;
  }
  size_t at = strlen(MAGIC);
  if (size < at || memcmp(text, MAGIC, at) || (at < size && text[at] != '\n')) {
    snprintf(error, 128, "manifest.txt is not \"" MAGIC "\"");
    return 0;
  }
  for (unsigned line = 2; at < size; line++) {
    char buf[256], sum[65], want[65];
    size_t end = at + 1;
    while (end < size && text[end] != '\n') end++;
    size_t n = end - at - 1;
    at = end;
    if (!n) continue;
    if (n >= sizeof buf || fw->count == JM_FIRMWARE_FILES) {
      snprintf(error, 128, "manifest.txt line %u: too long, or too many files", line);
      return 0;
    }
    memcpy(buf, text + end - n, n);
    buf[n] = 0;
    struct jm_firmware_file *f = &fw->files[fw->count];
    unsigned long long declared;
    if (sscanf(buf, "%16s %7s %95s %llu %64s", f->name, f->role, f->path, &declared, want) != 5 ||
        strlen(want) != 64 || strstr(f->path, "..")) {
      snprintf(error, 128, "manifest.txt line %u: not NAME ROLE PATH SIZE SHA256", line);
      return 0;
    }
    if (jm_firmware_find(fw, f->name, f->role)) {
      snprintf(error, 128, "manifest.txt: %s %s twice", f->name, f->role);
      return 0;
    }
    if (!read(user, f->path, &f->data, &f->size) || f->size != declared) {
      snprintf(error, 128, "%s: %s", f->path, f->data ? "not the size stated" : "missing");
      return 0;
    }
    uint8_t digest[32];
    jm_sha256(f->data, f->size, digest);
    hex(sum, digest);
    if (strcmp(sum, want)) {
      snprintf(error, 128, "%s: not the SHA-256 stated", f->path);
      return 0;
    }
    fw->count++;
  }
  if (!fw->count) {
    snprintf(error, 128, "manifest.txt lists no files");
    return 0;
  }
  return 1;
}

const struct jm_firmware_file *jm_firmware_find(const struct jm_firmware *fw, const char *name,
                                                const char *role) {
  for (unsigned i = 0; fw && i < fw->count; i++)
    if (!strcmp(fw->files[i].name, name) && !strcmp(fw->files[i].role, role))
      return &fw->files[i];
  return NULL;
}

int jm_zip_read(void *package, const char *path, const uint8_t **data, size_t *size) {
  struct jm_package_entry e;
  if (!jm_package_find(package, path, &e))
    return 0;
  *data = e.data;
  *size = e.size;
  return 1;
}

#ifdef UNITTEST
#include <assert.h>
struct files { const char *names[4]; const uint8_t *data[4]; size_t sizes[4]; };
static int fake_read(void *user, const char *path, const uint8_t **data, size_t *size) {
  struct files *f = user;
  for (int i = 0; i < 4 && f->names[i]; i++)
    if (!strcmp(f->names[i], path)) {
      *data = f->data[i];
      *size = f->sizes[i];
      return 1;
    }
  return 0;
}
int main(void) {
  static const uint8_t image[] = "image", loader[] = "loader";
  char text[512] = MAGIC "\n", error[128];
  size_t n = strlen(text);
  assert(jm_firmware_line(text + n, sizeof text - n, "U2C_WIRELESS", "install", "a/firmware.img", image, 5));
  n = strlen(text);
  assert(jm_firmware_line(text + n, sizeof text - n, "BR23", "loader", "loaders/br23loader.bin", loader, 6));
  struct files f = {{"manifest.txt", "a/firmware.img", "loaders/br23loader.bin"},
                    {(const uint8_t *)text, image, loader}, {strlen(text), 5, 6}};
  struct jm_firmware fw;
  assert(jm_firmware_open(&fw, fake_read, &f, error) && fw.count == 2);
  assert(jm_firmware_find(&fw, "BR23", "loader")->size == 6 && !jm_firmware_find(&fw, "BR23", "install"));
  f.sizes[1] = 4;                                   /* the size stated */
  assert(!jm_firmware_open(&fw, fake_read, &f, error) && strstr(error, "size"));
  f.sizes[1] = 5;
  static const uint8_t other[] = "imagf";            /* the hash stated */
  f.data[1] = other;
  assert(!jm_firmware_open(&fw, fake_read, &f, error) && strstr(error, "SHA-256"));
  f.data[1] = image;
  char twice[512];
  snprintf(twice, sizeof twice, "%s%s", text, strchr(text, '\n') + 1);   /* a line twice */
  f.data[0] = (const uint8_t *)twice, f.sizes[0] = strlen(twice);
  assert(!jm_firmware_open(&fw, fake_read, &f, error) && strstr(error, "twice"));
  f.data[0] = (const uint8_t *)"jollymaam-firmware 1\n", f.sizes[0] = 21;
  assert(!jm_firmware_open(&fw, fake_read, &f, error));
  assert(jm_product("AC695X_TRANS")->factory_offset == 0x78000 && !jm_product("NOPE"));
  return 0;
}
#endif
