/*
 * jlnewfw - inspect, unpack, and build JieLi "new firmware" flash images.
 *
 * This intentionally has no dependencies beyond libc.  The pack command is
 * aimed at the BR23 SDK layout emitted by isd_download: uboot.boot, the
 * encoded SYS_CFG_PARAM stream, app files, and PRCT/VM/BTIF/key_mac reserved
 * regions.  See docs/jieli/re-application-image.md for the on-flash layout.
 */
#define _XOPEN_SOURCE 700
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
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32be(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
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

/* A chip-key record made from nothing (when the INI carries none). */
static void make_chipkey(uint8_t x[34], uint16_t key) {
  memset(x, 8, 16); /* sum=128 */
  for (unsigned i = 0; i < 16; i++)
    x[16 + i] = (uint8_t)(8 ^ ((key & (1u << i)) ? 0 : 0x80));
  wr16(x + 32, crc16(x, 32));
}
static struct blob encode_syscfg(const struct config *c,
                                 const uint8_t record[34]) {
  struct blob b = {0};
  append(&b, record, 34);
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
  out[12] = (uint8_t)number(cfg(c, "JLNEWFW", "HEADER_FS", "2")); /* BD19: 11 */
  out[13] = 16;
  uint32_t special = number(cfg(c, "EXTRA_CFG_PARAM", "SPECIAL_OPT", "0"));
  out[15] = special ? (uint8_t)special : 0xff;
  const char *raw = cfg(c, "JLNEWFW", "HEADER_SPECIAL", NULL);
  if (raw)
    out[15] = (uint8_t)number(raw);
  wr16(out, crc16(out + 2, 30));
  jl_enc(out, 32, 0xffff);
}

static void name_string(char out[17], const uint8_t raw[16]) {
  size_t i;
  for (i = 0; i < 16 && raw[i] && raw[i] != 0xff; i++)
    out[i] = (char)raw[i];
  out[i] = 0;
}
static bool header_at(const uint8_t *p, size_t n, uint32_t o, uint8_t clear[32]) {
  if ((uint64_t)o + 32 > n)
    return false;
  memcpy(clear, p + o, 32);
  jl_enc(clear, 32, 0xffff);
  return rd16(clear) && crc16(clear + 2, 30) == rd16(clear);
}
/* EOFFSET from the isd_config.ini of the image at `o`, in 4 KiB units: where
 * the boot code takes the image from.  0 when there is none. */
static uint32_t eoffset(const uint8_t *p, size_t n, uint32_t o) {
  for (uint32_t off = o + 32; (uint64_t)off + 32 <= n && off < o + 0x400; off += 32) {
    uint8_t h[32];
    struct entry e;
    memcpy(h, p + off, 32);
    jl_enc(h, 32, 0xffff);
    if (!decode_entry(h, &e))
      return 0;
    char name[17];
    name_string(name, e.name);
    if (!strcmp(name, "isd_config.ini") && (uint64_t)o + e.offset + e.size <= n) {
      const uint8_t *d = p + o + e.offset;
      for (uint32_t i = 0; i + 12 <= e.size; i++)
        if (!memcmp(d + i, "EOFFSET", 8))
          return rd32(d + i + 8);
      return 0;
    }
    if (e.index)
      return 0;
  }
  return 0;
}
/* The first valid header, or, when its EOFFSET points at another valid one,
 * that: an upgraded dongle keeps stale copies at 0 and 0x1000 and boots the
 * image at 0x10000 (EOFFSET 16). */
static uint32_t find_base(const uint8_t *p, size_t n, uint8_t clear[32]) {
  static const uint32_t candidates[] = {0,       0x1000,   0x10000,
                                        0x80000, 0x100000, 0x180000};
  for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
    uint32_t o = candidates[i];
    if (!header_at(p, n, o, clear))
      continue;
    uint32_t e = eoffset(p, n, o) * 0x1000;
    if (e && e != o && e < n && header_at(p, n, e, clear))
      return e;
    header_at(p, n, o, clear);
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
#ifdef _WIN32
  if (mkdir(p) && errno != EEXIST)
#else
  if (mkdir(p, 0777) && errno != EEXIST)
#endif
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

/* ---- the chip-key record --------------------------------------------------
 * The first 34 bytes of isd_config.ini: 32 bytes and their CRC.  Bit j of the
 * key is set when byte 16+j XOR byte 15-j is below a threshold taken from
 * the sum of the first 16 bytes. */
static uint8_t record_threshold(const uint8_t *d) {
  uint8_t sum = 0;
  for (unsigned j = 0; j < 16; j++)
    sum += d[j];
  return sum >= 0xe0 ? 0xaa : sum <= 0x10 ? 0x55 : sum;
}
static uint16_t record_key(const uint8_t *d) {
  if (crc16(d, 32) != rd16(d + 32))
    die("chip-key record CRC mismatch");
  uint8_t t = record_threshold(d);
  uint16_t key = 0;
  for (unsigned j = 0; j < 16; j++)
    if ((uint8_t)(d[16 + j] ^ d[15 - j]) < t)
      key |= (uint16_t)(1u << j);
  return key;
}
/* Only the bytes whose bit must change are changed, to some value on the
 * right side of the threshold. */
static void set_record_key(uint8_t *d, uint16_t key) {
  uint8_t t = record_threshold(d);
  for (unsigned j = 0; j < 16; j++) {
    uint8_t v = d[16 + j] ^ d[15 - j];
    bool want = key >> j & 1;
    if ((v < t) != want)
      d[16 + j] = d[15 - j] ^ (uint8_t)(want ? v % t : t + v % (256 - t));
  }
  wr16(d + 32, crc16(d, 32));
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
  for (char *q = pid; *q; q++)
    if ((uint8_t)*q == 0xff)
      *q = 0;
  printf("base=0x%x burner=%u vid=%s flash_size=0x%x fs=%u align=%u "
         "special=0x%02x pid=%s eoffset=%u\n",
         base, rd16(fh + 2), vid, rd32(fh + 8), fh[12], fh[13], fh[15], pid,
         eoffset(image, n, base));
  if (extract)
    ensure_dir(outdir);
  struct entry top[MAX_FILES];
  size_t nt = 0;
  uint32_t appbase = 0, flash_end = 0;
  uint16_t chipkey = 0xffff, mark = 0xffff;
  const uint8_t *sys = NULL;
  uint32_t sys_n = 0;
  char uboot_name[17] = "";
  uint8_t top_uboot_flags = 0;
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
      appbase = base + top[nt].offset, mark = top[nt].data_crc;
    if (!strcmp(name, "key_mac"))
      flash_end = top[nt].offset + top[nt].size;
    if (!strcmp(name, "isd_config.ini") && (uint64_t)base + top[nt].offset +
                                                   top[nt].size <= n)
      sys = image + base + top[nt].offset, sys_n = top[nt].size;
    if (!strcmp(name, "uboot.boot"))
      snprintf(uboot_name, sizeof uboot_name, "%s", name), top_uboot_flags = top[nt].flags;
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
      if (extract && strcmp(name, "isd_config.ini")) { /* that one as text */
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
    if (!strcmp(name, "isd_config.ini") && top[i].size >= 34)
      chipkey = record_key(image + base + top[i].offset);
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
  /* All that follows, as resources may come after the app area. */
  uint32_t plain_n = (uint32_t)(n - appbase);
  uint8_t *plain = malloc(plain_n);
  if (!plain)
    die("out of memory");
  memcpy(plain, image + appbase, plain_n);
  jl_sfc(plain, 0, plain_n, 0, chipkey);
  if (crc16(plain + 32, outer.size - 32) != outer.data_crc)
    die("application-area data CRC mismatch");
  char files[MAX_FILES][17], res[MAX_FILES][17];
  size_t nfiles = 0, nres = 0;
  struct entry areas[MAX_FILES]; /* the reserved list, as it is */
  size_t nareas = 0;
  uint32_t code = 0, vm_len = 0, btif_len = 0, vm_at = 0;
  for (uint32_t off = 32;; off += 32) {
    struct entry e;
    if (off + 32 > outer.size || !decode_entry(plain + off, &e))
      die("nested header CRC mismatch");
    char name[17];
    name_string(name, e.name);
    printf("  %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n", name,
           e.offset, e.size, e.flags, e.reserved, e.index);
    if (!strcmp(name, "PRCT"))
      code = e.size;
    if (!strcmp(name, "VM")) {
      vm_len = rd32(e.name + 12);
      vm_at = e.offset;
    }
    if (!strcmp(name, "BTIF"))
      btif_len = e.size;
    if (e.flags == 0x82 && nfiles < MAX_FILES)
      memcpy(files[nfiles++], name, 17);
    if (e.flags != 0x82 && nareas < MAX_FILES)
      areas[nareas++] = e;
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
  /* Resources: areas after the app area, each a header (contents from its
   * offset field on, size including the header, CRC over the contents) and
   * one file, until one says last.  Flags 0x83 (resources) or 0x82 (the
   * BD19's p11_code.bin); "name:82" in RES keeps the latter. */
  uint8_t res_flags[MAX_FILES];
  for (uint32_t off = outer.size; !outer.index && off + 32 <= plain_n;) {
    struct entry e;
    if (!decode_entry(plain + off, &e) || (e.flags != 0x83 && e.flags != 0x82))
      die("resource header CRC mismatch");
    char name[17];
    name_string(name, e.name);
    printf("res  %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n", name,
           off, e.size, e.flags, e.reserved, e.index);
    if (e.offset < 32 || e.size < e.offset || (uint64_t)off + e.size > plain_n ||
        crc16(plain + off + e.offset, e.size - e.offset) != e.data_crc)
      die("resource data CRC mismatch");
    if (extract) {
      char dst[1024];
      path_join(dst, sizeof dst, outdir, name);
      write_file(dst, plain + off + e.offset, e.size - e.offset);
    }
    if (nres < MAX_FILES) {
      res_flags[nres] = e.flags;
      memcpy(res[nres++], name, 17);
    }
    if (e.index)
      break;
    off += e.size;
  }
  if (extract) {
    char dst[1024];
    path_join(dst, sizeof dst, outdir, "isd_config.ini");
    FILE *f = fopen(dst, "w");
    if (!f)
      die_errno(dst);
    fprintf(f, "// From %s by jlnewfw unpack: jlnewfw pack -c this_file -o OUT\n"
               "// rebuilds it ([JLNEWFW] holds what the SDK INI cannot say).\n",
            base_name(path));
    fprintf(f, "[EXTRA_CFG_PARAM]\nNEW_FLASH_FS = YES;\nFORCE_4K_ALIGN = YES;\n"
               "SPECIAL_OPT = 0;\nPID = %s;\nVID = %s;\nENTRY = 0x%x;\n"
               "RESERVED_OPT = 0;\n[SYS_CFG_PARAM]\n",
            pid, vid, outer.offset);
    /* After the 34-byte chip-key record: a length (4 for a number), the
     * name, the value.  A 4-character string and a number look the same;
     * printable is taken for a string. */
    for (uint32_t i = 34; sys && i < sys_n;) {
      uint8_t len = sys[i++];
      const char *key = (const char *)sys + i;
      size_t kn = strnlen(key, sys_n - i);
      i += (uint32_t)kn + 1;
      if (i + len > sys_n)
        die("isd_config.ini stream is truncated");
      bool text = true;
      for (unsigned j = 0; j < len; j++)
        if (!isalnum(sys[i + j]) && sys[i + j] != '_')
          text = false;
      if (len == 4 && !text)
        fprintf(f, "%.*s = %u;\n", (int)kn, key, rd32(sys + i));
      else
        fprintf(f, "%.*s = %.*s;\n", (int)kn, key, len, sys + i);
      i += len;
    }
    fprintf(f, "[RESERVED_CONFIG]\nPRCT_ADR = 0;\nPRCT_LEN = CODE_LEN;\n"
               "PRCT_OPT = 2;\nVM_ADR = 0;\nVM_LEN = 0x%x;\nVM_OPT = 1;\n"
               "BTIF_ADR = AUTO;\nBTIF_LEN = 0x%x;\nBTIF_OPT = 1;\n[JLNEWFW]\n",
            vm_len, btif_len);
    fprintf(f, "UBOOT = %s;\nUBOOT_FLAGS = 0x%02x;\nHEADER_FS = %u;\nAREAS = ", uboot_name,
            top_uboot_flags, fh[12]);
    for (size_t i = 0; i < nareas; i++) {
      fprintf(f, "%s0x%x:0x%x:0x%02x:0x%02x:0x%04x:", i ? ", " : "", areas[i].offset,
              areas[i].size, areas[i].flags, areas[i].reserved, areas[i].data_crc);
      for (unsigned k = 0; k < 16; k++)
        fprintf(f, "%02x", areas[i].name[k]);
    }
    fprintf(f, ";\nFILES = ");
    for (size_t i = 0; i < nfiles; i++)
      fprintf(f, "%s%s", i ? ", " : "", files[i]);
    fprintf(f, ";\n");
    if (nres) {
      fprintf(f, "RES = ");
      for (size_t i = 0; i < nres; i++)
        fprintf(f, "%s%s%s", i ? ", " : "", res[i], res_flags[i] == 0x82 ? ":82" : "");
      fprintf(f, ";\n");
    }
    if (!code && vm_at > base) /* no PRCT entry (BD19): the code ends where VM starts */
      code = vm_at - base;
    fprintf(f, "FLASH_SIZE = 0x%x;\nCODE_LIMIT = 0x%x;\nHEADER_SPECIAL = 0x%02x;\n"
               "CHIP_KEY = 0x%04x;\nCHIP_KEY_RECORD = ",
            flash_end, code, fh[15], chipkey);
    for (unsigned i = 0; sys && i < 34; i++)
      fprintf(f, "%02x", sys[i]);
    fprintf(f, ";\n");
    if (mark != 0xffff)
      fprintf(f, "MARK = 0x%04x;\n", mark);
    if (fclose(f))
      die_errno(dst);
  }
  free(plain);
  free(image);
}


/* ---- pack ---------------------------------------------------------------------
 * From an SDK isd_config.ini (as isd_download takes it) or one written by
 * unpack, whose [JLNEWFW] section holds what the SDK INI cannot say: the
 * files, resource files, flash size, code limit, chip key and its record, the
 * header's raw special byte.  The command line overrides it.
 *
 * The image goes to flash address EOFFSET x 4 KiB from SYS_CFG_PARAM (or
 * --at): PRCT and VM are flash addresses, so they follow it.  Output starts at
 * flash address 0; --copy duplicates parts of the image elsewhere. */

struct copy {
  uint32_t from, to, len;
};
struct pack_opts {
  const char *ini, *uboot, *out;
  const char *files[MAX_FILES], *res[MAX_FILES];
  size_t nf, nr;
  bool have_key, have_at, have_mark;
  uint16_t key, mark;
  uint32_t at, flash_size, code_limit;
  struct copy copies[16];
  size_t nc;
};

static void parse_copy(const char *s, struct copy *c) {
  char buf[128], *a, *b;
  snprintf(buf, sizeof buf, "%s", s);
  a = strchr(buf, ':');
  b = a ? strchr(a + 1, ':') : NULL;
  if (!a || !b)
    die("a copy is FROM:TO:LEN");
  *a = *b = 0;
  c->from = number(trim(buf));
  c->to = number(trim(a + 1));
  c->len = number(trim(b + 1));
}

/* One pack option at argv[*i]; false when it is not one. */
static bool pack_arg(struct pack_opts *o, int argc, char **argv, int *i) {
  const char *a = argv[*i];
  if (*i + 1 >= argc)
    return false;
  if (!strcmp(a, "-c"))
    o->ini = argv[++*i];
  else if (!strcmp(a, "-u"))
    o->uboot = argv[++*i];
  else if (!strcmp(a, "-o"))
    o->out = argv[++*i];
  else if (!strcmp(a, "-k"))
    o->key = (uint16_t)number(argv[++*i]), o->have_key = true;
  else if (!strcmp(a, "-r") && o->nr < MAX_FILES)
    o->res[o->nr++] = argv[++*i];
  else if (!strcmp(a, "--flash-size"))
    o->flash_size = number(argv[++*i]);
  else if (!strcmp(a, "--code-limit"))
    o->code_limit = number(argv[++*i]);
  else if (!strcmp(a, "--at"))
    o->at = number(argv[++*i]), o->have_at = true;
  else if (!strcmp(a, "--mark"))
    o->mark = (uint16_t)number(argv[++*i]), o->have_mark = true;
  else if (!strcmp(a, "--copy") && o->nc < 16)
    parse_copy(argv[++*i], &o->copies[o->nc++]);
  else
    return false;
  return true;
}

/* A path from the INI: relative to the INI's directory. */
static const char *beside(const char *ini, const char *name) {
  if (name[0] == '/')
    return strdup(name);
  const char *slash = strrchr(ini, '/');
  if (!slash)
    return strdup(name);
  char *p = malloc((size_t)(slash - ini) + 2 + strlen(name));
  if (!p)
    die("out of memory");
  sprintf(p, "%.*s/%s", (int)(slash - ini), ini, name);
  return p;
}
/* A comma-separated INI list into `out`. */
static size_t ini_list(const char *ini, const char *v, const char **out) {
  size_t n = 0;
  char *copy = strdup(v), *save = NULL;
  for (char *t = strtok_r(copy, ",", &save); t && n < MAX_FILES;
       t = strtok_r(NULL, ",", &save))
    if (*trim(t))
      out[n++] = beside(ini, trim(t));
  return n;
}

static struct blob build(struct pack_opts *o, uint16_t *key_out) {
  if (!o->ini)
    die("pack needs -c isd_config.ini");
  struct config c = {0};
  load_config(&c, o->ini);
  /* These are the layout modes understood below. Refuse rather than silently
     producing a plausible-looking image for a different SDK configuration. */
  require_value(&c, "EXTRA_CFG_PARAM", "NEW_FLASH_FS", "YES");
  require_value(&c, "EXTRA_CFG_PARAM", "FORCE_4K_ALIGN", "YES");
  require_value(&c, "EXTRA_CFG_PARAM", "RESERVED_OPT", "0");
  require_value(&c, "RESERVED_CONFIG", "PRCT_ADR", "0");
  require_value(&c, "RESERVED_CONFIG", "PRCT_LEN", "CODE_LEN");
  require_value(&c, "RESERVED_CONFIG", "PRCT_OPT", "2");
  require_value(&c, "RESERVED_CONFIG", "VM_ADR", "0");
  require_value(&c, "RESERVED_CONFIG", "VM_OPT", "1");
  require_value(&c, "RESERVED_CONFIG", "BTIF_ADR", "AUTO");
  require_value(&c, "RESERVED_CONFIG", "BTIF_OPT", "1");
  const char *v;
  if (!o->uboot && (v = cfg(&c, "JLNEWFW", "UBOOT", NULL)))
    o->uboot = beside(o->ini, v);
  if (!o->nf && (v = cfg(&c, "JLNEWFW", "FILES", NULL)))
    o->nf = ini_list(o->ini, v, o->files);
  if (!o->nr && (v = cfg(&c, "JLNEWFW", "RES", NULL)))
    o->nr = ini_list(o->ini, v, o->res);
  if (!o->have_key && (v = cfg(&c, "JLNEWFW", "CHIP_KEY", NULL)))
    o->key = (uint16_t)number(v), o->have_key = true;
  if (!o->have_key)
    o->key = 0xffff;
  if (!o->flash_size && (v = cfg(&c, "JLNEWFW", "FLASH_SIZE", NULL)))
    o->flash_size = number(v);
  if (!o->code_limit && (v = cfg(&c, "JLNEWFW", "CODE_LIMIT", NULL)))
    o->code_limit = number(v);
  if (!o->have_mark && (v = cfg(&c, "JLNEWFW", "MARK", NULL)))
    o->mark = (uint16_t)number(v), o->have_mark = true;
  if (!o->nc && (v = cfg(&c, "JLNEWFW", "COPY", NULL))) {
    char *copy = strdup(v), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t && o->nc < 16;
         t = strtok_r(NULL, ",", &save))
      parse_copy(t, &o->copies[o->nc++]);
  }
  if (!o->have_at)
    o->at = number(cfg(&c, "SYS_CFG_PARAM", "EOFFSET", "0")) * 0x1000;
  if (!o->uboot || !o->nf)
    die("pack needs uboot.boot (-u) and app files");
  if (strcmp(base_name(o->files[0]), "app.bin"))
    die("the first application file must be named app.bin");
  for (size_t i = 0; i < o->nf + o->nr; i++)
    if (strlen(base_name(i < o->nf ? o->files[i] : o->res[i - o->nf])) > 15)
      die("JLFS file names may be at most 15 bytes");

  uint8_t record[34];
  if ((v = cfg(&c, "JLNEWFW", "CHIP_KEY_RECORD", NULL))) {
    if (strlen(v) != 68)
      die("CHIP_KEY_RECORD is 34 bytes in hex");
    for (unsigned i = 0; i < 34; i++) {
      char h[3] = {v[2 * i], v[2 * i + 1], 0};
      record[i] = (uint8_t)strtoul(h, NULL, 16);
    }
    if (crc16(record, 32) != rd16(record + 32))
      die("CHIP_KEY_RECORD CRC mismatch");
    set_record_key(record, o->key);
  } else {
    make_chipkey(record, o->key);
  }
  struct blob sys = encode_syscfg(&c, record);
  size_t uboot_n;
  uint8_t *uboot = read_file(o->uboot, &uboot_n);
  struct app_file files[2 * MAX_FILES];
  uint8_t area_flags[2 * MAX_FILES]; /* resources: 0x83, or 0x82 for "name:82" */
  size_t nall = o->nf + o->nr;
  for (size_t i = 0; i < nall; i++) {
    char *path = strdup(i < o->nf ? o->files[i] : o->res[i - o->nf]);
    size_t pn = strlen(path);
    area_flags[i] = 0x83;
    if (i >= o->nf && pn > 3 && !strcmp(path + pn - 3, ":82")) {
      path[pn - 3] = 0;
      area_flags[i] = 0x82;
    }
    files[i] = (struct app_file){path, base_name(path), NULL, 0};
    files[i].data = read_file(path, &files[i].size);
  }
  uint32_t appbase =
      align_up((uint32_t)(32 + 4 * 32 + uboot_n + sys.n), 0x1000);
  uint32_t entry = number(cfg(&c, "EXTRA_CFG_PARAM", "ENTRY", "0x1e00120"));
  uint32_t appoff = entry & 0xfffffu;
  /* The reserved list as unpack found it (AREAS: offset:size:flags:reserved:
   * CRC field:name field, 16 bytes in hex), or else VM, PRCT and BTIF from the
   * layout. */
  struct entry areas[MAX_FILES];
  size_t nareas = 0;
  if ((v = cfg(&c, "JLNEWFW", "AREAS", NULL)) && *v) {
    char *copy = strdup(v), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t && nareas < MAX_FILES;
         t = strtok_r(NULL, ",", &save)) {
      unsigned off, size, flags, res, crc;
      char hex[33];
      if (sscanf(trim(t), "%x:%x:%x:%x:%x:%32s", &off, &size, &flags, &res, &crc, hex) != 6 ||
          strlen(hex) != 32)
        die("AREAS entry is offset:size:flags:reserved:crc:name-hex");
      struct entry *a = &areas[nareas++];
      *a = (struct entry){(uint16_t)crc, off, size, (uint8_t)flags, (uint8_t)res, 0, {0}};
      for (unsigned k = 0; k < 16; k++) {
        unsigned b;
        sscanf(hex + 2 * k, "%2x", &b);
        a->name[k] = (uint8_t)b;
      }
    }
    free(copy);
    areas[nareas - 1].index = 1;
  }
  size_t nested_n = o->nf + (nareas ? nareas : 3);
  if (appoff < 32 + (uint32_t)nested_n * 32)
    die("ENTRY's flash offset overlaps the app-area headers");
  uint32_t cursor = appoff;
  for (size_t i = 0; i < o->nf; i++) {
    if (files[i].size > UINT32_MAX - cursor)
      die("app files too large");
    cursor += (uint32_t)files[i].size;
  }
  uint32_t outer_size = cursor; /* relative to the outer header */
  uint32_t res_size = 0;        /* resources follow, each behind its header */
  for (size_t i = o->nf; i < nall; i++)
    res_size += 32 + (uint32_t)files[i].size;
  uint32_t image_end = align_up(appbase + outer_size + res_size, 0x1000);
  uint32_t code_end = o->code_limit ? o->code_limit : image_end;
  if ((code_end & 0xfff) || code_end < image_end)
    die("code limit must be 4-KiB aligned and contain the application");
  uint32_t vm_len = number(cfg(&c, "RESERVED_CONFIG", "VM_LEN", "8K"));
  uint32_t btif_len = number(cfg(&c, "RESERVED_CONFIG", "BTIF_LEN", "0x1000"));
  /* With the flash size given, VM is what is left between the code and
   * BTIF (GameSir's tool leaves a pad placed at 0x1000 28 of VM_LEN's 32
   * KiB); otherwise the flash is made big enough for VM_LEN. */
  uint32_t required = o->at + code_end + (o->flash_size ? 0 : vm_len) +
                      btif_len + 0x1000;
  uint32_t total = o->flash_size ? o->flash_size : next_pow2(required);
  if (!o->flash_size && total < 0x20000)
    total = 0x20000;
  if (total < required)
    die("flash size is too small for the image and reserved regions");
  if (total & (total - 1))
    die("flash size must be a power of two");
  uint32_t keymac = total - 0x1000, btif = keymac - btif_len,
           vm = o->at + code_end;
  if (vm > btif)
    die("application does not fit reserved layout");
  if (btif - vm < vm_len)
    printf("note: VM is 0x%x bytes, less than VM_LEN 0x%x\n", btif - vm, vm_len);

  struct blob image = {0};
  reserve(&image, image_end);
  image.n = image_end;
  memset(image.p, 0xff, image.n);
  flash_header(image.p, &c, keymac);
  uint32_t top_data = 32 + 4 * 32, uboot_off = top_data,
           sys_off = uboot_off + (uint32_t)uboot_n;
  struct entry top[4] = {0};
  top[0] = (struct entry){crc16(uboot, uboot_n), uboot_off, (uint32_t)uboot_n,
                          (uint8_t)number(cfg(&c, "JLNEWFW", "UBOOT_FLAGS", "0")), 0, 0, {0}};
  set_name(top[0].name, "uboot.boot");
  top[1] = (struct entry){
      crc16(sys.p, sys.n), sys_off, (uint32_t)sys.n, 0x02, 0xff, 0, {0}};
  set_name(top[1].name, "isd_config.ini");
  top[2] = (struct entry){o->have_mark ? o->mark : 0xffff,
                          appbase, 0xffffffff, 0x81, 0xff, 0, {0}};
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
  outer.index = o->nr ? 0 : 1; /* 1: the last area */
  set_name(outer.name, "app_area_head");
  cursor = appoff;
  for (size_t i = 0; i < o->nf; i++) {
    struct entry e = {crc16(files[i].data, files[i].size),
                      cursor, (uint32_t)files[i].size, 0x82, 0xff, 0, {0}};
    set_name(e.name, files[i].name);
    encode_entry(image.p + appbase + 32 + (uint32_t)i * 32, &e);
    memcpy(image.p + appbase + cursor, files[i].data, files[i].size);
    cursor += (uint32_t)files[i].size;
  }
  struct entry r[3] = {0};
  r[0] = (struct entry){0xffff, vm, btif - vm, 0x12, 0x81, 0, {0}};
  set_name(r[0].name, "VM");
  wr32(r[0].name + 12, vm_len);
  r[1] = (struct entry){0xffff, o->at, code_end, 0x92, 0x82, 0, {0}};
  set_name(r[1].name, "PRCT");
  memcpy(r[1].name + 12, "CODE", 4);
  r[2] = (struct entry){0xffff, btif, btif_len, 0x92, 0x81, 1, {0}};
  set_name(r[2].name, "BTIF");
  memcpy(r[2].name + 12, "AUTO", 4);
  for (unsigned i = 0; i < (nareas ? nareas : 3); i++)
    encode_entry(image.p + appbase + 32 + (uint32_t)(o->nf + i) * 32, nareas ? &areas[i] : &r[i]);
  outer.data_crc = crc16(image.p + appbase + 32, outer_size - 32);
  encode_entry(image.p + appbase, &outer);
  uint32_t pos = appbase + outer_size;
  for (size_t i = o->nf; i < nall; i++) {
    struct entry e = {crc16(files[i].data, files[i].size), 32,
                      32 + (uint32_t)files[i].size, area_flags[i], 0xff,
                      (uint16_t)(i + 1 == nall), {0}};
    set_name(e.name, files[i].name);
    encode_entry(image.p + pos, &e);
    memcpy(image.p + pos + 32, files[i].data, files[i].size);
    pos += 32 + (uint32_t)files[i].size;
  }
  scramble_bankcb(image.p + uboot_off, uboot_n);
  jl_sfc(image.p, appbase, outer_size + res_size, appbase, o->key);
  for (unsigned i = 0; i < 4; i++)
    jl_enc(image.p + 32 + i * 32, 32, 0xffff);

  /* On flash: the image at `at`, then the copies. */
  uint32_t end = o->at + image_end;
  for (size_t i = 0; i < o->nc; i++) {
    if ((uint64_t)o->copies[i].from + o->copies[i].len > image_end)
      die("a copy reaches past the image");
    if (o->copies[i].to + o->copies[i].len > end)
      end = o->copies[i].to + o->copies[i].len;
  }
  if (end > total)
    die("the image and copies do not fit the flash");
  struct blob flash = {0};
  reserve(&flash, end);
  flash.n = end;
  memset(flash.p, 0xff, end);
  memcpy(flash.p + o->at, image.p, image_end);
  for (size_t i = 0; i < o->nc; i++)
    memcpy(flash.p + o->copies[i].to, image.p + o->copies[i].from,
           o->copies[i].len);
  printf("packed: %u bytes, image at 0x%x (app @0x%x, end 0x%x, code limit "
         "0x%x), VM 0x%x-0x%x, flash 0x%x, chip key 0x%04x\n",
         end, o->at, appbase, image_end, code_end, vm, btif, total, o->key);
  if (key_out)
    *key_out = o->key;
  free(image.p);
  return flash;
}

static void cmd_pack(int argc, char **argv) {
  struct pack_opts o = {0};
  for (int i = 0; i < argc; i++) {
    if (pack_arg(&o, argc, argv, &i))
      continue;
    if (argv[i][0] == '-')
      die("unknown pack option");
    if (o.nf == MAX_FILES)
      die("too many app files");
    o.files[o.nf++] = argv[i];
  }
  if (!o.out)
    die("pack needs -o OUTPUT");
  struct blob b = build(&o, NULL);
  write_file(o.out, b.p, b.n);
  printf("wrote %s\n", o.out);
}

/* ---- flash --------------------------------------------------------------------
 * Through the chip's ROM loader (UBOOT1.00, a USB mass-storage device): a
 * loader blob goes into RAM and runs, then takes erase/write/read commands.
 * Commands are 16-byte vendor CDBs: a big-endian 16-bit command, arguments,
 * 0xff padding; those without data answer with 16 bytes starting with the
 * command (protocol as worked out by kagaimiq's jl-uboot-tool).  The loader
 * blob is the SDK's (cpu/br23/tools/br23loader.bin; --loader or
 * JLNEWFW_LOADER); jl-uboot-tool's older, smaller one works too.  Linux SCSI
 * generic or Windows SCSI pass-through. */

#if defined(__linux__) || defined(_WIN32)
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#else
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "hid.lib")
#endif
#include <time.h>
#define SG_DXFER_NONE 0
#define SG_DXFER_TO_DEV 1
#define SG_DXFER_FROM_DEV 2
#endif

static void dev_sleep(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  nanosleep(&(struct timespec){ms / 1000, (ms % 1000) * 1000000L}, NULL);
#endif
}

typedef intptr_t device_fd;
static device_fd dev_open(const char *path) {
#ifdef _WIN32
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  return h == INVALID_HANDLE_VALUE ? -1 : (device_fd)h;
#else
  return open(path, O_RDWR);
#endif
}
static void dev_close(device_fd fd) {
#ifdef _WIN32
  CloseHandle((HANDLE)fd);
#else
  close((int)fd);
#endif
}
/* Inquiry only during enumeration; vendor commands require a matched identity. */
static size_t device_paths(char paths[256][300]) {
  size_t n = 0;
#ifdef _WIN32
  for (unsigned i = 0; i < 64; i++)
    snprintf(paths[n++], 300, "\\\\.\\PhysicalDrive%u", i);
#else
  static const char *const dirs[] = {"/dev", "/dev/bwrap"};
  for (size_t i = 0; i < 2; i++) {
    DIR *dir = opendir(dirs[i]);
    struct dirent *e;
    while (dir && (e = readdir(dir)) && n < 256)
      if (!strncmp(e->d_name, "sg", 2))
        snprintf(paths[n++], 300, "%s/%s", dirs[i], e->d_name);
    if (dir) closedir(dir);
  }
#endif
  return n;
}

#ifndef _WIN32
/* The sysfs directory of the USB device behind an sg or hidraw node: the
 * path above its interface ("5-2.1:1.0"). */
static bool usb_dir(const char *path, char dir[400]) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  char link[400], *real, *c;
  bool ok = false;
  snprintf(link, sizeof link, "/sys/class/%s/%s/device",
           strncmp(name, "hidraw", 6) ? "scsi_generic" : "hidraw", name);
  real = realpath(link, NULL);
  /* .../usb5/5-2/5-2.1/5-2.1:1.0/host7/target7:0:0/7:0:0:0
   * .../usb5/5-2/5-2.1/5-2.1:1.2/0003:3537:100F.0042 */
  for (c = real && strstr(real, "/usb") ? strchr(real, '/') : NULL; c; c = strchr(c + 1, '/')) {
    char *end = strchr(c + 1, '/');
    size_t len = end ? (size_t)(end - c - 1) : strlen(c + 1);
    char *colon = memchr(c + 1, ':', len), *dash = memchr(c + 1, '-', len);
    if (c[1] >= '0' && c[1] <= '9' && colon && dash && dash < colon) {
      *c = 0;
      snprintf(dir, 400, "%s", real);
      ok = true;
      break;
    }
  }
  free(real);
  return ok;
}

#endif

/* The USB device behind a disk or HID node: its port (Linux: "5-2.1";
 * Windows: "Port_#0002.Hub_#0003"), and an ID that also changes with every
 * enumeration (Linux: the port and device number; Windows: the location
 * and the instance ID, which carries the VID and PID).  A device restarting
 * into its ROM loader comes back on the same port, but often on another
 * node: the number is the kernel's next free one. */
#ifdef _WIN32
/* GUID_DEVINTERFACE_DISK */
static const GUID disk_guid = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

/* The device instance of \\.\PhysicalDriveN or of a device interface path. */
static bool devinst_of(const char *path, DEVINST *out) {
  union {
    SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
    char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 300];
  } detail;
  SP_DEVICE_INTERFACE_DATA it = {.cbSize = sizeof it};
  SP_DEVINFO_DATA info = {.cbSize = sizeof info};
  bool ok = false;
  unsigned drive;
  if (sscanf(path, "\\\\.\\PhysicalDrive%u", &drive) == 1) {
    HDEVINFO set = SetupDiGetClassDevsA(&disk_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    for (DWORD k = 0; !ok && set != INVALID_HANDLE_VALUE &&
                      SetupDiEnumDeviceInterfaces(set, NULL, &disk_guid, k, &it); k++) {
      detail.d.cbSize = sizeof detail.d;
      if (!SetupDiGetDeviceInterfaceDetailA(set, &it, &detail.d, sizeof detail, NULL, &info))
        continue;
      HANDLE h = CreateFileA(detail.d.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
      STORAGE_DEVICE_NUMBER num;
      DWORD got;
      if (h == INVALID_HANDLE_VALUE)
        continue;
      if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &num, sizeof num, &got, NULL) &&
          num.DeviceNumber == drive)
        ok = true, *out = info.DevInst;
      CloseHandle(h);
    }
    if (set != INVALID_HANDLE_VALUE)
      SetupDiDestroyDeviceInfoList(set);
    return ok;
  }
  HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
  if (set == INVALID_HANDLE_VALUE)
    return false;
  detail.d.cbSize = sizeof detail.d;
  if (SetupDiOpenDeviceInterfaceA(set, path, 0, &it) &&
      SetupDiGetDeviceInterfaceDetailA(set, &it, &detail.d, sizeof detail, NULL, &info))
    ok = true, *out = info.DevInst;
  SetupDiDestroyDeviceInfoList(set);
  return ok;
}
#endif

#define USB_ID_MAX 300 /* usb_node()'s ID */
static bool usb_node(const char *path, char port[64], char id[USB_ID_MAX]) {
#ifdef _WIN32
  DEVINST d, parent;
  char instance[200], location[64];
  ULONG len = sizeof location;
  if (!devinst_of(path, &d))
    return false;
  /* Up to the USB device itself: not its interface (&MI_), not above it. */
  for (;;) {
    if (CM_Get_Device_IDA(d, instance, sizeof instance, 0) != CR_SUCCESS)
      return false;
    if (!strncmp(instance, "USB\\", 4) && !strstr(instance, "&MI_"))
      break;
    if (CM_Get_Parent(&parent, d, 0) != CR_SUCCESS)
      return false;
    d = parent;
  }
  if (CM_Get_DevNode_Registry_PropertyA(d, CM_DRP_LOCATION_INFORMATION, NULL, location, &len, 0) !=
      CR_SUCCESS)
    return false;
  if (port)
    snprintf(port, 64, "%s", location);
  if (id)
    snprintf(id, USB_ID_MAX, "%s#%s", location, instance);
  return true;
#else
  char dir[400], link[420];
  if (!usb_dir(path, dir))
    return false;
  if (port)
    snprintf(port, 64, "%s", strrchr(dir, '/') + 1);
  if (id) {
    snprintf(link, sizeof link, "%s/devnum", dir);
    FILE *f = fopen(link, "r");
    int num;
    bool ok = f && fscanf(f, "%d", &num) == 1;
    if (f)
      fclose(f);
    if (!ok)
      return false;
    snprintf(id, USB_ID_MAX, "%s#%d", strrchr(dir, '/') + 1, num);
  }
  return true;
#endif
}

static bool usb_port(const char *path, char port[64]) {
  return usb_node(path, port, NULL);
}

/* "" if not known. */
static void usb_device(const char *path, char id[USB_ID_MAX]) {
  if (!usb_node(path, NULL, id))
    id[0] = 0;
}

/* Devices asked to restart into their loader.  Until one leaves the bus it
 * gets nothing more: its loader is still learning the crystal from the
 * host's frames, attached but not answering, and a command to it makes the
 * host reset the port over and over (behind hub 05e3:0608 the loader then
 * never comes up). */
static char asked[16][USB_ID_MAX];
static size_t n_asked;
static bool restarting(const char *path) {
  char id[USB_ID_MAX];
  usb_device(path, id);
  for (size_t i = 0; id[0] && i < n_asked; i++)
    if (!strcmp(asked[i], id))
      return true;
  return false;
}

#define LOADER_BLOCK 512
#define LOADER_ARG 1 /* target: SPI NOR */

/* The chips whose ROM loader we drive, by the INQUIRY vendor it answers, with
 * the SDK's RAM loader for each and where it runs (the BD19 from kagaimiq's
 * jl-uboot-tool data; docs/gamesir-nova-lite-2/). */
static const struct {
  const char *chip, *blob, *sdk_dir; /* sdk_dir: under the SDK's cpu/ */
  uint32_t address;
} chips[] = {
    {"BR23", "br23loader.bin", "br23", 0x12000},
    {"BD19", "bd19loader.bin", "bd19", 0x2000},
};
#define CHIP_COUNT (sizeof chips / sizeof chips[0])

static int chip_of(const uint8_t vendor[4]) {
  for (size_t i = 0; i < CHIP_COUNT; i++)
    if (!memcmp(vendor, chips[i].chip, 4))
      return (int)i;
  return -1;
}

struct dev {
  device_fd fd;
  const char *path;
  int chip;            /* chips[] */
  uint32_t io;         /* largest flash transfer */
  uint16_t key;        /* chip key */
  uint32_t flash_size; /* from the flash ID */
  uint32_t flash_id;
};

static bool sg(device_fd fd, const uint8_t *cdb, int len, int dir, uint8_t *data,
               uint32_t n, unsigned timeout_ms) {
#ifdef _WIN32
  /* Buffered pass-through avoids application buffer alignment constraints. */
  size_t offset = sizeof(SCSI_PASS_THROUGH) + 32;
  size_t total = offset + n;
  uint8_t *packet = calloc(1, total);
  if (!packet) return false;
  SCSI_PASS_THROUGH *h = (SCSI_PASS_THROUGH *)packet;
  h->Length = sizeof *h;
  h->CdbLength = (UCHAR)len;
  h->SenseInfoLength = 32;
  h->SenseInfoOffset = sizeof *h;
  h->DataIn = !n ? SCSI_IOCTL_DATA_UNSPECIFIED :
              dir == SG_DXFER_FROM_DEV ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
  h->DataTransferLength = n;
  h->TimeOutValue = (timeout_ms + 999) / 1000;
  h->DataBufferOffset = offset;
  memcpy(h->Cdb, cdb, len);
  if (n && dir == SG_DXFER_TO_DEV) memcpy(packet + offset, data, n);
  DWORD got;
  bool ok = DeviceIoControl((HANDLE)fd, IOCTL_SCSI_PASS_THROUGH,
                             packet, (DWORD)total, packet, (DWORD)total, &got, NULL) &&
            !h->ScsiStatus && h->DataTransferLength == n;
  if (ok && n && dir == SG_DXFER_FROM_DEV) memcpy(data, packet + offset, n);
  free(packet);
  return ok;
#else
  uint8_t sense[32];
  sg_io_hdr_t h = {0};
  h.interface_id = 'S';
  h.cmd_len = (unsigned char)len;
  h.cmdp = (unsigned char *)cdb;
  h.dxfer_direction = n ? dir : SG_DXFER_NONE;
  h.dxferp = data;
  h.dxfer_len = n;
  h.sbp = sense;
  h.mx_sb_len = sizeof sense;
  h.timeout = timeout_ms;
  int r = ioctl(fd, SG_IO, &h);
  if (getenv("JLNEWFW_DEBUG")) {
    fprintf(stderr, "sg %02x%02x dir %d len %u: ioctl %d info %x status %x host %x "
                    "driver %x resid %d",
            cdb[0], cdb[1], dir, n, r, h.info, h.status, h.host_status,
            h.driver_status, h.resid);
    for (uint32_t i = 0; dir == SG_DXFER_FROM_DEV && i < n && i < 16; i++)
      fprintf(stderr, "%s%02x", i ? "" : " data ", data[i]);
    fprintf(stderr, "\n");
  }
  return r == 0 && (h.info & SG_INFO_OK_MASK) == SG_INFO_OK;
#endif
}

/* Product, and the chip (chips[]; -1 for another vendor). */
static bool inquiry(device_fd fd, char product[17], int *chip) {
  uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0}, d[36];
  if (!sg(fd, cdb, 6, SG_DXFER_FROM_DEV, d, 36, 2000))
    return false;
  memcpy(product, d + 16, 16);
  product[16] = 0;
  *chip = chip_of(d + 8);
  return true;
}
static void jl_cdb(uint8_t cdb[16], uint16_t cmd, const uint8_t *args,
                   size_t n) {
  memset(cdb, 0xff, 16);
  cdb[0] = (uint8_t)(cmd >> 8);
  cdb[1] = (uint8_t)cmd;
  memcpy(cdb + 2, args, n);
}
/* A command without data; `resp` gets the 14 bytes after the echo. */
static bool jl_exec(struct dev *d, uint16_t cmd, const uint8_t *args, size_t n,
                    uint8_t resp[14]) {
  uint8_t cdb[16], r[16];
  jl_cdb(cdb, cmd, args, n);
  if (!sg(d->fd, cdb, 16, SG_DXFER_FROM_DEV, r, 16, 20000) ||
      rd16be(r) != cmd)
    return false;
  if (resp)
    memcpy(resp, r + 2, 14);
  return true;
}
static bool jl_data(struct dev *d, uint16_t cmd, const uint8_t *args, size_t n,
                    int dir, uint8_t *data, uint32_t len) {
  uint8_t cdb[16];
  jl_cdb(cdb, cmd, args, n);
  return sg(d->fd, cdb, 16, dir, data, len, 20000);
}
static void be32(uint8_t *p, uint32_t x) {
  p[0] = (uint8_t)(x >> 24), p[1] = (uint8_t)(x >> 16), p[2] = (uint8_t)(x >> 8),
  p[3] = (uint8_t)x;
}
/* Address, 16-bit length, 0, CRC16 of the data: memory and flash writes. */
static bool jl_write(struct dev *d, uint16_t cmd, uint32_t addr,
                     const uint8_t *data, uint32_t len) {
  uint8_t a[9];
  be32(a, addr);
  a[4] = (uint8_t)(len >> 8), a[5] = (uint8_t)len, a[6] = 0;
  wr16(a + 7, crc16(data, len));
  return jl_data(d, cmd, a, 9, SG_DXFER_TO_DEV, (uint8_t *)data, len);
}
static bool flash_read(struct dev *d, uint32_t addr, uint8_t *out, uint32_t n) {
  for (uint32_t done = 0; done < n;) {
    uint32_t m = n - done < d->io ? n - done : d->io;
    uint8_t a[6];
    be32(a, addr + done);
    a[4] = (uint8_t)(m >> 8), a[5] = (uint8_t)m;
    if (!jl_data(d, 0xfd05, a, 6, SG_DXFER_FROM_DEV, out + done, m))
      return false;
    done += m;
  }
  return true;
}

/* The "CrcDecode" stream: CRC16s over a fixed 16-byte string (GB2312).  The
 * chip key comes back scrambled with it; the loader blob is stored scrambled
 * with it per 512-byte block, and BR23's ROM unscrambles RAM writes itself
 * (and scrambles reads), so the blob is sent as it is. */
static void crc_cipher(uint8_t *p, size_t n, uint32_t key) {
  static const uint8_t magic[16] = {0xc3, 0xcf, 0xc0, 0xe8, 0xce, 0xd2,
                                    0xb0, 0xae, 0xc4, 0xe3, 0xa3, 0xac,
                                    0xd3, 0xf1, 0xc1, 0xd6};
  uint8_t k[2];
  wr16(k, (uint16_t)(key >> 16));
  uint16_t crc = crc16_seed(k, 2, (uint16_t)key);
  for (size_t i = 0; i < n; i++) {
    crc = crc16_seed(magic + i % 16, 1, crc);
    p[i] ^= (uint8_t)crc;
  }
}

/* A running JieLi SDK firmware that shows itself as a disk (vendor: the
 * chip name) reboots into its ROM loader on any of the loader's commands, as
 * the SDK's mass-storage code does (and ours).  Sent to every such device;
 * which one is written is then decided by PID.  Repeat while waiting so
 * firmware connected after the command starts is also restarted; --device
 * limits restart requests to that path. */
/* IDENTIFY byte 6 distinguishes our pad and dongle builds. Keep firmware for
 * the other board running; stock firmware is selected by PID in the loader. */
/* ---- what is connected -------------------------------------------------------
 * A disk is classified by INQUIRY: vendor "BR23" is a Jieli chip; product
 * "UBOOT1.00" its ROM loader.  Our firmware answers IDENTIFY ("C2PD", the
 * image's PID at 40); other Jieli SDK firmware answers neither (its PID is
 * known only once its loader reads the flash).  A stock pad that shows
 * itself only as HID is known by the table below, in each of its USB modes;
 * in some of them it takes a command into its loader. */

enum disk_class { DISK_OTHER, DISK_LOADER, DISK_OURS, DISK_JIELI };

static enum disk_class classify_disk(device_fd fd, char pid[17], int *chip) {
  uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0}, q[36];
  uint8_t id[16] = {0xf5, 0x00}, ident[56] = {0};
  pid[0] = 0;
  *chip = -1;
  if (!sg(fd, cdb, 6, SG_DXFER_FROM_DEV, q, 36, 2000) || (*chip = chip_of(q + 8)) < 0)
    return DISK_OTHER;
  if (!memcmp(q + 16, "UBOOT", 5))
    return DISK_LOADER;
  if (sg(fd, id, 16, SG_DXFER_FROM_DEV, ident, sizeof ident, 2000) &&
      !memcmp(ident, "C2PD", 4)) {
    memcpy(pid, ident + 40, 16);
    pid[16] = 0;
    return DISK_OURS;
  }
  return DISK_JIELI;
}

static const struct {
  const char *pid;              /* the stock image's */
  const char *board, *mode;
  uint16_t vid, product, bcd;
  const char *manufacturer, *product_name;   /* NULL: any */
  uint16_t usage_page;          /* the interface taking the command; 0: no command */
  uint8_t len;                  /* written: the report ID byte (0: none) and the report */
  uint8_t report[5];            /* that byte, then the command; zero-padded to len */
  const char *by_hand;          /* where no mode has a command */
} stock_hid[] = {
  /* docs/gamesir-cyclone2/stock-pad-dongle.md (3.52) */
  {"GS_C2_ADC_DEVICE", "GameSir Cyclone 2", "Xbox 360 (green)", 0x3537, 0x1053, 0x0352,
   NULL, NULL, 0xfff0, 64, {0x0f, 0x17, 0x55, 0x88, 0x00}},
  {"GS_C2_ADC_DEVICE", "GameSir Cyclone 2", "Switch (red)", 0x057e, 0x2009, 0x0352,
   NULL, "Gamepad", 0, 0, {0}},
  {"GS_C2_ADC_DEVICE", "GameSir Cyclone 2", "DS4 (blue)", 0x054c, 0x09cc, 0x0352,
   "Guangzhou Chicken Run Network Technology Co., Ltd.", NULL, 0, 0, {0}},
  /* docs/gamesir-nova-lite-2/ */
  {"AC695X_TRANS", "GameSir Nova 2 Lite", "Xbox 360 (green)", 0x3537, 0x100f, 0x0100,
   NULL, NULL, 0xff7a, 65, {0x00, 0xf2, 0x00, 0x04, 0xf6}},
  {"AC695X_TRANS", "GameSir Nova 2 Lite", "Switch (red)", 0x057e, 0x2009, 0x0200,
   "Zikway", "Pro Controller", 0, 0, {0}},
  {"AC695X_TRANS", "GameSir Nova 2 Lite", "DS4 (blue)", 0x054c, 0x09cc, 0x0100,
   "Sony Computer Entertainment", "Wireless Controller", 0, 0, {0}},
  {"AC632N_TRANS", "GameSir Nova 2 Lite dongle", "", 0x3537, 0x2106, 0x0100, "Zikway", "HID zk", 0,
   0, {0}, "hold its pairing button while connecting USB"},
};
#define STOCK_HID_COUNT (sizeof stock_hid / sizeof stock_hid[0])

/* A HID interface (Linux) or top-level collection (Windows) as the table
 * needs it. */
struct hid_dev {
  char path[300];
  unsigned vid, product, bcd;
  char manufacturer[64], product_name[64];
#ifdef _WIN32
  unsigned usage_page;          /* the collection's */
  unsigned out_len;             /* its output report, with the ID byte */
#else
  uint8_t desc[4096];           /* the report descriptor */
  size_t desc_n;
#endif
};

/* Whether a report descriptor has a usage page item for page. */
static bool has_usage_page(const uint8_t *d, size_t n, unsigned page) {
  for (size_t i = 0; i < n;) {
    if (d[i] == 0xfe) { /* long item: size in the next byte */
      i += 3 + (i + 1 < n ? d[i + 1] : 0);
      continue;
    }
    size_t size = (d[i] & 3) == 3 ? 4 : d[i] & 3;
    unsigned v = 0;
    for (size_t k = 0; k < size && i + 1 + k < n; k++)
      v |= (unsigned)d[i + 1 + k] << 8 * k;
    if ((d[i] & 0xfc) == 0x04 && v == page)
      return true;
    i += 1 + size;
  }
  return false;
}

/* The table entry h is, or -1.  An entry with a command matches only the
 * interface that takes it. */
static int stock_hid_entry(const struct hid_dev *h) {
  for (size_t i = 0; i < STOCK_HID_COUNT; i++) {
    if (h->vid != stock_hid[i].vid || h->product != stock_hid[i].product ||
        h->bcd != stock_hid[i].bcd)
      continue;
    if (stock_hid[i].manufacturer && strcmp(h->manufacturer, stock_hid[i].manufacturer))
      continue;
    if (stock_hid[i].product_name && strcmp(h->product_name, stock_hid[i].product_name))
      continue;
#ifdef _WIN32
    if (stock_hid[i].usage_page && (h->usage_page != stock_hid[i].usage_page ||
                                    h->out_len != stock_hid[i].len))
      continue;
#else
    if (stock_hid[i].usage_page && !has_usage_page(h->desc, h->desc_n, stock_hid[i].usage_page))
      continue;
#endif
    return (int)i;
  }
  return -1;
}

#ifndef _WIN32
static void read_line(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "r");
  out[0] = 0;
  if (f && fgets(out, (int)n, f))
    out[strcspn(out, "\n")] = 0;
  if (f)
    fclose(f);
}
#endif

/* Every HID interface, into out (at most max). */
static size_t hid_devices(struct hid_dev *out, size_t max) {
  size_t n = 0;
#ifdef _WIN32
  GUID guid;
  HidD_GetHidGuid(&guid);
  HDEVINFO set = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  SP_DEVICE_INTERFACE_DATA it = {.cbSize = sizeof it};
  for (DWORD k = 0; set != INVALID_HANDLE_VALUE && n < max &&
                    SetupDiEnumDeviceInterfaces(set, NULL, &guid, k, &it); k++) {
    union {
      SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
      char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 300];
    } detail;
    detail.d.cbSize = sizeof detail.d;
    if (!SetupDiGetDeviceInterfaceDetailA(set, &it, &detail.d, sizeof detail, NULL, NULL))
      continue;
    /* No access asked: attributes and strings need none, even for a
     * keyboard Windows keeps to itself. */
    HANDLE h = CreateFileA(detail.d.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
      continue;
    struct hid_dev *d = &out[n];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", detail.d.DevicePath);
    HIDD_ATTRIBUTES a = {.Size = sizeof a};
    PHIDP_PREPARSED_DATA pp;
    HIDP_CAPS caps = {0};
    WCHAR w[64];
    if (HidD_GetAttributes(h, &a)) {
      d->vid = a.VendorID, d->product = a.ProductID, d->bcd = a.VersionNumber;
      if (HidD_GetPreparsedData(h, &pp)) {
        HidP_GetCaps(pp, &caps);
        HidD_FreePreparsedData(pp);
      }
      d->usage_page = caps.UsagePage, d->out_len = caps.OutputReportByteLength;
      if (HidD_GetManufacturerString(h, w, sizeof w))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, d->manufacturer, sizeof d->manufacturer, NULL, NULL);
      if (HidD_GetProductString(h, w, sizeof w))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, d->product_name, sizeof d->product_name, NULL, NULL);
      n++;
    }
    CloseHandle(h);
  }
  if (set != INVALID_HANDLE_VALUE)
    SetupDiDestroyDeviceInfoList(set);
#else
  static const char *const dirs[] = {"/dev", "/dev/bwrap"};
  char seen[256][256];
  size_t ns = 0;
  for (size_t i = 0; i < 2; i++) {
    DIR *dir = opendir(dirs[i]);
    struct dirent *e;
    while (dir && (e = readdir(dir)) && n < max) {
      char sys[512], line[128], usb[400];
      unsigned bus;
      if (strncmp(e->d_name, "hidraw", 6))
        continue;
      struct hid_dev *d = &out[n];
      memset(d, 0, sizeof *d);
      snprintf(d->path, sizeof d->path, "%s/%s", dirs[i], e->d_name);
      /* The same node under both directories: once. */
      bool dup = false;
      for (size_t k = 0; k < ns && !dup; k++)
        dup = !strcmp(seen[k], e->d_name);
      if (dup || access(d->path, R_OK | W_OK)) /* the node we may use */
        continue;
      if (ns < 256)
        snprintf(seen[ns++], sizeof seen[0], "%s", e->d_name);
      snprintf(sys, sizeof sys, "/sys/class/hidraw/%s/device/uevent", e->d_name);
      FILE *f = fopen(sys, "r");
      while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &d->vid, &d->product) == 3)
          break;
      if (f)
        fclose(f);
      snprintf(sys, sizeof sys, "/sys/class/hidraw/%s/device/report_descriptor", e->d_name);
      f = fopen(sys, "rb");
      if (f) {
        d->desc_n = fread(d->desc, 1, sizeof d->desc, f);
        fclose(f);
      }
      if (usb_dir(d->path, usb)) {
        snprintf(sys, sizeof sys, "%s/bcdDevice", usb);
        read_line(sys, line, sizeof line);
        d->bcd = (unsigned)strtoul(line, NULL, 16);
        snprintf(sys, sizeof sys, "%s/manufacturer", usb);
        read_line(sys, d->manufacturer, sizeof d->manufacturer);
        snprintf(sys, sizeof sys, "%s/product", usb);
        read_line(sys, d->product_name, sizeof d->product_name);
      }
      n++;
    }
    if (dir)
      closedir(dir);
  }
#endif
  return n;
}

/* Whether path's device was already named in a message: true from the
 * second call on. */
static bool told_before(const char *path) {
  static char told[16][USB_ID_MAX];
  static size_t n_told;
  char id[USB_ID_MAX];
  usb_device(path, id);
  if (!id[0])
    snprintf(id, sizeof id, "%.299s", path);
  for (size_t k = 0; k < n_told; k++)
    if (!strcmp(told[k], id))
      return true;
  if (n_told < 16)
    snprintf(told[n_told++], USB_ID_MAX, "%s", id);
  return false;
}

/* Writes entry e's loader command to h. */
static bool send_stock_hid(const struct hid_dev *h, int e) {
  uint8_t out[65] = {0};
  bool ok = false;
  memcpy(out, stock_hid[e].report, sizeof stock_hid[e].report);
#ifdef _WIN32
  HANDLE fh = CreateFileA(h->path, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  DWORD done;
  if (fh != INVALID_HANDLE_VALUE) {
    ok = WriteFile(fh, out, stock_hid[e].len, &done, NULL);
    CloseHandle(fh);
  }
#else
  int fd = open(h->path, O_RDWR);
  if (fd >= 0) {
    ok = write(fd, out, stock_hid[e].len) == (ssize_t)stock_hid[e].len;
    close(fd);
  }
#endif
  return ok;
}

static bool open_dev(struct dev *d, const char *path) {
  char product[17];
  if (restarting(path))
    return false;
  d->fd = dev_open(path);
  d->path = path;
  if (d->fd < 0)
    return false;
  if (inquiry(d->fd, product, &d->chip) && d->chip >= 0 && !strncmp(product, "UBOOT", 5))
    return true;
  dev_close(d->fd);
  return false;
}
/* Load and start the flash loader, then ask what the chip is. */
/* The RAM loader for a chip: --loader (or JLNEWFW_LOADER) names the SDK
 * (its cpu/<chip>/tools/), a folder holding <chip>loader.bin, or one of the
 * SDK's loader files (the others are found beside it). */
static const char *loader_path(const char *loader, int chip) {
  static char p[1100], dir[1024];
  if (!loader)
    loader = getenv("JLNEWFW_LOADER");
  if (!loader)
    die("give the SDK (or a folder of <chip>loader.bin) with --loader or JLNEWFW_LOADER");
  if (!strcmp(base_name(loader), chips[chip].blob))
    return loader;
  snprintf(dir, sizeof dir, "%s", loader);
  struct stat st;
  char *slash = strrchr(dir, '/');
  if (!stat(loader, &st) && !S_ISDIR(st.st_mode) && slash)
    *slash = 0; /* a file: from its folder */
  const char *tries[] = {"%s/cpu/%s/tools/%s", "%s/%.0s%s", "%s/../../%s/tools/%s"};
  for (size_t i = 0; i < 3; i++) {
    snprintf(p, sizeof p, tries[i], dir, chips[chip].sdk_dir, chips[chip].blob);
    FILE *f = fopen(p, "rb");
    if (f) {
      fclose(f);
      return p;
    }
  }
  fprintf(stderr, "jlnewfw: no %s for the %s under %s\n", chips[chip].blob, chips[chip].chip, loader);
  die("give the SDK or a folder of loaders with --loader");
  return NULL;
}

static void start_loader(struct dev *d, const char *loader) {
  size_t n;
  uint8_t *blob = read_file(loader_path(loader, d->chip), &n);
  const uint32_t address = chips[d->chip].address;
  for (size_t off = 0; off < n; off += LOADER_BLOCK) {
    uint32_t m = (uint32_t)(n - off < LOADER_BLOCK ? n - off : LOADER_BLOCK);
    if (!jl_write(d, 0xfb06, address + (uint32_t)off, blob + off, m))
      die("could not write the loader into RAM");
  }
  if (getenv("JLNEWFW_DEBUG")) { /* what landed, as the ROM reads it back */
    uint8_t ra[6], got[32];
    be32(ra, address);
    ra[4] = 0, ra[5] = 32;
    if (jl_data(d, 0xfd07, ra, 6, SG_DXFER_FROM_DEV, got, 32)) {
      fprintf(stderr, "RAM at 0x%x:", address);
      for (int i = 0; i < 32; i++)
        fprintf(stderr, " %02x", got[i]);
      fprintf(stderr, "\n");
    }
  }
  free(blob);
  uint8_t a[6];
  be32(a, address);
  a[4] = 0, a[5] = LOADER_ARG;
  if (!jl_exec(d, 0xfb08, a, 6, NULL))
    die("could not start the loader");
  uint8_t r[14];
  bool up = false;
  for (int i = 0; i < 20 && !(up = jl_exec(d, 0xfc0a, NULL, 0, r)); i++)
    dev_sleep(100);
  if (!up)
    die("the loader does not answer");
  d->flash_id = r[2] | (uint32_t)r[3] << 8 | (uint32_t)r[4] << 16;
  d->flash_size = 1u << (d->flash_id & 0x1f); /* the capacity byte */
  d->io = 512;
  if (jl_exec(d, 0xfc14, NULL, 0, r) && rd32be(r) >= 64 && rd32be(r) <= 0x8000)
    d->io = rd32be(r);
  uint8_t ka[4];
  be32(ka, 0xac6900);
  if (!jl_exec(d, 0xfc09, ka, 4, r))
    die("could not read the chip key");
  uint8_t k[2] = {r[5], r[4]};
  crc_cipher(k, 2, 0xffffffffu);
  d->key = rd16(k);
  printf("%s: flash ID 0x%06x (%u KiB), chip key 0x%04x\n", d->path,
         d->flash_id, d->flash_size / 1024, d->key);
}

/* PID and chip key of the image whose header find_base() would pick: in a
 * file to be flashed, or in what the device holds. */
static bool image_ident(const uint8_t *p, size_t n, char pid[17], uint16_t *key) {
  uint32_t o = 0;
  uint8_t clear[32];
  static const uint32_t candidates[] = {0, 0x1000, 0x10000};
  bool found = false;
  for (size_t i = 0; i < 3 && !found; i++)
    if (header_at(p, n, candidates[i], clear))
      o = candidates[i], found = true;
  if (!found)
    return false;
  uint32_t e = eoffset(p, n, o) * 0x1000;
  if (e && e != o && header_at(p, n, e, clear))
    o = e;
  memcpy(pid, p + o + 16, 16);
  pid[16] = 0;
  for (char *q = pid; *q; q++)
    if ((uint8_t)*q == 0xff)
      *q = 0;
  for (uint32_t off = o + 32; key && off + 32 <= n && off < o + 0x400; off += 32) {
    uint8_t h[32];
    struct entry en;
    char name[17];
    memcpy(h, p + off, 32);
    jl_enc(h, 32, 0xffff);
    if (!decode_entry(h, &en))
      break;
    name_string(name, en.name);
    if (!strcmp(name, "isd_config.ini") && (uint64_t)o + en.offset + 34 <= n) {
      *key = record_key(p + o + en.offset);
      break;
    }
  }
  return true;
}

struct flash_opts {
  const char *device, *loader, *backup, *pid;
  unsigned wait;
  bool force_pid, force_key, dry_run, enter_loader;
  struct copy keep[16]; /* from the device: .to and .len */
  size_t nk;
};
static bool flash_arg(struct flash_opts *o, int argc, char **argv, int *i) {
  const char *a = argv[*i];
  if (!strcmp(a, "--force-pid"))
    return o->force_pid = true;
  if (!strcmp(a, "--force-key"))
    return o->force_key = true;
  if (!strcmp(a, "--dry-run"))
    return o->dry_run = true;
  if (!strcmp(a, "--enter-loader"))
    return o->enter_loader = true;
  if (*i + 1 >= argc)
    return false;
  if (!strcmp(a, "--device"))
    o->device = argv[++*i];
  else if (!strcmp(a, "--loader"))
    o->loader = argv[++*i];
  else if (!strcmp(a, "--pid"))
    o->pid = argv[++*i];
  else if (!strcmp(a, "--backup"))
    o->backup = argv[++*i];
  else if (!strcmp(a, "--wait"))
    o->wait = number(argv[++*i]);
  else if (!strcmp(a, "--keep") && o->nk < 16) {
    char buf[64], *c;
    snprintf(buf, sizeof buf, "%s", argv[++*i]);
    if (!(c = strchr(buf, ':')))
      die("--keep is ADDR:LEN");
    *c = 0;
    o->keep[o->nk].to = number(buf);
    o->keep[o->nk++].len = number(c + 1);
  } else
    return false;
  return true;
}


/* What a device's flash holds: the PID of the image its boot code takes. */
static void device_pid(struct dev *d, char pid[17]) {
  uint32_t head_n = d->flash_size < 0x11000 ? d->flash_size : 0x11000;
  uint8_t *head = malloc(head_n);
  pid[0] = 0;
  if (!head || !flash_read(d, 0, head, head_n))
    die("could not read the device's flash");
  if (!image_ident(head, head_n, pid, NULL))
    pid[0] = 0;
  free(head);
}

/* ---- choosing the device ------------------------------------------------------
 * Everything connected is surveyed first, with no side effect but the RAM
 * loader started in ROM loaders (to read their PID).  A candidate is a
 * device for the wanted PID (any, with --force-pid): a ROM loader holding
 * it, our firmware built for it, a stock pad of it, in any USB mode; Jieli
 * firmware of unknown PID only when nothing else is.  --device keeps those
 * on that USB port (or at that node).  More than one is refused before any
 * restarts.  The one left is restarted into its ROM loader, which comes back
 * on the same port as a new device: the next survey finds it. */

enum cand_kind { CAND_LOADER, CAND_OURS, CAND_JIELI, CAND_STOCK };
struct cand {
  enum cand_kind kind;
  char path[300], port[64], pid[17];
  int entry;                    /* CAND_STOCK: stock_hid[] */
  struct hid_dev *hid;          /* CAND_STOCK */
  struct dev *dev;              /* CAND_LOADER: open, its RAM loader running */
};

/* ROM loaders whose RAM loader runs, by node and enumeration. */
static struct {
  char path[300], id[USB_ID_MAX], pid[17];
  struct dev d;
} loaders[8];
static size_t n_loaders;

static struct dev *loader_at(const char *path, const char *blob, char pid[17]) {
  char id[USB_ID_MAX];
  usb_device(path, id);
  for (size_t i = 0; i < n_loaders; i++)
    if (!strcmp(loaders[i].path, path) && !strcmp(loaders[i].id, id)) {
      strcpy(pid, loaders[i].pid);
      return &loaders[i].d;
    }
  if (n_loaders == 8)
    die("too many ROM loaders");
  struct dev *d = &loaders[n_loaders].d;
  if (!open_dev(d, strdup(path)))
    return NULL;
  start_loader(d, blob);
  device_pid(d, pid);
  snprintf(loaders[n_loaders].path, 300, "%.299s", path);
  snprintf(loaders[n_loaders].id, USB_ID_MAX, "%s", id);
  strcpy(loaders[n_loaders].pid, pid);
  n_loaders++;
  return d;
}

static const char *cand_what(const struct cand *c, char buf[160]) {
  switch (c->kind) {
  case CAND_LOADER: return "ROM loader";
  case CAND_OURS: return "our firmware";
  case CAND_JIELI: return "Jieli firmware, PID not known";
  case CAND_STOCK:
    snprintf(buf, 160, stock_hid[c->entry].mode[0] ? "stock %s, %s mode" : "stock %s%s",
             stock_hid[c->entry].board, stock_hid[c->entry].mode);
    return buf;
  }
  return "";
}

/* On --device: its port or node. */
static bool cand_chosen(const struct cand *c, const char *device) {
  return !device || !strcmp(device, c->path) || (c->port[0] && !strcmp(device, c->port));
}

static size_t survey(const struct flash_opts *o, const char *want, struct cand *c, size_t max) {
  static struct hid_dev h[64];
  char paths[256][300], pid[17];
  size_t n = 0, np = device_paths(paths);
  for (size_t i = 0; i < np && n < max; i++) {
    if (restarting(paths[i]))
      continue;
    device_fd fd = dev_open(paths[i]);
    if (fd < 0)
      continue;
    int chip;
    enum disk_class k = classify_disk(fd, pid, &chip);
    dev_close(fd);
    struct cand *x = &c[n];
    memset(x, 0, sizeof *x);
    snprintf(x->path, sizeof x->path, "%.299s", paths[i]);
    if (!usb_port(x->path, x->port))
      x->port[0] = 0;
    if (!cand_chosen(x, o->device))
      continue;
    if (k == DISK_LOADER) {
      if (!(x->dev = loader_at(paths[i], o->loader, pid)))
        continue;
      x->kind = CAND_LOADER;
    } else if (k == DISK_OURS) {
      x->kind = CAND_OURS;
    } else if (k == DISK_JIELI) {
      x->kind = CAND_JIELI;
    } else
      continue;
    strcpy(x->pid, pid);
    if (want && k != DISK_JIELI && strcmp(pid, want)) {
      if (k == DISK_OURS && !told_before(x->path))
        printf("%s: our firmware for %s, left running\n", x->path, pid);
      continue;
    }
    n++;
  }
  size_t nh = hid_devices(h, 64);
  for (size_t i = 0; i < nh && n < max; i++) {
    int e = stock_hid_entry(&h[i]);
    if (e < 0 || (want && strcmp(want, stock_hid[e].pid)) || restarting(h[i].path))
      continue;
    struct cand *x = &c[n];
    memset(x, 0, sizeof *x);
    x->kind = CAND_STOCK, x->entry = e, x->hid = &h[i];
    snprintf(x->path, sizeof x->path, "%.299s", h[i].path);
    snprintf(x->pid, sizeof x->pid, "%s", stock_hid[e].pid);
    if (!usb_port(x->path, x->port))
      x->port[0] = 0;
    if (cand_chosen(x, o->device))
      n++;
  }
  /* A stock pad shows itself as several HID interfaces: one candidate per
   * pad, the one taking the command first. */
  for (size_t i = 0; i < n; i++)
    for (size_t j = i + 1; j < n; j++)
      if (c[i].kind == CAND_STOCK && c[j].kind == CAND_STOCK && c[i].port[0] &&
          !strcmp(c[i].port, c[j].port)) {
        if (!stock_hid[c[i].entry].usage_page && stock_hid[c[j].entry].usage_page)
          c[i] = c[j];
        c[j--] = c[--n];
      }
  /* Jieli firmware of unknown PID only when nothing else fits. */
  size_t known = 0;
  for (size_t i = 0; i < n; i++)
    known += c[i].kind != CAND_JIELI;
  if (known && known < n)
    for (size_t i = 0; i < n; i++)
      if (c[i].kind == CAND_JIELI)
        c[i--] = c[--n];
  return n;
}

static void select_dev(struct dev *out, const struct flash_opts *o, const char *want) {
  struct cand c[16];
  char buf[160];
  if (want && !want[0])
    want = NULL;
  if (o->force_pid)
    want = NULL;
  for (unsigned t = 0;; t++) {
    size_t n = survey(o, want, c, 16);
    if (n > 1) {
      fprintf(stderr, "jlnewfw: more than one device %s%s:\n", want ? "for " : "",
              want ? want : "connected");
      for (size_t i = 0; i < n; i++)
        fprintf(stderr, "  %-24s %s%s%s  (%s)\n", c[i].port[0] ? c[i].port : c[i].path,
                cand_what(&c[i], buf), c[i].pid[0] ? ", " : "", c[i].pid, c[i].path);
      die("pick one with --device PORT (make: DEVICE=PORT); jlnewfw scan lists them");
    }
    if (n == 1) {
      struct cand *x = &c[0];
      if (x->kind == CAND_LOADER) {
        printf("%s: ROM loader holding %s\n", x->path, x->pid[0] ? x->pid : "no firmware header");
        *out = *x->dev;
        return;
      }
      if (!o->enter_loader) {
        if (!told_before(x->path))
          printf("%s: %s%s%s; not in its ROM loader (--enter-loader)\n", x->path,
                 cand_what(x, buf), x->pid[0] ? ", " : "", x->pid);
      } else if (x->kind == CAND_STOCK && !stock_hid[x->entry].usage_page) {
        if (!told_before(x->path)) {
          const char *mode = NULL;
          for (size_t k = 0; k < STOCK_HID_COUNT && !mode; k++)
            if (!strcmp(stock_hid[k].pid, x->pid) && stock_hid[k].usage_page)
              mode = stock_hid[k].mode;
          if (mode)
            printf("%s: %s, which has no command for its loader: switch it to %s\n",
                   x->path, cand_what(x, buf), mode);
          else
            printf("%s: %s, which has no command for its loader: %s\n", x->path,
                   cand_what(x, buf), stock_hid[x->entry].by_hand);
        }
      } else {
        bool sent = true;
        printf("%s: %s, asking it to restart into its ROM loader\n", x->path, cand_what(x, buf));
        if (x->kind == CAND_STOCK) {
          sent = send_stock_hid(x->hid, x->entry);
        } else {
          uint8_t cmd[16], r[16];
          device_fd fd = dev_open(x->path);
          memset(cmd, 0xff, 16);
          cmd[0] = 0xfc, cmd[1] = 0x0a; /* running firmware: any loader command */
          if (fd >= 0) {
            sg(fd, cmd, 16, SG_DXFER_FROM_DEV, r, 16, 2000);
            dev_close(fd);
          }
        }
        if (sent && n_asked < 16)
          usb_device(x->path, asked[n_asked++]);
      }
    }
    if (t >= o->wait * 2) {
      fprintf(stderr, "jlnewfw: no ROM loader %s%s\n", want ? "holding " : "found",
              want ? want : "");
      die(n ? "the device did not come up in its ROM loader"
            : "no device for it found (jlnewfw scan lists what is connected)");
    }
    if (!t && o->wait && !n)
      printf("waiting up to %u s for a device %s%s\n", o->wait, want ? "for " : "", want ? want : "");
    dev_sleep(500);
  }
}

/* What is connected, by USB port (what --device and DEVICE= take); nothing
 * is restarted, and a ROM loader's PID is not read. */
static void cmd_scan(void) {
  static struct hid_dev h[64];
  char paths[256][300], pid[17], port[64], seen[64][64];
  size_t np = device_paths(paths), ns = 0;
  static const char *const names[] = {"", "ROM loader", "our firmware", "Jieli firmware"};
  for (size_t i = 0; i < np; i++) {
    device_fd fd = dev_open(paths[i]);
    if (fd < 0)
      continue;
    int chip;
    enum disk_class k = classify_disk(fd, pid, &chip);
    dev_close(fd);
    if (k == DISK_OTHER)
      continue;
    if (!usb_port(paths[i], port))
      snprintf(port, sizeof port, "-");
    printf("%-24s %s (%s)%s%s  (%s)\n", port, names[k], chips[chip].chip, pid[0] ? ", " : "", pid,
           paths[i]);
  }
  size_t nh = hid_devices(h, 64);
  for (int pass = 0; pass < 2; pass++) /* the interface taking a command first */
    for (size_t i = 0; i < nh; i++) {
      int e = stock_hid_entry(&h[i]);
      if (e < 0 || (stock_hid[e].usage_page == 0) != pass)
        continue;
      bool dup = false;
      if (!usb_port(h[i].path, port))
        snprintf(port, sizeof port, "-");
      for (size_t k = 0; k < ns && !dup; k++)
        dup = port[0] != '-' && !strcmp(seen[k], port);
      if (dup)
        continue;
      if (ns < 64)
        snprintf(seen[ns++], sizeof seen[0], "%s", port);
      printf("%-24s stock %s%s%s, %s%s%s  (%s)\n", port, stock_hid[e].board,
             stock_hid[e].mode[0] ? ", " : "", stock_hid[e].mode, stock_hid[e].pid,
             stock_hid[e].usage_page ? "" : stock_hid[e].by_hand ? "; by hand: " : "; no loader command in this mode",
             stock_hid[e].by_hand && !stock_hid[e].usage_page ? stock_hid[e].by_hand : "", h[i].path);
    }
}

static void erase(struct dev *d, uint32_t from, uint32_t to) {
  uint8_t a[4];
  for (uint32_t at = from; at < to;) {
    bool block = !(at & 0xffff) && to - at >= 0x10000;
    be32(a, at);
    if (!jl_exec(d, block ? 0xfb00 : 0xfb01, a, 4, NULL))
      die("erase failed");
    at += block ? 0x10000 : 0x1000;
  }
}

static void flash_image(struct dev *d, const struct flash_opts *o,
                        uint8_t *data, size_t n) {
  char want[17], have[17] = "";
  uint16_t want_key = 0xffff;
  if (!image_ident(data, n, want, &want_key))
    die("no flash header in what is to be written");
  device_pid(d, have);
  printf("image: %s, chip key 0x%04x; device holds %s\n", want, want_key,
         have[0] ? have : "no firmware header");
  if (strcmp(want, have)) {
    if (!o->force_pid)
      die("PID mismatch: the image is for another device (--force-pid to "
          "write it anyway)");
    printf("PID mismatch, written anyway (--force-pid)\n");
  }
  if (want_key != d->key) {
    if (!o->force_key)
      die("the image is encrypted for another chip key: pack it with -k "
          "(flashpack does), or --force-key");
    printf("chip key mismatch, written anyway (--force-key)\n");
  }
  if (n > d->flash_size)
    die("the image is larger than the flash");
  if (o->backup) {
    uint8_t *all = malloc(d->flash_size);
    if (!all || !flash_read(d, 0, all, d->flash_size))
      die("could not read the flash for --backup");
    write_file(o->backup, all, d->flash_size);
    printf("backup: %s\n", o->backup);
    free(all);
  }
  if (o->dry_run) {
    printf("dry run: nothing written\n");
    return;
  }
  /* Ranges kept from the device: written back with the image. */
  size_t end = n;
  for (size_t i = 0; i < o->nk; i++)
    if (o->keep[i].to + o->keep[i].len > end)
      end = o->keep[i].to + o->keep[i].len;
  if (end > d->flash_size)
    die("--keep reaches past the flash");
  uint8_t *w = malloc(end);
  if (!w)
    die("out of memory");
  memset(w, 0xff, end);
  memcpy(w, data, n);
  for (size_t i = 0; i < o->nk; i++)
    if (!flash_read(d, o->keep[i].to, w + o->keep[i].to, o->keep[i].len))
      die("could not read a --keep range");
  uint32_t top = align_up((uint32_t)end, 0x1000);
  printf("erasing 0x0-0x%x, writing 0x%x bytes\n", top, (unsigned)end);
  erase(d, 0, top);
  for (uint32_t at = 0; at < end;) {
    uint32_t m = (uint32_t)(end - at < d->io ? end - at : d->io);
    bool blank = true;
    for (uint32_t i = 0; i < m && blank; i++)
      blank = w[at + i] == 0xff;
    if (!blank && !jl_write(d, 0xfb04, at, w + at, m))
      die("write failed");
    at += m;
  }
  uint8_t *back = malloc(end);
  if (!back || !flash_read(d, 0, back, (uint32_t)end) || memcmp(back, w, end))
    die("readback differs: the device is left in its loader");
  printf("written and verified\n");
  uint8_t a[4];
  be32(a, 1);
  jl_exec(d, 0xfc0c, a, 4, NULL); /* run: no answer expected */
  free(back);
  free(w);
}

/* Full raw backup; requires an explicit board PID or device path. */
static void cmd_backup(int argc, char **argv) {
  struct flash_opts o = {0};
  const char *output = NULL;
  for (int i = 0; i < argc; i++) {
    if (flash_arg(&o, argc, argv, &i)) continue;
    else if (argv[i][0] != '-' && !output) output = argv[i];
    else die("backup: unexpected argument");
  }
  const char *pid = o.pid;
  if (!output || (!pid && !o.device))
    die("usage: jlnewfw backup --pid PID [FLASH OPTIONS] OUTPUT");
  struct dev d;
  select_dev(&d, &o, pid ? pid : "");
  if (pid) {
    char have[17];
    device_pid(&d, have);
    if (strcmp(pid, have)) die("backup: board PID mismatch");
  }
  uint8_t *all = malloc(d.flash_size);
  if (!all || !flash_read(&d, 0, all, d.flash_size)) die("backup: flash read failed");
  write_file(output, all, d.flash_size);
  printf("backup: %s (%u bytes); reconnect USB to leave the loader\n", output, d.flash_size);
  free(all);
  dev_close(d.fd);
}

/* Running firmware for PID restarts into its ROM loader, which stays until
 * USB is reconnected. */
static void cmd_loader(int argc, char **argv) {
  struct flash_opts o = {.wait = 30};
  for (int i = 0; i < argc; i++)
    if (!flash_arg(&o, argc, argv, &i))
      die("usage: jlnewfw loader --pid PID [--device PATH] [--wait SECONDS] [--loader BLOB]");
  if (!o.pid)
    die("usage: jlnewfw loader --pid PID [--device PATH] [--wait SECONDS] [--loader BLOB]");
  o.enter_loader = true;
  struct dev d = {0};
  select_dev(&d, &o, o.pid);
  printf("%s: in its ROM loader, holding %s; reconnect USB to leave it\n", d.path, o.pid);
  dev_close(d.fd);
}

static void cmd_flash(int argc, char **argv) {
  struct flash_opts o = {0};
  const char *image = NULL;
  for (int i = 0; i < argc; i++) {
    if (flash_arg(&o, argc, argv, &i))
      continue;
    if (argv[i][0] == '-' || image)
      die("usage: jlnewfw flash [options] IMAGE");
    image = argv[i];
  }
  if (!image)
    die("usage: jlnewfw flash [options] IMAGE");
  size_t n;
  uint8_t *data = load_image(image, &n);
  char want[17];
  if (!image_ident(data, n, want, NULL))
    die("no flash header in what is to be written");
  if (o.pid && strcmp(o.pid, want))
    die("image PID does not match the selected board");
  struct dev d = {0};
  select_dev(&d, &o, want);
  flash_image(&d, &o, data, n);
}

static void cmd_flashpack(int argc, char **argv) {
  struct pack_opts p = {0};
  struct flash_opts o = {0};
  for (int i = 0; i < argc; i++) {
    if (pack_arg(&p, argc, argv, &i) || flash_arg(&o, argc, argv, &i))
      continue;
    if (argv[i][0] == '-')
      die("unknown flashpack option");
    if (p.nf == MAX_FILES)
      die("too many app files");
    p.files[p.nf++] = argv[i];
  }
  if (!p.ini)
    die("flashpack needs -c isd_config.ini");
  struct config c = {0};
  load_config(&c, p.ini);
  char want[17];
  snprintf(want, sizeof want, "%s", cfg(&c, "EXTRA_CFG_PARAM", "PID", ""));
  if (o.pid && strcmp(o.pid, want))
    die("image PID does not match the selected board");
  struct dev d = {0};
  select_dev(&d, &o, want);
  if (!p.have_key)
    p.key = d.key, p.have_key = true; /* for the chip in front of us */
  struct blob b = build(&p, NULL);
  if (p.out)
    write_file(p.out, b.p, b.n);
  flash_image(&d, &o, b.p, b.n);
}
#else
static void cmd_flash(int argc, char **argv) {
  (void)argc, (void)argv;
  die("device access supports Linux and Windows");
}
static void cmd_flashpack(int argc, char **argv) { cmd_flash(argc, argv); }
static void cmd_backup(int argc, char **argv) { cmd_flash(argc, argv); }
static void cmd_loader(int argc, char **argv) { cmd_flash(argc, argv); }
static void cmd_scan(void) { cmd_flash(0, NULL); }
#endif

static void usage(void) {
  puts(
      "usage:\n"
      "  jlnewfw list IMAGE\n"
      "  jlnewfw verify IMAGE\n"
      "  jlnewfw unpack IMAGE DIRECTORY\n"
      "  jlnewfw pack -c isd_config.ini [-u uboot.boot] -o OUT [app.bin "
      "[cfg_tool.bin ...]]\n"
      "               [-r RESOURCE]... [-k KEY] [--flash-size SIZE] "
      "[--code-limit ADDR]\n"
      "               [--at ADDR] [--mark VALUE] [--copy FROM:TO:LEN]...\n"
      "  jlnewfw backup --pid PID [FLASH OPTIONS] OUTPUT\n"
      "  jlnewfw loader --pid PID [--device PATH] [--wait SECONDS] [--loader BLOB]\n"
      "  jlnewfw scan\n"
      "  jlnewfw flash [FLASH OPTIONS] IMAGE\n"
      "  jlnewfw flashpack [PACK OPTIONS] [FLASH OPTIONS] [app.bin ...]\n"
      "FLASH OPTIONS: [--pid PID] [--device PATH] [--loader SDK/cpu/br23/tools/br23loader.bin] "
      "[--backup FILE]\n"
      "               [--keep ADDR:LEN]... [--force-pid] [--force-key] "
      "[--dry-run]\n"
      "               [--wait SECONDS] [--enter-loader]\n"
      "\n"
      "An IMAGE is read from the header the boot code would take (EOFFSET is\n"
      "followed).  unpack writes the files and an isd_config.ini that pack\n"
      "takes back: unpack, then pack, rebuilds the image, changed by options:\n"
      "  -k        the chip key to encrypt for (default: the INI's, or 0xffff)\n"
      "  --at      flash address of the image (default: EOFFSET x 4 KiB);\n"
      "            PRCT and VM follow it.  OUT starts at flash address 0.\n"
      "  --copy    also put image bytes FROM (offset in the image) at TO\n"
      "  --mark    the app directory's data-CRC field (unused by the format)\n"
      "flash writes OUT through the ROM loader: erased and written from 0,\n"
      "read back, then the chip runs.  The image's PID must be the one on\n"
      "the device and its chip key the chip's (--force-pid, --force-key).\n"
      "Without --device the device is the one in its ROM loader whose flash\n"
      "holds the image's PID (any, with --force-pid); more than one is\n"
      "refused.  --enter-loader asks running Jieli firmware that shows\n"
      "itself as a disk (ours only for the image's PID), and stock pads known\n"
      "to take a HID command, to restart into the ROM loader throughout\n"
      "--wait; a stock pad in a mode without one is told which mode has it.\n"
      "loader does only that, and waits for the loader holding PID.  scan\n"
      "lists ROM loaders, our firmware, other Jieli firmware and stock pads.\n"
      "--keep writes back ranges of what the device held; --dry-run checks\n"
      "only.  flashpack packs for the chip's key and flashes; -o also saves\n"
      "what it wrote.");
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0); /* progress before any error */
  if (argc < 2) {
    usage();
    return 2;
  }
  if (!strcmp(argv[1], "pack")) {
    cmd_pack(argc - 2, argv + 2);
    return 0;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!strcmp(argv[1], "backup")) {
    cmd_backup(argc - 2, argv + 2);
    return 0;
  }
  if (!strcmp(argv[1], "scan") && argc == 2) {
    cmd_scan();
    return 0;
  }
  if (!strcmp(argv[1], "loader")) {
    cmd_loader(argc - 2, argv + 2);
    return 0;
  }
  if (!strcmp(argv[1], "flash")) {
    cmd_flash(argc - 2, argv + 2);
    return 0;
  }
  if (!strcmp(argv[1], "flashpack")) {
    cmd_flashpack(argc - 2, argv + 2);
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
