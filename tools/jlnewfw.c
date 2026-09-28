/*
 * jlnewfw - inspect, unpack, and build JieLi "new firmware" flash images.
 *
 * This intentionally has no dependencies beyond libc.  The pack command is
 * aimed at the BR23 SDK layout emitted by isd_download: uboot.boot, the
 * encoded SYS_CFG_PARAM stream, app files, and PRCT/VM/BTIF/key_mac reserved
 * regions.  See docs/image-formats.md for the on-flash layout.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define JLFS_HEADER_SIZE 32u
#define MAX_FILES 32u
#define MAX_CFG 128u

struct blob {
  uint8_t *p;
  size_t n, cap;
};
struct kv {
  char *section, *key, *value;
};
struct config {
  struct kv v[MAX_CFG];
  size_t n;
};
struct entry {
  uint16_t data_crc;
  uint32_t offset, size;
  uint8_t flags, reserved;
  uint16_t index;
  uint8_t name[16];
};
struct app_file {
  const char *path;
  const char *name;
  uint8_t *data;
  size_t size;
};

static void die(const char *s) {
  fprintf(stderr, "jlnewfw: %s\n", s);
  exit(1);
}
static void die_errno(const char *s) {
  fprintf(stderr, "jlnewfw: %s: %s\n", s, strerror(errno));
  exit(1);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static void wr16(uint8_t *p, uint16_t x) {
  p[0] = x;
  p[1] = x >> 8;
}
static void wr32(uint8_t *p, uint32_t x) {
  p[0] = x;
  p[1] = x >> 8;
  p[2] = x >> 16;
  p[3] = x >> 24;
}
static uint32_t align_up(uint32_t x, uint32_t a) { return (x + a - 1) / a * a; }

static uint16_t crc16_seed(const uint8_t *p, size_t n, uint16_t crc) {
  while (n--) {
    crc ^= (uint16_t)*p++ << 8;
    for (unsigned i = 0; i < 8; i++)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
  }
  return crc;
}
static uint16_t crc16(const uint8_t *p, size_t n) {
  return crc16_seed(p, n, 0);
}

/* Symmetric JieLi ENC stream and the flash-controller block variant. */
static void jl_enc(uint8_t *p, size_t n, uint16_t key) {
  while (n--) {
    *p++ ^= key;
    key = (uint16_t)((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0));
  }
}
static void jl_sfc(uint8_t *image, uint32_t off, uint32_t n, uint32_t base,
                   uint16_t key) {
  for (uint32_t i = 0; i < n; i += 32) {
    uint32_t m = n - i < 32 ? n - i : 32;
    jl_enc(image + off + i, m, (uint16_t)(key ^ ((off + i - base) >> 2)));
  }
}

static void reserve(struct blob *b, size_t n) {
  if (n <= b->cap)
    return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < n)
    cap *= 2;
  b->p = realloc(b->p, cap);
  if (!b->p)
    die("out of memory");
  b->cap = cap;
}
static void append(struct blob *b, const void *p, size_t n) {
  reserve(b, b->n + n);
  memcpy(b->p + b->n, p, n);
  b->n += n;
}
static void append_u8(struct blob *b, uint8_t x) { append(b, &x, 1); }
static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    die_errno(path);
  if (fseek(f, 0, SEEK_END) ||
      (*size = (size_t)ftell(f), fseek(f, 0, SEEK_SET)))
    die_errno(path);
  uint8_t *p = malloc(*size ? *size : 1);
  if (!p)
    die("out of memory");
  if (*size && fread(p, 1, *size, f) != *size)
    die_errno(path);
  fclose(f);
  return p;
}
static void write_file(const char *path, const void *p, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f)
    die_errno(path);
  if (n && fwrite(p, 1, n, f) != n)
    die_errno(path);
  if (fclose(f))
    die_errno(path);
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = 0;
  return s;
}
static void upper(char *s) {
  for (; *s; s++)
    *s = (char)toupper((unsigned char)*s);
}
static void load_config(struct config *c, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    die_errno(path);
  char line[1024], section[80] = "";
  while (fgets(line, sizeof line, f)) {
    char *comment = strstr(line, "//");
    if (comment)
      *comment = 0;
    char *s = trim(line);
    if (!*s || *s == '#')
      continue;
    if (*s == '[') {
      char *e = strchr(s, ']');
      if (!e)
        die("bad INI section");
      *e = 0;
      snprintf(section, sizeof section, "%s", s + 1);
      upper(section);
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = trim(s), *value = trim(eq + 1);
    char *semi = strrchr(value, ';');
    if (semi)
      *semi = 0;
    value = trim(value);
    if (c->n == MAX_CFG)
      die("too many INI values");
    upper(key);
    c->v[c->n++] = (struct kv){strdup(section), strdup(key), strdup(value)};
  }
  fclose(f);
}
static const char *cfg(const struct config *c, const char *sec, const char *key,
                       const char *def) {
  for (size_t i = 0; i < c->n; i++)
    if (!strcmp(c->v[i].section, sec) && !strcmp(c->v[i].key, key))
      return c->v[i].value;
  return def;
}
static void require_value(const struct config *c, const char *section,
                          const char *key, const char *expected) {
  const char *actual = cfg(c, section, key, expected);
  if (strcasecmp(actual, expected)) {
    fprintf(stderr, "jlnewfw: unsupported %s.%s=%s (expected %s)\n", section,
            key, actual, expected);
    exit(1);
  }
}
static uint32_t number(const char *s) {
  char *end;
  unsigned long v = strtoul(s, &end, 0);
  while (isspace((unsigned char)*end))
    end++;
  if (*end == 'K' || *end == 'k') {
    v *= 1024;
    end++;
  } else if (*end == 'M' || *end == 'm') {
    v *= 1024 * 1024;
    end++;
  }
  if (*end) {
    fprintf(stderr, "jlnewfw: not a number: %s\n", s);
    exit(1);
  }
  if (v > UINT32_MAX)
    die("number too large");
  return (uint32_t)v;
}
static bool numeric(const char *s) {
  char *e;
  (void)strtoul(s, &e, 0);
  while (isspace((unsigned char)*e))
    e++;
  return *s && !*e;
}

static void make_chipkey(struct blob *b, uint16_t key) {
  uint8_t x[32];
  memset(x, 8, 16); /* sum=128 */
  for (unsigned i = 0; i < 16; i++)
    x[16 + i] = (uint8_t)(8 ^ ((key & (1u << i)) ? 0 : 0x80));
  append(b, x, 32);
  uint8_t c[2];
  wr16(c, crc16(x, 32));
  append(b, c, 2);
}
static struct blob encode_syscfg(const struct config *c, uint16_t chipkey) {
  struct blob b = {0};
  make_chipkey(&b, chipkey);
  for (size_t i = 0; i < c->n; i++)
    if (!strcmp(c->v[i].section, "SYS_CFG_PARAM")) {
      char key[256], val[512];
      snprintf(key, sizeof key, "%s", c->v[i].key);
      snprintf(val, sizeof val, "%s", c->v[i].value);
      upper(val);
      if (numeric(val)) {
        append_u8(&b, 4);
        append(&b, key, strlen(key) + 1);
        uint8_t n[4];
        wr32(n, number(val));
        append(&b, n, 4);
      } else {
        size_t n = strlen(val);
        if (n > 255)
          die("SYS_CFG_PARAM string too long");
        append_u8(&b, (uint8_t)n);
        append(&b, key, strlen(key) + 1);
        append(&b, val, n);
      }
    }
  return b;
}

static void set_name(uint8_t name[16], const char *s) {
  memset(name, 0xff, 16);
  size_t n = strlen(s);
  if (n > 15)
    n = 15;
  memcpy(name, s, n);
  name[n] = 0;
}
static void encode_entry(uint8_t out[32], const struct entry *e) {
  memset(out, 0xff, 32);
  wr16(out + 2, e->data_crc);
  wr32(out + 4, e->offset);
  wr32(out + 8, e->size);
  out[12] = e->flags;
  out[13] = e->reserved;
  wr16(out + 14, e->index);
  memcpy(out + 16, e->name, 16);
  wr16(out, crc16(out + 2, 30));
}
static bool decode_entry(const uint8_t *p, struct entry *e) {
  if (crc16(p + 2, 30) != rd16(p))
    return false;
  e->data_crc = rd16(p + 2);
  e->offset = rd32(p + 4);
  e->size = rd32(p + 8);
  e->flags = p[12];
  e->reserved = p[13];
  e->index = rd16(p + 14);
  memcpy(e->name, p + 16, 16);
  return true;
}
static void scramble_bankcb(uint8_t *p, size_t n) {
  if (n < 16)
    die("uboot.boot is not BANKCB");
  unsigned count = rd16(p);
  if (!count || count > 256 || (size_t)count * 16 > n)
    die("bad BANKCB count");
  for (unsigned i = 0; i < count; i++) {
    uint8_t *h = p + i * 16;
    uint32_t off = rd32(h + 8);
    uint16_t size = rd16(h + 2);
    if ((uint64_t)off + size > n || crc16(h, 14) != rd16(h + 14))
      die("invalid BANKCB header");
    jl_enc(p + off, size, 0xffff);
    jl_enc(h, 16, 0xffff);
  }
}
static uint32_t next_pow2(uint32_t x) {
  uint32_t p = 1;
  while (p < x && p < 0x80000000u)
    p <<= 1;
  return p;
}
static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static void flash_header(uint8_t out[32], const struct config *c,
                         uint32_t flash_size) {
  memset(out, 0xff, 32);
  const char *vid = cfg(c, "EXTRA_CFG_PARAM", "VID", "0.01"),
             *pid = cfg(c, "EXTRA_CFG_PARAM", "PID", "JL_DEVICE");
  memcpy(out + 4, vid, strlen(vid) > 4 ? 4 : strlen(vid));
  memcpy(out + 16, pid, strlen(pid) > 16 ? 16 : strlen(pid));
  jl_enc(out, 32, 0xffff); /* enter the header's CRC domain */
  wr16(out + 2, 544);
  wr32(out + 8, flash_size);
  out[12] = 2;
  out[13] = 16;
  uint32_t special = number(cfg(c, "EXTRA_CFG_PARAM", "SPECIAL_OPT", "0"));
  out[15] = special ? (uint8_t)special : 0xff;
  wr16(out, crc16(out + 2, 30));
  jl_enc(out, 32, 0xffff);
}

static void cmd_pack(int argc, char **argv) {
  const char *ini = NULL, *uboot_path = NULL, *output = NULL;
  uint16_t chipkey = 0xffff;
  uint32_t requested_flash_size = 0;
  uint32_t requested_code_limit = 0;
  struct app_file files[MAX_FILES];
  size_t nf = 0;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "-c") && ++i < argc)
      ini = argv[i];
    else if (!strcmp(argv[i], "-u") && ++i < argc)
      uboot_path = argv[i];
    else if (!strcmp(argv[i], "-o") && ++i < argc)
      output = argv[i];
    else if (!strcmp(argv[i], "-k") && ++i < argc)
      chipkey = (uint16_t)number(argv[i]);
    else if (!strcmp(argv[i], "--flash-size") && ++i < argc)
      requested_flash_size = number(argv[i]);
    else if (!strcmp(argv[i], "--code-limit") && ++i < argc)
      requested_code_limit = number(argv[i]);
    else if (argv[i][0] == '-')
      die("unknown pack option");
    else {
      if (nf == MAX_FILES)
        die("too many app files");
      files[nf++] = (struct app_file){argv[i], base_name(argv[i]), NULL, 0};
    }
  }
  if (!ini || !uboot_path || !output || !nf)
    die("usage: jlnewfw pack [--flash-size SIZE] [--code-limit ADDRESS] "
        "-c isd_config.ini "
        "-u uboot.boot -o jl_isd.bin app.bin [cfg_tool.bin ...]");
  struct config c = {0};
  load_config(&c, ini);
  /* These are the layout modes understood below. Refuse rather than silently
     producing a plausible-looking image for a different SDK configuration. */
  require_value(&c, "EXTRA_CFG_PARAM", "NEW_FLASH_FS", "YES");
  require_value(&c, "EXTRA_CFG_PARAM", "FORCE_4K_ALIGN", "YES");
  require_value(&c, "EXTRA_CFG_PARAM", "RESERVED_OPT", "0");
  require_value(&c, "SYS_CFG_PARAM", "EOFFSET", "0");
  require_value(&c, "RESERVED_CONFIG", "PRCT_ADR", "0");
  require_value(&c, "RESERVED_CONFIG", "PRCT_LEN", "CODE_LEN");
  require_value(&c, "RESERVED_CONFIG", "PRCT_OPT", "2");
  require_value(&c, "RESERVED_CONFIG", "VM_ADR", "0");
  require_value(&c, "RESERVED_CONFIG", "VM_OPT", "1");
  require_value(&c, "RESERVED_CONFIG", "BTIF_ADR", "AUTO");
  require_value(&c, "RESERVED_CONFIG", "BTIF_OPT", "1");
  if (strcmp(files[0].name, "app.bin"))
    die("the first application file must be named app.bin");
  for (size_t i = 0; i < nf; i++)
    if (strlen(files[i].name) > 15)
      die("JLFS file names may be at most 15 bytes");
  struct blob sys = encode_syscfg(&c, chipkey);
  size_t uboot_n;
  uint8_t *uboot = read_file(uboot_path, &uboot_n);
  for (size_t i = 0; i < nf; i++)
    files[i].data = read_file(files[i].path, &files[i].size);
  uint32_t appbase =
      align_up((uint32_t)(32 + 4 * 32 + uboot_n + sys.n), 0x1000);
  uint32_t entry = number(cfg(&c, "EXTRA_CFG_PARAM", "ENTRY", "0x1e00120"));
  uint32_t appoff = entry & 0xfffffu;
  size_t nr = 3, nested_n = nf + nr;
  uint32_t header_end = 32 + (uint32_t)nested_n * 32;
  if (appoff < header_end)
    die("ENTRY's flash offset overlaps the app-area headers");
  uint32_t cursor = appoff;
  for (size_t i = 0; i < nf; i++) {
    if (files[i].size > UINT32_MAX - cursor)
      die("app files too large");
    cursor += (uint32_t)files[i].size;
  }
  uint32_t outer_size =
      cursor; /* cursor is already relative to the outer header */
  uint32_t image_end = align_up(appbase + outer_size, 0x1000);
  uint32_t code_end = requested_code_limit ? requested_code_limit : image_end;
  if ((code_end & 0xfff) || code_end < image_end)
    die("code limit must be 4-KiB aligned and contain the application");
  uint32_t vm_len = number(cfg(&c, "RESERVED_CONFIG", "VM_LEN", "8K"));
  uint32_t btif_len = number(cfg(&c, "RESERVED_CONFIG", "BTIF_LEN", "0x1000"));
  uint32_t required = code_end + vm_len + btif_len + 0x1000;
  uint32_t total = requested_flash_size ? requested_flash_size
                                        : next_pow2(required);
  if (!requested_flash_size && total < 0x20000)
    total = 0x20000;
  if (total < required)
    die("requested flash size is too small for code and reserved regions");
  if (total & (total - 1))
    die("flash size must be a power of two");
  uint32_t keymac = total - 0x1000, btif = keymac - btif_len, vm = code_end,
           flash_size = keymac;
  if (vm > btif)
    die("application does not fit reserved layout");
  struct blob image = {0};
  reserve(&image, image_end);
  image.n = image_end;
  memset(image.p, 0xff, image.n);
  flash_header(image.p, &c, flash_size);
  uint32_t top_data = 32 + 4 * 32, uboot_off = top_data,
           sys_off = uboot_off + (uint32_t)uboot_n;
  struct entry top[4] = {0};
  top[0] = (struct entry){
      crc16(uboot, uboot_n), uboot_off, (uint32_t)uboot_n, 0, 0, 0, {0}};
  set_name(top[0].name, "uboot.boot");
  top[1] = (struct entry){
      crc16(sys.p, sys.n), sys_off, (uint32_t)sys.n, 0x02, 0xff, 0, {0}};
  set_name(top[1].name, "isd_config.ini");
  top[2] = (struct entry){0xffff, appbase, 0xffffffff, 0x81, 0xff, 0, {0}};
  set_name(top[2].name, "app_dir_head");
  top[3] = (struct entry){0xffff, keymac, 0x1000, 0x12, 0x01, 0xffff, {0}};
  set_name(top[3].name, "key_mac");
  for (unsigned i = 0; i < 4; i++)
    encode_entry(image.p + 32 + i * 32, &top[i]);
  memcpy(image.p + uboot_off, uboot, uboot_n);
  memcpy(image.p + sys_off, sys.p, sys.n);
  struct entry outer = {0};
  outer.offset = entry;
  outer.size = outer_size;
  outer.flags = 0x83;
  outer.reserved = 0xff;
  outer.index = 1;
  set_name(outer.name, "app_area_head");
  cursor = appoff;
  for (size_t i = 0; i < nf; i++) {
    struct entry e = {crc16(files[i].data, files[i].size),
                      cursor,
                      (uint32_t)files[i].size,
                      0x82,
                      0xff,
                      0,
                      {0}};
    set_name(e.name, files[i].name);
    encode_entry(image.p + appbase + 32 + (uint32_t)i * 32, &e);
    memcpy(image.p + appbase + cursor, files[i].data, files[i].size);
    cursor += (uint32_t)files[i].size;
  }
  struct entry r[3] = {0};
  r[0] = (struct entry){0xffff, vm, btif - vm, 0x12, 0x81, 0, {0}};
  set_name(r[0].name, "VM");
  wr32(r[0].name + 12, vm_len);
  r[1] = (struct entry){0xffff, 0, code_end, 0x92, 0x82, 0, {0}};
  set_name(r[1].name, "PRCT");
  memcpy(r[1].name + 12, "CODE", 4);
  r[2] = (struct entry){0xffff, btif, btif_len, 0x92, 0x81, 1, {0}};
  set_name(r[2].name, "BTIF");
  memcpy(r[2].name + 12, "AUTO", 4);
  for (unsigned i = 0; i < 3; i++)
    encode_entry(image.p + appbase + 32 + (uint32_t)(nf + i) * 32, &r[i]);
  outer.data_crc = crc16(image.p + appbase + 32, outer_size - 32);
  encode_entry(image.p + appbase, &outer);
  scramble_bankcb(image.p + uboot_off, uboot_n);
  jl_sfc(image.p, appbase, outer_size, appbase, chipkey);
  for (unsigned i = 0; i < 4; i++)
    jl_enc(image.p + 32 + i * 32, 32, 0xffff);
  write_file(output, image.p, image.n);
  printf("wrote %s: %zu bytes, app @0x%x, image end 0x%x, code limit 0x%x, "
         "declared flash 0x%x (total 0x%x)\n",
         output, image.n, appbase, image_end, code_end, flash_size, total);
}

static void name_string(char out[17], const uint8_t raw[16]) {
  size_t i;
  for (i = 0; i < 16 && raw[i] && raw[i] != 0xff; i++)
    out[i] = (char)raw[i];
  out[i] = 0;
}
static uint32_t find_base(const uint8_t *p, size_t n, uint8_t clear[32]) {
  static const uint32_t candidates[] = {0,       0x1000,   0x10000,
                                        0x80000, 0x100000, 0x180000};
  for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
    uint32_t o = candidates[i];
    if ((uint64_t)o + 32 > n)
      continue;
    memcpy(clear, p + o, 32);
    jl_enc(clear, 32, 0xffff);
    if (rd16(clear) && crc16(clear + 2, 30) == rd16(clear))
      return o;
  }
  die("no valid new-firmware flash header found");
  return 0;
}
static void descramble_bankcb(uint8_t *p, size_t n) {
  unsigned count = 1;
  for (unsigned i = 0; i < count; i++) {
    if ((size_t)(i + 1) * 16 > n)
      die("truncated BANKCB header list");
    uint8_t *h = p + i * 16;
    jl_enc(h, 16, 0xffff);
    if (crc16(h, 14) != rd16(h + 14))
      die("invalid BANKCB header");
    if (!i) {
      count = rd16(h);
      if (!count || count > 256)
        die("bad BANKCB count");
    }
    uint32_t off = rd32(h + 8);
    uint16_t size = rd16(h + 2);
    if ((uint64_t)off + size > n)
      die("BANKCB data outside uboot.boot");
    jl_enc(p + off, size, 0xffff);
  }
}
static void ensure_dir(const char *p) {
  if (mkdir(p, 0777) && errno != EEXIST)
    die_errno(p);
}
static void path_join(char *out, size_t n, const char *dir, const char *name) {
  if (snprintf(out, n, "%s/%s", dir, name) >= (int)n)
    die("path too long");
}

/* Return a malloc'd raw flash image, unwrapping a UFW transport if present. */
static uint8_t *load_image(const char *path, size_t *size) {
  size_t file_size;
  uint8_t *file = read_file(path, &file_size);
  if (file_size < 0x40) {
    *size = file_size;
    return file;
  }

  uint8_t header[0x40];
  memcpy(header, file, sizeof header);
  jl_enc(header, sizeof header, 0xffff);
  if (crc16(header + 2, 62) != rd16(header)) {
    *size = file_size;
    return file;
  }

  uint16_t count = rd16(header + 8);
  uint32_t header_size = 0x40 + (uint32_t)count * 0x50;
  if (!count || header_size > file_size ||
      crc16(file + 0x40, header_size - 0x40) != rd16(header + 2)) {
    die("invalid UFW entry list");
  }

  for (uint16_t i = 0; i < count; i++) {
    uint8_t item[0x50];
    memcpy(item, file + 0x40 + i * 0x50, sizeof item);
    jl_enc(item, sizeof item, 0xffff);
    if (rd16(item) != 0)
      continue; /* type 0 is flash.bin */
    uint16_t data_crc = rd16(item + 4);
    uint32_t offset = rd32(item + 8), data_size = rd32(item + 12);
    if ((uint64_t)offset + data_size > file_size)
      die("UFW flash.bin is truncated");
    if (crc16(file + offset, data_size) != data_crc)
      die("UFW flash.bin CRC mismatch");
    uint8_t *image = malloc(data_size ? data_size : 1);
    if (!image)
      die("out of memory");
    memcpy(image, file + offset, data_size);
    free(file);
    *size = data_size;
    printf("UFW wrapper: extracted flash.bin (%u bytes)\n", data_size);
    return image;
  }
  die("UFW has no flash.bin entry");
  return NULL;
}

static void inspect(const char *path, const char *outdir, bool extract) {
  size_t n;
  uint8_t *image = load_image(path, &n), fh[32];
  uint32_t base = find_base(image, n, fh);
  /* VID and PID are deliberately left clear in the otherwise ENC header. */
  memcpy(fh + 4, image + base + 4, 4);
  memcpy(fh + 16, image + base + 16, 16);
  char vid[5] = {0}, pid[17] = {0};
  memcpy(vid, fh + 4, 4);
  memcpy(pid, fh + 16, 16);
  printf("base=0x%x burner=%u vid=%s flash_size=0x%x fs=%u align=%u "
         "special=0x%02x pid=%s\n",
         base, rd16(fh + 2), vid, rd32(fh + 8), fh[12], fh[13], fh[15], pid);
  if (extract)
    ensure_dir(outdir);
  struct entry top[MAX_FILES];
  size_t nt = 0;
  uint32_t appbase = 0;
  uint16_t chipkey = 0xffff;
  for (uint32_t off = base + 32;; off += 32) {
    if ((uint64_t)off + 32 > n || nt == MAX_FILES)
      die("invalid top-level list");
    uint8_t h[32];
    memcpy(h, image + off, 32);
    jl_enc(h, 32, 0xffff);
    if (!decode_entry(h, &top[nt]))
      die("top-level header CRC mismatch");
    char name[17];
    name_string(name, top[nt].name);
    printf("top %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n", name,
           top[nt].offset, top[nt].size, top[nt].flags, top[nt].reserved,
           top[nt].index);
    if (!strcmp(name, "app_dir_head"))
      appbase = base + top[nt].offset;
    if (!(top[nt].flags & 0x10) && top[nt].flags != 0x81) {
      if ((uint64_t)base + top[nt].offset + top[nt].size > n)
        die("top-level file is truncated");
      uint8_t *d = malloc(top[nt].size ? top[nt].size : 1);
      if (!d)
        die("out of memory");
      memcpy(d, image + base + top[nt].offset, top[nt].size);
      if (!strcmp(name, "uboot.boot"))
        descramble_bankcb(d, top[nt].size);
      if (crc16(d, top[nt].size) != top[nt].data_crc)
        die("top-level file data CRC mismatch");
      if (extract) {
        char dst[1024];
        path_join(dst, sizeof dst, outdir, name);
        write_file(dst, d, top[nt].size);
      }
      free(d);
    }
    bool last = top[nt].index != 0;
    nt++;
    if (last)
      break;
  }
  if (!appbase || appbase + 32 > n)
    die("app_dir_head points outside image");
  /* Extract chip key from deterministic/normal chipkey blob. */
  for (size_t i = 0; i < nt; i++) {
    char name[17];
    name_string(name, top[i].name);
    if (!strcmp(name, "isd_config.ini") && top[i].size >= 34) {
      const uint8_t *d = image + base + top[i].offset;
      if (crc16(d, 32) != rd16(d + 32))
        die("chip-key record CRC mismatch");
      uint8_t sum = 0;
      for (unsigned j = 0; j < 16; j++)
        sum += d[j];
      uint8_t t = sum >= 0xe0 ? 0xaa : sum <= 0x10 ? 0x55 : sum;
      chipkey = 0;
      for (unsigned j = 0; j < 16; j++)
        if ((uint8_t)(d[16 + j] ^ d[15 - j]) < t)
          chipkey |= 1u << j;
    }
  }
  uint8_t oh[32];
  memcpy(oh, image + appbase, 32);
  jl_sfc(oh, 0, 32, 0, chipkey);
  struct entry outer;
  if (!decode_entry(oh, &outer))
    die("application-area header CRC mismatch");
  char on[17];
  name_string(on, outer.name);
  printf("app  %-16s entry=0x%08x size=0x%x key=0x%04x\n", on, outer.offset,
         outer.size, chipkey);
  if ((uint64_t)appbase + outer.size > n)
    die("application area is truncated");
  uint8_t *plain = malloc(outer.size);
  memcpy(plain, image + appbase, outer.size);
  jl_sfc(plain, 0, outer.size, 0, chipkey);
  if (crc16(plain + 32, outer.size - 32) != outer.data_crc)
    die("application-area data CRC mismatch");
  for (uint32_t off = 32;; off += 32) {
    struct entry e;
    if (off + 32 > outer.size || !decode_entry(plain + off, &e))
      die("nested header CRC mismatch");
    char name[17];
    name_string(name, e.name);
    printf("  %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n", name,
           e.offset, e.size, e.flags, e.reserved, e.index);
    if (e.flags == 0x82) {
      if ((uint64_t)e.offset + e.size > outer.size)
        die("nested file outside app area");
      if (crc16(plain + e.offset, e.size) != e.data_crc)
        die("nested file data CRC mismatch");
      if (extract) {
        char dst[1024];
        path_join(dst, sizeof dst, outdir, name);
        write_file(dst, plain + e.offset, e.size);
      }
    }
    if (e.index)
      break;
  }
  free(plain);
  free(image);
}

static void usage(void) {
  puts("usage:\n  jlnewfw list IMAGE\n  jlnewfw verify IMAGE\n  jlnewfw unpack "
       "IMAGE DIRECTORY\n  jlnewfw pack [--flash-size SIZE] "
       "[--code-limit ADDRESS] -c isd_config.ini "
       "-u uboot.boot -o jl_isd.bin app.bin [cfg_tool.bin ...]");
}
int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  if (!strcmp(argv[1], "pack")) {
    cmd_pack(argc - 2, argv + 2);
    return 0;
  }
  if (!strcmp(argv[1], "list") && argc == 3) {
    inspect(argv[2], NULL, false);
    return 0;
  }
  if (!strcmp(argv[1], "verify") && argc == 3) {
    inspect(argv[2], NULL, false);
    puts("verification OK");
    return 0;
  }
  if (!strcmp(argv[1], "unpack") && argc == 4) {
    inspect(argv[2], argv[3], true);
    return 0;
  }
  usage();
  return 2;
}
