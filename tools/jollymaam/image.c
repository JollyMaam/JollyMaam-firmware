/* jollymaam's image commands: building the images the loaders take (pack),
 * Jieli images apart (unpack), what a file is (inspect), and a firmware
 * package from a build (package). */
#define _XOPEN_SOURCE 700
#include "cli.h"
#include "../../src/lib/jollymaam.h"
#include "../../src/lib/package.h"
#include "../../src/lib/proto_8bitdo.h"
#include "../../src/lib/jieli_image/builder.h"
#include "../../src/lib/jieli_image/config.h"
#include "../../src/lib/jieli_image/entry.h"
#include "../../src/lib/jieli_image/image.h"
#include "../../src/lib/jieli_image/ufw.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("jollymaam: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  long n = 0;
  if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 64 * 1024 * 1024 || fseek(f, 0, SEEK_SET))
    die("%s: %s", path, f ? "cannot read" : strerror(errno));
  uint8_t *p = malloc(n ? (size_t)n : 1);
  if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) die("%s: cannot read", path);
  fclose(f);
  *size = (size_t)n;
  return p;
}

void write_file(const char *path, const void *data, size_t size) {
  FILE *f = fopen(path, "wb");
  if (!f || (size && fwrite(data, 1, size, f) != size) || fclose(f)) die("%s: cannot write", path);
}

uint32_t number(const char *text) {
  char *end;
  errno = 0;
  unsigned long long v = strtoull(text, &end, 0);
  if (*end == 'K' || *end == 'k') v *= 1024, end++;
  else if (*end == 'M' || *end == 'm') v *= 1024 * 1024, end++;
  if (errno || !*text || *end || v > 0xffffffffu) die("not a number: %s", text);
  return (uint32_t)v;
}

int is_dir(const char *path) {
  struct stat st;
  return !stat(path, &st) && S_ISDIR(st.st_mode);
}

void make_dir(const char *path) {
#ifdef _WIN32
  if (mkdir(path) && errno != EEXIST)
#else
  if (mkdir(path, 0777) && errno != EEXIST)
#endif
    die("%s: %s", path, strerror(errno));
}

static const char *arg(int argc, char **argv, int *i) {
  if (*i + 1 >= argc) die("%s needs a value", argv[*i]);
  return argv[++*i];
}

/* ---- pack ------------------------------------------------------------------------------------
 * jieli: a Jieli flash image from an SDK isd_config.ini (as isd_download
 *   takes it) or one unpack wrote, the uboot, the app files; --main puts a
 *   second app above the first behind its MAIN header (--main-out writes it
 *   in the clear: update.bin).  docs/jieli/re-application-image.md.
 * telink: a Telink app from objcopy as the 8BitDo boot loader starts it.
 * join: the Telink updater's image, 0xff to --at, then update.bin (the app
 *   behind its MAIN header): firmware.img; --main-out writes update.bin. */

static int read_any(void *user, const char *name, const uint8_t **data, size_t *size) {
  uint8_t **current = user;
  free(*current);
  *data = *current = read_file(name, size);
  return 1;
}
static void log_line(void *user, const char *message) {
  (void)user;
  fputs(message, stdout);
}

static int pack_jieli(int argc, char **argv) {
  struct jm_jieli_build_options o = {0};
  const char *out = NULL, *main_out = NULL;
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-c")) o.ini = arg(argc, argv, &i);
    else if (!strcmp(a, "-u")) o.uboot = arg(argc, argv, &i);
    else if (!strcmp(a, "-o")) out = arg(argc, argv, &i);
    else if (!strcmp(a, "-k")) o.key = (uint16_t)number(arg(argc, argv, &i)), o.have_key = 1;
    else if (!strcmp(a, "-r") && o.nr < JM_JIELI_BUILD_FILES) o.res[o.nr++] = arg(argc, argv, &i);
    else if (!strcmp(a, "--flash-size")) o.flash_size = number(arg(argc, argv, &i));
    else if (!strcmp(a, "--code-limit")) o.code_limit = number(arg(argc, argv, &i));
    else if (!strcmp(a, "--at")) o.at = number(arg(argc, argv, &i)), o.have_at = 1;
    else if (!strcmp(a, "--mark")) o.mark = (uint16_t)number(arg(argc, argv, &i)), o.have_mark = 1;
    else if (!strcmp(a, "--copy") && o.nc < 16) {
      long from, to, len;
      if (sscanf(arg(argc, argv, &i), "%li:%li:%li", &from, &to, &len) != 3)
        die("--copy is FROM:TO:LEN");
      o.copies[o.nc++] = (struct jm_jieli_copy){(uint32_t)from, (uint32_t)to, (uint32_t)len};
    } else if (!strcmp(a, "--main")) {
      static char spec[1024];
      snprintf(spec, sizeof spec, "%s", arg(argc, argv, &i));
      char *colon = strchr(spec, ':');
      if (!colon) die("--main is OFFSET:FILE");
      *colon = 0, o.main_at = number(spec), o.main_file = colon + 1;
    } else if (!strcmp(a, "--main-out")) main_out = arg(argc, argv, &i);
    else if (a[0] != '-' && o.nf < JM_JIELI_BUILD_FILES) o.files[o.nf++] = a;
    else die("pack jieli: unknown argument %s", a);
  }
  if (!out) die("pack jieli needs -o OUT");
  uint8_t *current = NULL;
  struct jm_jieli_build_result r;
  char error[256];
  int ok = jm_jieli_build(&o, read_any, log_line, &current, &r, error);
  free(current);
  if (!ok) die("%s", error);
  write_file(out, r.image, r.image_size);
  if (main_out && r.update) write_file(main_out, r.update, r.update_size);
  jm_jieli_build_free(&r);
  return 0;
}

static int pack_telink(int argc, char **argv) {
  const char *stamp = NULL, *out = NULL, *app = NULL;
  uint32_t max = 0;
  uint8_t bytes[6];
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--stamp")) stamp = arg(argc, argv, &i);
    else if (!strcmp(argv[i], "--max")) max = number(arg(argc, argv, &i));
    else if (!strcmp(argv[i], "-o")) out = arg(argc, argv, &i);
    else if (argv[i][0] != '-' && !app) app = argv[i];
    else die("pack telink: unknown argument %s", argv[i]);
  }
  if (!stamp || !max || !out || !app) die("usage: jollymaam pack telink --stamp HEX --max BYTES -o OUT APP");
  for (int i = 0; i < 6; i++)
    if (strlen(stamp) != 12 || sscanf(stamp + 2 * i, "%2hhx", &bytes[i]) != 1) die("--stamp: 6 bytes in hex");
  size_t n, made;
  uint8_t *data = read_file(app, &n), *image = malloc(n + 20);
  if (!image || !jm_8bitdo_telink_pack(data, n, bytes, image, n + 20, &made)) die("%s: not a Telink app", app);
  if (made > max) die("%s: the image is %zu bytes, more than the %u it may take", app, made, max);
  write_file(out, image, made);
  free(data), free(image);
  return 0;
}

static int pack_join(int argc, char **argv) {
  const char *pid = NULL, *out = NULL, *main_out = NULL, *files[2] = {0};
  uint32_t at = 0, entry = 0;
  int nf = 0;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--pid")) pid = arg(argc, argv, &i);
    else if (!strcmp(argv[i], "--at")) at = number(arg(argc, argv, &i));
    else if (!strcmp(argv[i], "--entry")) entry = number(arg(argc, argv, &i));
    else if (!strcmp(argv[i], "--main-out")) main_out = arg(argc, argv, &i);
    else if (!strcmp(argv[i], "-o")) out = arg(argc, argv, &i);
    else if (argv[i][0] != '-' && nf < 2) files[nf++] = argv[i];
    else die("pack join: unknown argument %s", argv[i]);
  }
  if (!pid || !at || !entry || !out || nf != 2)
    die("usage: jollymaam pack join --pid PID --at OFFSET --entry ADDR [--main-out UPDATE] -o OUT UPDATER APP");
  size_t un, an;
  uint8_t *updater = read_file(files[0], &un), *app = read_file(files[1], &an);
  if (!jm_8bitdo_image_check(updater, un)) die("%s: not a packed Telink image", files[0]);
  if (at % 4096 || un > at) die("--at: a sector boundary past the updater (%zu bytes)", un);
  size_t total = at + JM_MAIN_HEAD_SIZE + an;
  uint8_t *image = malloc(total);
  if (!image) die("out of memory");
  memcpy(image, updater, un);
  memset(image + un, 0xff, at - un);
  if (!jm_update_image_make(app, an, pid, at, entry, image + at, total - at)) die("--pid: 1 to 16 characters");
  if (main_out) write_file(main_out, image + at, total - at);
  write_file(out, image, total);
  free(updater), free(app), free(image);
  return 0;
}

int cmd_pack(int argc, char **argv) {
  if (argc >= 1 && !strcmp(argv[0], "jieli")) return pack_jieli(argc - 1, argv + 1);
  if (argc >= 1 && !strcmp(argv[0], "telink")) return pack_telink(argc - 1, argv + 1);
  if (argc >= 1 && !strcmp(argv[0], "join")) return pack_join(argc - 1, argv + 1);
  die("pack jieli|telink|join (jollymaam help)");
  return 2;
}

/* ---- Jieli images: what they hold, and apart ----------------------------------------------- */

/* A raw flash image, unwrapping a UFW file. */
static const uint8_t *jieli_flash(const uint8_t *data, size_t size, size_t *out) {
  const uint8_t *flash;
  enum jm_jieli_ufw_result r = jm_jieli_ufw_open(data, size, &flash, out);
  return r == JM_JIELI_UFW_RAW || r == JM_JIELI_UFW_OK ? flash : NULL;
}

static void jieli_listing(const struct jm_jieli_image *image, const uint8_t *raw, const uint8_t *plain) {
  char pid[17], name[17];
  jm_jieli_name_string(pid, raw + image->base + 16);
  printf("Jieli image: base 0x%x, PID %s, key 0x%04x\n", image->base, pid, image->key);
  for (size_t i = 0; i < image->top_count; i++) {
    struct jm_jieli_entry e;
    jm_jieli_decode_entry(plain + image->base + 32 + i * 32, &e);
    jm_jieli_name_string(name, e.name);
    printf("  %-16s 0x%08x size 0x%x\n", name, e.offset, e.size);
  }
}

struct extract { const char *dir; };
static int extract_file(void *user, const struct jm_jieli_file *file) {
  struct extract *x = user;
  char path[1024];
  if (!strcmp(file->name, "isd_config.ini")) return 0;
  if (!*file->name || !strcmp(file->name, ".") || !strcmp(file->name, "..") || strpbrk(file->name, "/\\:"))
    return 1;
  snprintf(path, sizeof path, "%s/%s", x->dir, file->name);
  write_file(path, file->data, file->size);
  return 0;
}

/* unpack IMAGE DIR: its files and an isd_config.ini pack takes back. */
int cmd_unpack(int argc, char **argv) {
  size_t size, flash_size;
  char path[1024], *config;
  if (argc != 2) die("usage: jollymaam unpack IMAGE DIR");
  uint8_t *data = read_file(argv[0], &size);
  const uint8_t *flash = jieli_flash(data, size, &flash_size);
  uint8_t *work = malloc(size ? size : 1);
  struct jm_jieli_image image;
  if (!flash || !work || jm_jieli_image_open(&image, flash, flash_size, work, flash_size))
    die("%s: not a Jieli image", argv[0]);
  make_dir(argv[1]);
  struct extract x = {argv[1]};
  if (jm_jieli_image_visit(&image, extract_file, &x) != JM_JIELI_IMAGE_OK) die("cannot extract its files safely");
  const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
  if (!jm_jieli_image_config(&image, base, &config, &size)) die("cannot make its isd_config.ini");
  snprintf(path, sizeof path, "%s/isd_config.ini", argv[1]);
  write_file(path, config, size);
  free(config), free(work), free(data);
  return 0;
}

int inspect_image(const char *path, const uint8_t *data, size_t size) {
  char product[17];
  size_t flash_size;
  const uint8_t *flash = jieli_flash(data, size, &flash_size);
  struct jm_jieli_image image;
  (void)path;
  if (jm_update_image_check(data, size, NULL)) {
    printf("update.bin for %.16s: %zu bytes\n", (const char *)data + JM_MAIN_PRODUCT_OFF, size - JM_MAIN_HEAD_SIZE);
    return 1;
  }
  if (jm_8bitdo_image_product(data, size, product)) {
    printf("Telink firmware.img for %s: %zu bytes\n", product, size);
    return 1;
  }
  if (jm_8bitdo_image_check(data, size)) {
    printf("Telink image: %zu bytes\n", size);
    return 1;
  }
  uint8_t *work = flash ? malloc(flash_size ? flash_size : 1) : NULL;
  int ok = work && !jm_jieli_image_open(&image, flash, flash_size, work, flash_size);
  if (ok) jieli_listing(&image, flash, work);
  free(work);
  return ok;
}

/* ---- package BUILD OUT ------------------------------------------------------------------------
 * Every product built in BUILD (its board's directory) with the Jieli SDK's
 * RAM loaders: a folder (manifest.txt and the files), or a zip when OUT ends
 * in .zip (stored entries, the manifest first). */

struct entry { char path[96]; uint8_t *data; size_t size; };
struct zip { FILE *f; uint8_t *central; size_t central_size; unsigned count; long at; };

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xffffffff;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = c >> 1 ^ (c & 1 ? 0xedb88320u : 0);
  }
  return ~c;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff), put16(p + 2, v >> 16); }

/* A stored entry: its local header and data, its central record kept. */
static void zip_add(struct zip *z, const char *name, const uint8_t *data, size_t size) {
  uint8_t h[30] = {0x50, 0x4b, 3, 4, 10}, c[46] = {0x50, 0x4b, 1, 2, 0x1e, 3, 10};
  size_t n = strlen(name);
  uint32_t crc = crc32(data, size);
  put16(h + 12, 0x21);                         /* 1980-01-01, as reproducible */
  put32(h + 14, crc), put32(h + 18, (uint32_t)size), put32(h + 22, (uint32_t)size), put16(h + 26, (unsigned)n);
  memcpy(c + 12, h + 10, 16);
  put16(c + 28, (unsigned)n), put32(c + 38, 0100644u << 16), put32(c + 42, (uint32_t)z->at);
  if (fwrite(h, 1, 30, z->f) != 30 || fwrite(name, 1, n, z->f) != n || fwrite(data, 1, size, z->f) != size)
    die("cannot write the zip");
  z->at += (long)(30 + n + size);
  z->central = realloc(z->central, z->central_size + 46 + n);
  if (!z->central) die("out of memory");
  memcpy(z->central + z->central_size, c, 46), memcpy(z->central + z->central_size + 46, name, n);
  z->central_size += 46 + n, z->count++;
}

static void zip_end(struct zip *z) {
  uint8_t e[22] = {0x50, 0x4b, 5, 6};
  put16(e + 8, z->count), put16(e + 10, z->count);
  put32(e + 12, (uint32_t)z->central_size), put32(e + 16, (uint32_t)z->at);
  if (fwrite(z->central, 1, z->central_size, z->f) != z->central_size || fwrite(e, 1, 22, z->f) != 22 || fclose(z->f))
    die("cannot write the zip");
  free(z->central);
}

int cmd_package(int argc, char **argv) {
  const char *sdk = "external/jieli-fw-AC63_BT_SDK", *build = NULL, *out = NULL;
  struct entry files[JM_FIRMWARE_FILES];
  char manifest[JM_FIRMWARE_FILES * 200] = "jollymaam-firmware 2\n", path[1024];
  unsigned n = 0, chips = 0;
  const struct jm_product *p;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--sdk")) sdk = arg(argc, argv, &i);
    else if (argv[i][0] != '-' && !build) build = argv[i];
    else if (argv[i][0] != '-' && !out) out = argv[i];
    else die("package: unknown argument %s", argv[i]);
  }
  if (!build || !out || strlen(build) > 400 || strlen(out) > 400)
    die("usage: jollymaam package [--sdk JIELI_SDK] BUILD_DIR OUT[.zip]");
  for (size_t i = 0; (p = jm_product_at(i)); i++) {
    if (!p->board) continue;
    for (int role = 0; role < 2; role++) {
      const char *file = role ? (p->update ? "update.bin" : NULL) : p->install;
      FILE *f;
      if (!file) continue;
      snprintf(path, sizeof path, "%s/%s/%s", build, p->board, file);
      if (!(f = fopen(path, "rb"))) {
        if (!role) printf("package: %s not built, left out\n", p->board);
        break;
      }
      fclose(f);
      snprintf(files[n].path, sizeof files[n].path, "%s/%s", p->board, file);
      files[n].data = read_file(path, &files[n].size);
      size_t m = strlen(manifest);
      if (!jm_firmware_line(manifest + m, sizeof manifest - m, p->pid, role ? "update" : "install", files[n].path,
                            files[n].data, files[n].size))
        die("%s: does not fit the manifest", files[n].path);
      n++;
      if (p->chip == JM_CHIP_BR23 || p->chip == JM_CHIP_BD19) chips |= 1u << p->chip;
    }
  }
  for (int chip = JM_CHIP_BR23; chip <= JM_CHIP_BD19; chip++) {
    if (!(chips & 1u << chip)) continue;
    char lower[8];
    for (int k = 0; k < 5; k++) lower[k] = (char)(jm_chip_name(chip)[k] | (k < 4 ? 0x20 : 0));
    snprintf(path, sizeof path, "%s/cpu/%s/tools/%sloader.bin", sdk, lower, lower);
    snprintf(files[n].path, sizeof files[n].path, "loaders/%sloader.bin", lower);
    files[n].data = read_file(path, &files[n].size);
    size_t m = strlen(manifest);
    jm_firmware_line(manifest + m, sizeof manifest - m, jm_chip_name(chip), "loader", files[n].path, files[n].data,
                     files[n].size);
    n++;
  }
  if (!n) die("%s: nothing built", build);
  size_t len = strlen(out);
  if (len > 4 && !strcmp(out + len - 4, ".zip")) {
    struct zip z = {fopen(out, "wb"), NULL, 0, 0, 0};
    if (!z.f) die("%s: %s", out, strerror(errno));
    zip_add(&z, "manifest.txt", (const uint8_t *)manifest, strlen(manifest));
    for (unsigned i = 0; i < n; i++) zip_add(&z, files[i].path, files[i].data, files[i].size);
    zip_end(&z);
  } else {
    make_dir(out);
    for (unsigned i = 0; i < n; i++) {
      char *slash = strchr(files[i].path, '/');
      snprintf(path, sizeof path, "%.400s/%.*s", out, (int)(slash - files[i].path), files[i].path);
      make_dir(path);
      snprintf(path, sizeof path, "%.400s/%.95s", out, files[i].path);
      write_file(path, files[i].data, files[i].size);
    }
    snprintf(path, sizeof path, "%.400s/manifest.txt", out);
    write_file(path, manifest, strlen(manifest));
  }
  for (unsigned i = 0; i < n; i++) free(files[i].data);
  printf("package: %s, %u files\n", out, n);
  return 0;
}
