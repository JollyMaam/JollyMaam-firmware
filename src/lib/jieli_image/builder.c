#define _POSIX_C_SOURCE 200809L
#include "builder.h"
#include "crypto.h"
#include "entry.h"
#include "key.h"
#include "bankcb.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define MAX_FILES JM_JIELI_BUILD_FILES
#define MAX_CFG 128
#define MAIN_MAGIC 0x4e49414du
#define MAIN_HEAD_SIZE 0x120u
#define MAIN_HEAD_VERSION 1u
struct blob { uint8_t *p; size_t n,cap; };
struct kv { char *section,*key,*value; };
struct config { struct kv v[MAX_CFG];size_t n; };
struct app_file { const char *path,*name;uint8_t *data;size_t size; };
struct build_context {
  jmp_buf error_jump;
  char message[256];
  void *allocations[1024];size_t allocations_count;
  jm_jieli_read_file read;jm_jieli_build_log log;void *user;
  uint8_t *update;size_t update_size;
};
static void fail(struct build_context *ctx,const char *format,...) {
  va_list args;va_start(args,format);vsnprintf(ctx->message,sizeof ctx->message,format,args);va_end(args);
  longjmp(ctx->error_jump,1);
}
static void *bmalloc(struct build_context *ctx,size_t size) {
  if (size>64*1024*1024 || ctx->allocations_count==1024) fail(ctx,"allocation exceeds builder limits");
  void *p=malloc(size?size:1);if(!p) fail(ctx,"out of memory");
  ctx->allocations[ctx->allocations_count++]=p;return p;
}
static void bfree(struct build_context *ctx,void *p) {
  if(!p) return;
  for(size_t i=0;i<ctx->allocations_count;++i) if(ctx->allocations[i]==p) {
    free(p);ctx->allocations[i]=NULL;return;
  }
}
static void *brealloc(struct build_context *ctx,void *p,size_t size) {
  if(!p) return bmalloc(ctx,size);
  if(size>64*1024*1024) fail(ctx,"allocation exceeds builder limits");
  for(size_t i=0;i<ctx->allocations_count;++i) if(ctx->allocations[i]==p) {
    void *grown=realloc(p,size?size:1);if(!grown) fail(ctx,"out of memory");
    ctx->allocations[i]=grown;return grown;
  }
  fail(ctx,"invalid builder allocation");return NULL;
}
static char *bstrdup(struct build_context *ctx,const char *s) {
  size_t n=strlen(s)+1;char *p=bmalloc(ctx,n);memcpy(p,s,n);return p;
}
static void blog(struct build_context *ctx,const char *format,...) {
  if(!ctx->log) return;
  char text[1024];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
  ctx->log(ctx->user,text);
}
static uint8_t *read_file(struct build_context *ctx,const char *name,size_t *size) {
  const uint8_t *data=NULL;size_t n=0;
  if(!ctx->read(ctx->user,name,&data,&n) || (!data && n) || n>32*1024*1024) fail(ctx,"cannot read input: %s",name);
  uint8_t *copy=bmalloc(ctx,n);if(n) memcpy(copy,data,n);*size=n;return copy;
}
static void load_config(struct build_context *ctx,struct config *config,const char *name);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }



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

static const char *cfg(const struct config *c, const char *sec, const char *key,
                       const char *def) {
  for (size_t i = 0; i < c->n; i++)
    if (!strcmp(c->v[i].section, sec) && !strcmp(c->v[i].key, key))
      return c->v[i].value;
  return def;
}

static bool numeric(const char *s) {
  char *e;
  (void)strtoul(s, &e, 0);
  while (isspace((unsigned char)*e))
    e++;
  return *s && !*e;
}

static void make_chipkey(uint8_t x[34], uint16_t key) {
  memset(x, 8, 16); /* sum=128 */
  for (unsigned i = 0; i < 16; i++)
    x[16 + i] = (uint8_t)(8 ^ ((key & (1u << i)) ? 0 : 0x80));
  wr16(x + 32, jm_crc16(x, 32));
}

static void set_name(uint8_t name[16], const char *s) {
  memset(name, 0xff, 16);
  size_t n = strlen(s);
  if (n > 15)
    n = 15;
  memcpy(name, s, n);
  name[n] = 0;
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

static void reserve(struct build_context *ctx, struct blob *b, size_t n) {
  if (n <= b->cap)
    return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < n)
    cap *= 2;
  b->p = brealloc(ctx, b->p, cap);
  if (!b->p)
    fail(ctx, "out of memory");
  b->cap = cap;
}

static void append(struct build_context *ctx, struct blob *b, const void *p, size_t n) {
  reserve(ctx, b, b->n + n);
  memcpy(b->p + b->n, p, n);
  b->n += n;
}

static void append_u8(struct build_context *ctx, struct blob *b, uint8_t x) { append(ctx, b, &x, 1); }

static uint32_t number(struct build_context *ctx, const char *s) {
  char *end; errno=0; unsigned long long n=strtoull(s,&end,0);
  if (end==s || errno || *s=='-') fail(ctx, "not a number: %s",s);
  while (isspace((unsigned char)*end)) ++end;
  uint64_t factor=1;
  if (*end=='K' || *end=='k') { factor=1024;++end; }
  else if (*end=='M' || *end=='m') { factor=1024*1024;++end; }
  if (*end || n>UINT32_MAX/factor) fail(ctx, "invalid or excessive number: %s",s);
  return (uint32_t)(n*factor);
}

static void require_value(struct build_context *ctx, const struct config *c, const char *section, const char *key, const char *expected) {
  const char *actual=cfg(c,section,key,expected);
  if (strcasecmp(actual,expected)) fail(ctx, "unsupported %s.%s=%s (expected %s)",section,key,actual,expected);
}

static struct blob encode_syscfg(struct build_context *ctx, const struct config *c,
                                 const uint8_t record[34]) {
  struct blob b = {0};
  append(ctx, &b, record, 34);
  for (size_t i = 0; i < c->n; i++)
    if (!strcmp(c->v[i].section, "SYS_CFG_PARAM")) {
      char key[256], val[512];
      snprintf(key, sizeof key, "%s", c->v[i].key);
      snprintf(val, sizeof val, "%s", c->v[i].value);
      upper(val);
      if (numeric(val)) {
        append_u8(ctx, &b, 4);
        append(ctx, &b, key, strlen(key) + 1);
        uint8_t n[4];
        wr32(n, number(ctx, val));
        append(ctx, &b, n, 4);
      } else {
        size_t n = strlen(val);
        if (n > 255)
          fail(ctx, "SYS_CFG_PARAM string too long");
        append_u8(ctx, &b, (uint8_t)n);
        append(ctx, &b, key, strlen(key) + 1);
        append(ctx, &b, val, n);
      }
    }
  return b;
}

static void flash_header(struct build_context *ctx, uint8_t out[32], const struct config *c,
                         uint32_t flash_size) {
  memset(out, 0xff, 32);
  const char *vid = cfg(c, "EXTRA_CFG_PARAM", "VID", "0.01"),
             *pid = cfg(c, "EXTRA_CFG_PARAM", "PID", "JL_DEVICE");
  memcpy(out + 4, vid, strlen(vid) > 4 ? 4 : strlen(vid));
  memcpy(out + 16, pid, strlen(pid) > 16 ? 16 : strlen(pid));
  jm_jieli_enc(out, 32, 0xffff); /* enter the header's CRC domain */
  wr16(out + 2, 544);
  wr32(out + 8, flash_size);
  out[12] = (uint8_t)number(ctx, cfg(c, "JLNEWFW", "HEADER_FS", "2")); /* BD19: 11 */
  out[13] = 16;
  uint32_t special = number(ctx, cfg(c, "EXTRA_CFG_PARAM", "SPECIAL_OPT", "0"));
  out[15] = special ? (uint8_t)special : 0xff;
  const char *raw = cfg(c, "JLNEWFW", "HEADER_SPECIAL", NULL);
  if (raw)
    out[15] = (uint8_t)number(ctx, raw);
  wr16(out, jm_crc16(out + 2, 30));
  jm_jieli_enc(out, 32, 0xffff);
}

static void scramble_bankcb(struct build_context *ctx, uint8_t *p, size_t n) {
  if (!jm_jieli_scramble_bankcb(p, n))
    fail(ctx, "invalid BANKCB image");
}

static void parse_copy(struct build_context *ctx, const char *s, struct jm_jieli_copy *c) {
  char buf[128], *a, *b;
  snprintf(buf, sizeof buf, "%s", s);
  a = strchr(buf, ':');
  b = a ? strchr(a + 1, ':') : NULL;
  if (!a || !b)
    fail(ctx, "a copy is FROM:TO:LEN");
  *a = *b = 0;
  c->from = number(ctx, trim(buf));
  c->to = number(ctx, trim(a + 1));
  c->len = number(ctx, trim(b + 1));
}

static const char *beside(struct build_context *ctx, const char *ini, const char *name) {
  if (name[0] == '/')
    return bstrdup(ctx, name);
  const char *slash = strrchr(ini, '/');
  if (!slash)
    return bstrdup(ctx, name);
  char *p = bmalloc(ctx, (size_t)(slash - ini) + 2 + strlen(name));
  if (!p)
    fail(ctx, "out of memory");
  sprintf(p, "%.*s/%s", (int)(slash - ini), ini, name);
  return p;
}

static size_t ini_list(struct build_context *ctx, const char *ini, const char *v, const char **out) {
  size_t n = 0;
  char *copy = bstrdup(ctx, v), *save = NULL;
  for (char *t = strtok_r(copy, ",", &save); t && n < MAX_FILES;
       t = strtok_r(NULL, ",", &save))
    if (*trim(t))
      out[n++] = beside(ctx, ini, trim(t));
  return n;
}

static struct blob build(struct build_context *ctx, struct jm_jieli_build_options *o, uint16_t *key_out) {
  if (!o->ini)
    fail(ctx, "pack needs -c isd_config.ini");
  struct config c = {0};
  load_config(ctx, &c, o->ini);
  /* These are the layout modes understood below. Refuse rather than silently
     producing a plausible-looking image for a different SDK configuration. */
  require_value(ctx, &c, "EXTRA_CFG_PARAM", "NEW_FLASH_FS", "YES");
  require_value(ctx, &c, "EXTRA_CFG_PARAM", "FORCE_4K_ALIGN", "YES");
  require_value(ctx, &c, "EXTRA_CFG_PARAM", "RESERVED_OPT", "0");
  require_value(ctx, &c, "RESERVED_CONFIG", "PRCT_ADR", "0");
  require_value(ctx, &c, "RESERVED_CONFIG", "PRCT_LEN", "CODE_LEN");
  require_value(ctx, &c, "RESERVED_CONFIG", "PRCT_OPT", "2");
  require_value(ctx, &c, "RESERVED_CONFIG", "VM_ADR", "0");
  require_value(ctx, &c, "RESERVED_CONFIG", "VM_OPT", "1");
  require_value(ctx, &c, "RESERVED_CONFIG", "BTIF_ADR", "AUTO");
  require_value(ctx, &c, "RESERVED_CONFIG", "BTIF_OPT", "1");
  const char *v;
  if (!o->uboot && (v = cfg(&c, "JLNEWFW", "UBOOT", NULL)))
    o->uboot = beside(ctx, o->ini, v);
  if (!o->nf && (v = cfg(&c, "JLNEWFW", "FILES", NULL)))
    o->nf = ini_list(ctx, o->ini, v, o->files);
  if (!o->nr && (v = cfg(&c, "JLNEWFW", "RES", NULL)))
    o->nr = ini_list(ctx, o->ini, v, o->res);
  if (!o->main_file && (v=cfg(&c,"JLNEWFW","MAIN_FILE",NULL))) {
    o->main_file=beside(ctx,o->ini,v);
    o->main_at=number(ctx,cfg(&c,"JLNEWFW","MAIN_AT","0"));
  }
  if (!o->have_key && (v = cfg(&c, "JLNEWFW", "CHIP_KEY", NULL)))
    o->key = (uint16_t)number(ctx, v), o->have_key = true;
  if (!o->have_key)
    o->key = 0xffff;
  if (!o->flash_size && (v = cfg(&c, "JLNEWFW", "FLASH_SIZE", NULL)))
    o->flash_size = number(ctx, v);
  if (!o->code_limit && (v = cfg(&c, "JLNEWFW", "CODE_LIMIT", NULL)))
    o->code_limit = number(ctx, v);
  if (!o->have_mark && (v = cfg(&c, "JLNEWFW", "MARK", NULL)))
    o->mark = (uint16_t)number(ctx, v), o->have_mark = true;
  if (!o->nc && (v = cfg(&c, "JLNEWFW", "COPY", NULL))) {
    char *copy = bstrdup(ctx, v), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t && o->nc < 16;
         t = strtok_r(NULL, ",", &save))
      parse_copy(ctx, t, &o->copies[o->nc++]);
  }
  if (!o->have_at) {
    uint32_t units=number(ctx, cfg(&c,"SYS_CFG_PARAM","EOFFSET","0"));
    if (units>16384) fail(ctx, "EOFFSET outside supported flash");
    o->at=units*0x1000;
  }
  if (o->at>0x4000000 || o->main_at>0x4000000 || o->flash_size>0x4000000 || o->code_limit>0x4000000)
    fail(ctx, "layout exceeds 64 MiB limit");
  if (!o->uboot || !o->nf)
    fail(ctx, "pack needs uboot.boot (-u) and app files");
  if (strcmp(base_name(o->files[0]), "app.bin"))
    fail(ctx, "the first application file must be named app.bin");
  for (size_t i = 0; i < o->nf + o->nr; i++)
    if (strlen(base_name(i < o->nf ? o->files[i] : o->res[i - o->nf])) > 15)
      fail(ctx, "JLFS file names may be at most 15 bytes");

  uint8_t record[34];
  if ((v = cfg(&c, "JLNEWFW", "CHIP_KEY_RECORD", NULL))) {
    if (strlen(v) != 68)
      fail(ctx, "CHIP_KEY_RECORD is 34 bytes in hex");
    for (unsigned i = 0; i < 34; i++) {
      char h[3] = {v[2 * i], v[2 * i + 1], 0};
      record[i] = (uint8_t)strtoul(h, NULL, 16);
    }
    if (jm_crc16(record, 32) != rd16(record + 32))
      fail(ctx, "CHIP_KEY_RECORD CRC mismatch");
    jm_jieli_set_record_key(record, o->key);
  } else {
    make_chipkey(record, o->key);
  }
  struct blob sys = encode_syscfg(ctx, &c, record);
  size_t uboot_n;
  uint8_t *uboot = read_file(ctx, o->uboot, &uboot_n);
  struct app_file files[2 * MAX_FILES];
  uint8_t area_flags[2 * MAX_FILES]; /* resources: 0x83, or 0x82 for "name:82" */
  size_t nall = o->nf + o->nr;
  for (size_t i = 0; i < nall; i++) {
    char *path = bstrdup(ctx, i < o->nf ? o->files[i] : o->res[i - o->nf]);
    size_t pn = strlen(path);
    area_flags[i] = 0x83;
    if (i >= o->nf && pn > 3 && !strcmp(path + pn - 3, ":82")) {
      path[pn - 3] = 0;
      area_flags[i] = 0x82;
    }
    files[i] = (struct app_file){path, base_name(path), NULL, 0};
    files[i].data = read_file(ctx, path, &files[i].size);
  }
  uint32_t appbase =
      align_up((uint32_t)(32 + 4 * 32 + uboot_n + sys.n), 0x1000);
  uint32_t entry = number(ctx, cfg(&c, "EXTRA_CFG_PARAM", "ENTRY", "0x1e00120"));
  uint32_t appoff = entry & 0xfffffu;
  /* The reserved list as unpack found it (AREAS: offset:size:flags:reserved:
   * CRC field:name field, 16 bytes in hex), or else VM, PRCT and BTIF from the
   * layout. */
  struct jm_jieli_entry areas[MAX_FILES];
  size_t nareas = 0;
  if ((v = cfg(&c, "JLNEWFW", "AREAS", NULL)) && *v) {
    char *copy = bstrdup(ctx, v), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t && nareas < MAX_FILES;
         t = strtok_r(NULL, ",", &save)) {
      unsigned off, size, flags, res, crc;
      char hex[33];
      if (sscanf(trim(t), "%x:%x:%x:%x:%x:%32s", &off, &size, &flags, &res, &crc, hex) != 6 ||
          strlen(hex) != 32)
        fail(ctx, "AREAS entry is offset:size:flags:reserved:crc:name-hex");
      struct jm_jieli_entry *a = &areas[nareas++];
      *a = (struct jm_jieli_entry){(uint16_t)crc, off, size, (uint8_t)flags, (uint8_t)res, 0, {0}};
      for (unsigned k = 0; k < 16; k++) {
        unsigned b;
        sscanf(hex + 2 * k, "%2x", &b);
        a->name[k] = (uint8_t)b;
      }
    }
    bfree(ctx, copy);
    if (!nareas) fail(ctx,"empty AREAS list");
    areas[nareas - 1].index = 1;
  }
  size_t nested_n = o->nf + (nareas ? nareas : 3);
  if (appoff < 32 + (uint32_t)nested_n * 32)
    fail(ctx, "ENTRY's flash offset overlaps the app-area headers");
  uint32_t cursor = appoff;
  for (size_t i = 0; i < o->nf; i++) {
    if (files[i].size > UINT32_MAX - cursor)
      fail(ctx, "app files too large");
    cursor += (uint32_t)files[i].size;
  }
  uint32_t outer_size = cursor; /* relative to the outer header */
  uint32_t res_size = 0;        /* resources follow, each behind its header */
  for (size_t i = o->nf; i < nall; i++)
    res_size += 32 + (uint32_t)files[i].size;
  /* --main: the main app, linked at the code window's start + OFFSET +
   * 0x120, behind a header the updater checks; the area's cipher runs on
   * over it.  The app area itself holds the updater. */
  size_t main_n = 0;
  uint8_t *main_bin = o->main_file ? read_file(ctx, o->main_file, &main_n) : NULL;
  uint32_t area_n = outer_size + res_size;
  if (main_bin) {
    if ((o->main_at & 0xfff) || o->main_at < area_n)
      fail(ctx, "--main's offset must be 4-KiB aligned and above the app area");
    area_n = o->main_at + MAIN_HEAD_SIZE + (uint32_t)main_n;
  }
  uint32_t image_end = align_up(appbase + area_n, 0x1000);
  uint32_t code_end = o->code_limit ? o->code_limit : image_end;
  if ((code_end & 0xfff) || code_end < image_end)
    fail(ctx, "code limit must be 4-KiB aligned and contain the application");
  uint32_t vm_len = number(ctx, cfg(&c, "RESERVED_CONFIG", "VM_LEN", "8K"));
  uint32_t btif_len = number(ctx, cfg(&c, "RESERVED_CONFIG", "BTIF_LEN", "0x1000"));
  /* With the flash size given, VM is what is left between the code and
   * BTIF (GameSir's tool leaves a pad placed at 0x1000 28 of VM_LEN's 32
   * KiB); otherwise the flash is made big enough for VM_LEN. */
  if (vm_len>0x4000000 || btif_len>0x4000000) fail(ctx, "reserved region exceeds flash limit");
  uint32_t required = o->at + code_end + (o->flash_size ? 0 : vm_len) +
                      btif_len + 0x1000;
  uint32_t total = o->flash_size ? o->flash_size : next_pow2(required);
  if (!o->flash_size && total < 0x20000)
    total = 0x20000;
  if (total>0x4000000 || image_end>0x4000000) fail(ctx, "image exceeds 64 MiB limit");
  if (total < required)
    fail(ctx, "flash size is too small for the image and reserved regions");
  if (total & (total - 1))
    fail(ctx, "flash size must be a power of two");
  uint32_t keymac = total - 0x1000, btif = keymac - btif_len,
           vm = o->at + code_end;
  if (vm > btif)
    fail(ctx, "application does not fit reserved layout");
  if (btif - vm < vm_len)
    blog(ctx, "note: VM is 0x%x bytes, less than VM_LEN 0x%x\n", btif - vm, vm_len);

  struct blob image = {0};
  reserve(ctx, &image, image_end);
  image.n = image_end;
  memset(image.p, 0xff, image.n);
  flash_header(ctx, image.p, &c, keymac);
  uint32_t top_data = 32 + 4 * 32, uboot_off = top_data,
           sys_off = uboot_off + (uint32_t)uboot_n;
  struct jm_jieli_entry top[4] = {0};
  top[0] = (struct jm_jieli_entry){jm_crc16(uboot, uboot_n), uboot_off, (uint32_t)uboot_n,
                          (uint8_t)number(ctx, cfg(&c, "JLNEWFW", "UBOOT_FLAGS", "0")), 0, 0, {0}};
  set_name(top[0].name, "uboot.boot");
  top[1] = (struct jm_jieli_entry){
      jm_crc16(sys.p, sys.n), sys_off, (uint32_t)sys.n, 0x02, 0xff, 0, {0}};
  set_name(top[1].name, "isd_config.ini");
  top[2] = (struct jm_jieli_entry){o->have_mark ? o->mark : 0xffff,
                          appbase, 0xffffffff, 0x81, 0xff, 0, {0}};
  set_name(top[2].name, "app_dir_head");
  top[3] = (struct jm_jieli_entry){0xffff, keymac, 0x1000, 0x12, 0x01, 0xffff, {0}};
  set_name(top[3].name, "key_mac");
  for (unsigned i = 0; i < 4; i++)
    jm_jieli_encode_entry(image.p + 32 + i * 32, &top[i]);
  memcpy(image.p + uboot_off, uboot, uboot_n);
  memcpy(image.p + sys_off, sys.p, sys.n);
  struct jm_jieli_entry outer = {0};
  outer.offset = entry;
  outer.size = outer_size;
  outer.flags = 0x83;
  outer.reserved = 0xff;
  outer.index = o->nr ? 0 : 1; /* 1: the last area */
  set_name(outer.name, "app_area_head");
  cursor = appoff;
  for (size_t i = 0; i < o->nf; i++) {
    struct jm_jieli_entry e = {jm_crc16(files[i].data, files[i].size),
                      cursor, (uint32_t)files[i].size, 0x82, 0xff, 0, {0}};
    set_name(e.name, files[i].name);
    jm_jieli_encode_entry(image.p + appbase + 32 + (uint32_t)i * 32, &e);
    memcpy(image.p + appbase + cursor, files[i].data, files[i].size);
    cursor += (uint32_t)files[i].size;
  }
  struct jm_jieli_entry r[3] = {0};
  r[0] = (struct jm_jieli_entry){0xffff, vm, btif - vm, 0x12, 0x81, 0, {0}};
  set_name(r[0].name, "VM");
  wr32(r[0].name + 12, vm_len);
  r[1] = (struct jm_jieli_entry){0xffff, o->at, code_end, 0x92, 0x82, 0, {0}};
  set_name(r[1].name, "PRCT");
  memcpy(r[1].name + 12, "CODE", 4);
  r[2] = (struct jm_jieli_entry){0xffff, btif, btif_len, 0x92, 0x81, 1, {0}};
  set_name(r[2].name, "BTIF");
  memcpy(r[2].name + 12, "AUTO", 4);
  for (unsigned i = 0; i < (nareas ? nareas : 3); i++)
    jm_jieli_encode_entry(image.p + appbase + 32 + (uint32_t)(o->nf + i) * 32, nareas ? &areas[i] : &r[i]);
  outer.data_crc = jm_crc16(image.p + appbase + 32, outer_size - 32);
  jm_jieli_encode_entry(image.p + appbase, &outer);
  uint32_t pos = appbase + outer_size;
  for (size_t i = o->nf; i < nall; i++) {
    struct jm_jieli_entry e = {jm_crc16(files[i].data, files[i].size), 32,
                      32 + (uint32_t)files[i].size, area_flags[i], 0xff,
                      (uint16_t)(i + 1 == nall), {0}};
    set_name(e.name, files[i].name);
    jm_jieli_encode_entry(image.p + pos, &e);
    memcpy(image.p + pos + 32, files[i].data, files[i].size);
    pos += 32 + (uint32_t)files[i].size;
  }
  if (main_bin) {
    uint8_t *h = image.p + appbase + o->main_at;
    wr32(h, MAIN_MAGIC);
    wr32(h + 4, (uint32_t)main_n);
    wr32(h + 8, jm_crc16(main_bin, main_n));
    wr32(h + 12, (entry & ~0xfffffu) + o->main_at + MAIN_HEAD_SIZE);
    wr32(h + 16, MAIN_HEAD_VERSION);
    wr32(h + 20, o->main_at);
    memset(h + 24, 0, 16);
    strncpy((char *)h + 24, cfg(&c, "EXTRA_CFG_PARAM", "PID", ""), 16);
    wr32(h + 40, jm_crc16(h + 4, 36));
    memcpy(h + MAIN_HEAD_SIZE, main_bin, main_n);
    bfree(ctx, main_bin);
    ctx->update_size=MAIN_HEAD_SIZE+main_n;
    ctx->update=bmalloc(ctx,ctx->update_size);
    memcpy(ctx->update,h,ctx->update_size);
  }
  scramble_bankcb(ctx, image.p + uboot_off, uboot_n);
  jm_jieli_sfc(image.p, appbase, area_n, appbase, o->key);
  for (unsigned i = 0; i < 4; i++)
    jm_jieli_enc(image.p + 32 + i * 32, 32, 0xffff);

  /* On flash: the image at `at`, then the copies. */
  uint32_t end = o->at + image_end;
  for (size_t i = 0; i < o->nc; i++) {
    if ((uint64_t)o->copies[i].from + o->copies[i].len > image_end)
      fail(ctx, "a copy reaches past the image");
    uint64_t copy_end=(uint64_t)o->copies[i].to+o->copies[i].len;
    if (copy_end>total) fail(ctx, "copy exceeds flash capacity");
    if (copy_end>end) end=(uint32_t)copy_end;
  }
  if (end > total)
    fail(ctx, "the image and copies do not fit the flash");
  struct blob flash = {0};
  reserve(ctx, &flash, end);
  flash.n = end;
  memset(flash.p, 0xff, end);
  memcpy(flash.p + o->at, image.p, image_end);
  for (size_t i = 0; i < o->nc; i++)
    memcpy(flash.p + o->copies[i].to, image.p + o->copies[i].from,
           o->copies[i].len);
  blog(ctx, "packed: %u bytes, image at 0x%x (app @0x%x, end 0x%x, code limit "
         "0x%x), VM 0x%x-0x%x, flash 0x%x, chip key 0x%04x\n",
         end, o->at, appbase, image_end, code_end, vm, btif, total, o->key);
  if (key_out)
    *key_out = o->key;
  bfree(ctx, image.p);
  return flash;
}
static void load_config(struct build_context *ctx,struct config *c,const char *name) {
  size_t size;uint8_t *input=read_file(ctx,name,&size);char section[80]="";
  if(size>128*1024) fail(ctx,"INI exceeds 128 KiB");
  if(memchr(input,0,size)) fail(ctx,"NUL in INI");
  size_t at=0;
  while(at<size) {
    size_t end=at;while(end<size && input[end]!='\n') ++end;
    if(end-at>=1024) fail(ctx,"INI line exceeds 1023 bytes");
    char line[1024];memcpy(line,input+at,end-at);line[end-at]=0;at=end+(end<size);
    char *comment=strstr(line,"//");if(comment) *comment=0;
    char *s=trim(line);if(!*s || *s=='#') continue;
    if(*s=='[') {
      char *close=strchr(s,']');if(!close) fail(ctx,"bad INI section");*close=0;
      if(strlen(s+1)>=sizeof section) fail(ctx,"INI section too long");
      strcpy(section,s+1);upper(section);continue;
    }
    char *eq=strchr(s,'=');if(!eq) continue;*eq=0;
    char *key=trim(s),*val=trim(eq+1),*semi=strrchr(val,';');if(semi) *semi=0;
    val=trim(val);upper(key);
    if(c->n==MAX_CFG) fail(ctx,"too many INI values");
    c->v[c->n++]=(struct kv){bstrdup(ctx,section),bstrdup(ctx,key),bstrdup(ctx,val)};
  }
}
static void detach(struct build_context *ctx,void *p) {
  for(size_t i=0;i<ctx->allocations_count;++i) if(ctx->allocations[i]==p) { ctx->allocations[i]=NULL;return; }
}
/* Build a complete new-layout Jieli image from SDK or unpack-generated INI.
 * Input names are virtual: read supplies borrowed bytes, copied before its next
 * invocation. Relative input names resolve beside options.ini. No filesystem
 * or device access occurs. Optional log receives informational text only.
 * On success result owns malloc-backed image/update buffers; release with jm_jieli_build_free.
 * On failure output is unchanged, error receives text, and temporary allocations
 * are released. Options are copied and never modified. Each call owns its error
 * context; no globals or process-exiting error handlers are used. */
int jm_jieli_build(const struct jm_jieli_build_options *options,jm_jieli_read_file read,
                     jm_jieli_build_log log,void *user,
                     struct jm_jieli_build_result *out,char error[256]) {
  if(!options || !read || !out || !options->ini || options->nf>MAX_FILES || options->nr>MAX_FILES || options->nc>16) {
    if(error) snprintf(error,256,"invalid builder arguments");
    return 0;
  }
  for(size_t i=0;i<options->nf;++i) if(!options->files[i]) {
    if(error) snprintf(error,256,"missing application filename");
    return 0;
  }
  for(size_t i=0;i<options->nr;++i) if(!options->res[i]) {
    if(error) snprintf(error,256,"missing resource filename");
    return 0;
  }
  struct build_context *ctx=calloc(1,sizeof *ctx);
  if(!ctx) { if(error) snprintf(error,256,"out of memory");return 0; }
  ctx->read=read;ctx->log=log;ctx->user=user;
  volatile int ok=0;
  if(!setjmp(ctx->error_jump)) {
    struct jm_jieli_build_options copy=*options;uint16_t key;
    struct blob image=build(ctx,&copy,&key);
    struct jm_jieli_build_result result={image.p,ctx->update,image.n,ctx->update_size,key};
    detach(ctx,image.p);detach(ctx,ctx->update);*out=result;ok=1;
  } else if(error) snprintf(error,256,"%s",ctx->message);
  for(size_t i=0;i<ctx->allocations_count;++i) free(ctx->allocations[i]);
  free(ctx);return ok;
}
/* Release successful builder output. Reset fields to support safe reuse. */
void jm_jieli_build_free(struct jm_jieli_build_result *result) {
  if(!result) return;
  free(result->image);free(result->update);memset(result,0,sizeof *result);
}

#ifdef UNITTEST
#include "image.h"
#include "config.h"
#include "../proto_jollymaam.h"
#include <assert.h>
struct fixture { const char *ini;uint8_t boot[20],app[32],main[64]; };
static int fixture_read(void *user,const char *name,const uint8_t **data,size_t *size) {
  struct fixture *f=user;
  if(!strcmp(name,"isd_config.ini")) { *data=(const uint8_t *)f->ini;*size=strlen(f->ini); }
  else if(!strcmp(name,"boot")) { *data=f->boot;*size=sizeof f->boot; }
  else if(!strcmp(name,"app.bin")) { *data=f->app;*size=sizeof f->app; }
  else if(!strcmp(name,"main.bin")) { *data=f->main;*size=sizeof f->main; }
  else return 0;
  return 1;
}
struct roundtrip { char *ini;size_t count;struct jm_jieli_file files[128]; };
static int collect(void *user,const struct jm_jieli_file *file) {
  struct roundtrip *r=user;assert(r->count<128);r->files[r->count++]=*file;return 0;
}
static int roundtrip_read(void *user,const char *name,const uint8_t **data,size_t *size) {
  struct roundtrip *r=user;
  if(!strcmp(name,"isd_config.ini")) { *data=(const uint8_t *)r->ini;*size=strlen(r->ini);return 1; }
  for(size_t i=0;i<r->count;++i) if(!strcmp(name,r->files[i].name)) {
    *data=r->files[i].data;*size=r->files[i].size;return 1;
  }
  return 0;
}
int main(void) {
  struct fixture f;memset(&f,0,sizeof f);
  f.ini="[EXTRA_CFG_PARAM]\nPID=TEST;\nENTRY=0x1e00120;\n";
  wr16(f.boot,1);wr16(f.boot+2,4);wr32(f.boot+8,16);wr16(f.boot+14,jm_crc16(f.boot,14));
  memcpy(f.boot+16,"boot",4);memset(f.app,0xa5,sizeof f.app);memset(f.main,0x5a,sizeof f.main);
  struct jm_jieli_build_options options={0};options.ini="isd_config.ini";options.uboot="boot";
  options.files[0]="app.bin";options.nf=1;options.key=0x1234;options.have_key=true;
  for(unsigned main=0;main<2;++main) {
    options.main_file=main?"main.bin":NULL;options.main_at=main?0x1000:0;
    struct jm_jieli_build_result result;char error[256];
    assert(jm_jieli_build(&options,fixture_read,NULL,&f,&result,error));
    uint8_t *scratch=malloc(result.image_size);assert(scratch);struct jm_jieli_image view;
    assert(jm_jieli_image_open(&view,result.image,result.image_size,scratch,result.image_size)==JM_JIELI_IMAGE_OK);
    assert(view.key==0x1234);
    if(main) assert(result.update_size==0x120+64 && jm_update_image_check(result.update,result.update_size,"TEST"));
    else assert(!result.update && !result.update_size);
    struct roundtrip r={0};size_t ini_size;
    assert(jm_jieli_image_config(&view,"fixture",&r.ini,&ini_size));
    assert(ini_size==strlen(r.ini));
    assert(jm_jieli_image_visit(&view,collect,&r)==JM_JIELI_IMAGE_OK);
    struct jm_jieli_build_options again={0};again.ini="isd_config.ini";
    struct jm_jieli_build_result rebuilt;
    assert(jm_jieli_build(&again,roundtrip_read,NULL,&r,&rebuilt,error));
    assert(rebuilt.image_size==result.image_size && !memcmp(rebuilt.image,result.image,result.image_size));
    assert(rebuilt.update_size==result.update_size);
    if(main) assert(!memcmp(rebuilt.update,result.update,result.update_size));
    free(r.ini);jm_jieli_build_free(&rebuilt);
    free(scratch);jm_jieli_build_free(&result);assert(!result.image);
  }
  const char *invalid[]={"[SYS_CFG_PARAM]\nEOFFSET=4294967295;\n", "[JLNEWFW]\nAREAS=,,,;\n", "[JLNEWFW]\nFLASH_SIZE=0xffffffff;\n", "[EXTRA_CFG_PARAM]\nNEW_FLASH_FS=NO;\n"};
  for(unsigned i=0;i<sizeof invalid/sizeof *invalid;++i) {
    f.ini=invalid[i];char error[256]={0};struct jm_jieli_build_result out={0};
    out.image=(uint8_t *)1;
    assert(!jm_jieli_build(&options,fixture_read,NULL,&f,&out,error));
    assert(out.image==(uint8_t *)1 && error[0]);
  }
  return 0;
}
#endif
