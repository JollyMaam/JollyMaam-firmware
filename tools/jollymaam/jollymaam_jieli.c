/*
 * jollymaam - inspect, unpack, and build JieLi "new firmware" flash images.
 *
 * The image commands delegate to libjollymaam. Device selection and legacy
 * Jieli command-line policy live in this application module. The pack command is
 * aimed at the BR23 SDK layout emitted by isd_download: uboot.boot, the
 * encoded SYS_CFG_PARAM stream, app files, and PRCT/VM/BTIF/key_mac reserved
 * regions.  See docs/jieli/re-application-image.md for the on-flash layout.
 */
#define _XOPEN_SOURCE 700
#include "jollymaam_jieli.h"
#include "../../src/lib/jieli_image/crypto.h"
#include "../../src/lib/jieli_image/entry.h"
#include "../../src/lib/jieli_image/key.h"
#include "../../src/lib/jieli_image/ufw.h"
#include "../../src/lib/jieli_image/bankcb.h"
#include "../../src/lib/proto_jieli.h"
#include "../../src/lib/jieli_image/builder.h"
#include "../../src/lib/jieli_image/config.h"
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
/* --main's header (src/updater/updater.c): "MAIN", the app's size, CRC16
 * and entry, the format version, the offset it is linked for, the image's
 * PID, and a CRC16 of the fields from size to PID. */
#define MAIN_MAGIC 0x4e49414du
#define MAIN_HEAD_SIZE 0x120u
#define MAIN_HEAD_VERSION 1u

struct blob {
  uint8_t *p;
  size_t n, cap;
};


static void die(const char *s) {
  fprintf(stderr, "jollymaam: %s\n", s);
  exit(1);
}
static void die_errno(const char *s) {
  fprintf(stderr, "jollymaam: %s: %s\n", s, strerror(errno));
  exit(1);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint32_t rd32be(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr16(uint8_t *p, uint16_t x) {
  p[0] = x;
  p[1] = x >> 8;
}
static uint32_t align_up(uint32_t x, uint32_t a) { return (x + a - 1) / a * a; }


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
    fprintf(stderr, "jollymaam: not a number: %s\n", s);
    exit(1);
  }
  if (v > UINT32_MAX)
    die("number too large");
  return (uint32_t)v;
}


/* A chip-key record made from nothing (when the INI carries none). */


static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/'), *back = strrchr(p, '\\');
  if(back && (!s || back>s)) s=back;
  return s ? s + 1 : p;
}


/* EOFFSET from the isd_config.ini of the image at `o`, in 4 KiB units: where
 * the boot code takes the image from.  0 when there is none. */

/* The first valid header, or, when its EOFFSET points at another valid one,
 * that: an upgraded dongle keeps stale copies at 0 and 0x1000 and boots the
 * image at 0x10000 (EOFFSET 16). */


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
  const uint8_t *flash;
  enum jm_jieli_ufw_result result = jm_jieli_ufw_open(file, file_size, &flash, size);
  switch (result) {
  case JM_JIELI_UFW_RAW:
    return file;
  case JM_JIELI_UFW_OK: {
    uint8_t *image = malloc(*size ? *size : 1);
    if (!image)
      die("out of memory");
    memcpy(image, flash, *size);
    free(file);
    printf("UFW wrapper: extracted flash.bin (%u bytes)\n", (unsigned)*size);
    return image;
  }
  case JM_JIELI_UFW_BAD_LIST: die("invalid UFW entry list"); break;
  case JM_JIELI_UFW_TRUNCATED: die("UFW flash.bin is truncated"); break;
  case JM_JIELI_UFW_BAD_CRC: die("UFW flash.bin CRC mismatch"); break;
  case JM_JIELI_UFW_NO_FLASH: die("UFW has no flash.bin entry"); break;
  }
  return NULL;
}

/* ---- the chip-key record --------------------------------------------------
 * The first 34 bytes of isd_config.ini: 32 bytes and their CRC.  Bit j of the
 * key is set when byte 16+j XOR byte 15-j is below a threshold taken from
 * the sum of the first 16 bytes. */
static uint16_t record_key(const uint8_t *d) {
  uint16_t key;
  if (!jm_jieli_record_key(d, &key))
    die("chip-key record CRC mismatch");
  return key;
}

struct extract_context { const char *directory; };
static int extract_file(void *user,const struct jm_jieli_file *file) {
  if(!strcmp(file->name,"isd_config.ini")) return 0;
  if(!*file->name || !strcmp(file->name,".") || !strcmp(file->name,"..") ||
      strpbrk(file->name,"/\\:")) return 1;
  struct extract_context *context=user;char path[1024];
  path_join(path,sizeof path,context->directory,file->name);write_file(path,file->data,file->size);return 0;
}
static void inspect(const char *path,const char *outdir,bool extract) {
  size_t size;uint8_t *raw=load_image(path,&size),*workspace=malloc(size?size:1);struct jm_jieli_image image;
  if(!workspace) die("out of memory");
  int error=jm_jieli_image_open(&image,raw,size,workspace,size);
  if(error) { fprintf(stderr,"jollymaam: invalid image (%d)\n",error);exit(1); }
  uint8_t fh[32];jm_jieli_header_at(raw,size,image.base,fh);
  char vid[5]={0},pid[17];memcpy(vid,raw+image.base+4,4);jm_jieli_name_string(pid,raw+image.base+16);
  printf("base=0x%x burner=%u vid=%s flash_size=0x%x fs=%u align=%u special=0x%02x pid=%s eoffset=%u\n",
         image.base,rd16(fh+2),vid,rd32(fh+8),fh[12],fh[13],fh[15],pid,jm_jieli_eoffset(raw,size,image.base));
  for(size_t i=0;i<image.top_count;++i) {
    struct jm_jieli_entry e;jm_jieli_decode_entry(workspace+image.base+32+i*32,&e);char name[17];jm_jieli_name_string(name,e.name);
    printf("top %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n",name,e.offset,e.size,e.flags,e.reserved,e.index);
  }
  uint8_t *plain=workspace+image.appbase;struct jm_jieli_entry outer;jm_jieli_decode_entry(plain,&outer);
  char name[17];jm_jieli_name_string(name,outer.name);
  printf("app  %-16s entry=0x%08x size=0x%x key=0x%04x\n",name,outer.offset,outer.size,image.key);
  for(uint32_t off=32;off+32<=outer.size;off+=32) {
    struct jm_jieli_entry e;jm_jieli_decode_entry(plain+off,&e);jm_jieli_name_string(name,e.name);
    printf("  %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n",name,e.offset,e.size,e.flags,e.reserved,e.index);
    if(e.index) break;
  }
  if(!outer.index) for(uint32_t off=outer.size;;) {
    struct jm_jieli_entry e;jm_jieli_decode_entry(plain+off,&e);jm_jieli_name_string(name,e.name);
    printf("res  %-16s off=0x%08x size=0x%08x flags=%02x/%02x index=%u\n",name,off,e.size,e.flags,e.reserved,e.index);
    if(e.index) break;
    off+=e.size;
  }
  if(extract) {
    ensure_dir(outdir);struct extract_context context={outdir};
    if(jm_jieli_image_visit(&image,extract_file,&context)!=JM_JIELI_IMAGE_OK) die("cannot extract image files safely");
    char *config;size_t config_size;
    if(!jm_jieli_image_config(&image,base_name(path),&config,&config_size)) die("cannot reconstruct INI");
    char output[1024];path_join(output,sizeof output,outdir,"isd_config.ini");write_file(output,config,config_size);free(config);
  }
  free(workspace);free(raw);
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
  const char *main_file; /* --main: a second app above the first */
  const char *main_out;  /* --main-out: it with its header, in the clear */
  uint32_t main_at;
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
  else if (!strcmp(a, "--main")) {
    char *colon = strchr(argv[++*i], ':');
    if (!colon)
      die("--main is OFFSET:FILE");
    *colon = 0;
    o->main_at = number(argv[*i]);
    o->main_file = colon + 1;
  } else if (!strcmp(a, "--main-out"))
    o->main_out = argv[++*i];
  else
    return false;
  return true;
}

struct build_reader { uint8_t *current; };
static int build_read(void *user,const char *name,const uint8_t **data,size_t *size) {
  struct build_reader *reader=user;free(reader->current);reader->current=read_file(name,size);
  *data=reader->current;return 1;
}
static void build_log(void *user,const char *message) { (void)user;fputs(message,stdout); }
static struct blob build_with_log(struct pack_opts *o,uint16_t *key_out,jm_jieli_build_log log) {
  struct jm_jieli_build_options options={0};
  options.ini=o->ini;options.uboot=o->uboot;options.nf=o->nf;options.nr=o->nr;
  memcpy(options.files,o->files,sizeof options.files);memcpy(options.res,o->res,sizeof options.res);
  options.have_key=o->have_key;options.have_at=o->have_at;options.have_mark=o->have_mark;
  options.key=o->key;options.mark=o->mark;options.at=o->at;
  options.flash_size=o->flash_size;options.code_limit=o->code_limit;options.nc=o->nc;
  for(size_t i=0;i<o->nc;++i) options.copies[i]=(struct jm_jieli_copy){o->copies[i].from,o->copies[i].to,o->copies[i].len};
  options.main_file=o->main_file;options.main_at=o->main_at;
  struct build_reader reader={0};struct jm_jieli_build_result result;char error[256];
  int ok=jm_jieli_build(&options,build_read,log,&reader,&result,error);free(reader.current);
  if(!ok) die(error);
  if(key_out) *key_out=result.key;
  if(o->main_out && result.update) write_file(o->main_out,result.update,result.update_size);
  free(result.update);return (struct blob){result.image,result.image_size,result.image_size};
}

static struct blob build(struct pack_opts *o,uint16_t *key_out) {
  return build_with_log(o,key_out,build_log);
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

#ifdef JOLLYMAAM_PACK_ONLY
/* Offline build tool: the same options and image builder as jieli pack. */
int main(int argc, char **argv) {
  if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--help"))) {
    puts("Usage: jollymaam-jlpack -c CONFIG [-u UBOOT] -o OUTPUT [OPTIONS] [APP ...]\n"
         "Uses the options of jollymaam jieli pack.");
    return 0;
  }
  cmd_pack(argc - 1, argv + 1);
  return 0;
}
#else

/* ---- flash --------------------------------------------------------------------
 * Through the chip's ROM loader (UBOOT1.00, a USB mass-storage device): a
 * loader blob goes into RAM and runs, then takes erase/write/read commands.
 * Commands are 16-byte vendor CDBs: a big-endian 16-bit command, arguments,
 * 0xff padding; those without data answer with 16 bytes starting with the
 * command (protocol as worked out by kagaimiq's jl-uboot-tool).  The loader
 * blob is the SDK's (cpu/br23/tools/br23loader.bin; --loader or
 * JOLLYMAAM_LOADER); jl-uboot-tool's older, smaller one works too.  Linux SCSI
 * generic or Windows SCSI pass-through. */

#if defined(__linux__) || defined(_WIN32)
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Where device nodes are: $DEV_ROOT when set (every node in one directory,
 * as a container exports them), else /dev. */
static const char *dev_root(void) {
  const char *r = getenv("DEV_ROOT");
  return r && *r ? r : "/dev";
}

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
  DIR *dir = opendir(dev_root());
  struct dirent *e;
  while (dir && (e = readdir(dir)) && n < 256)
    if (!strncmp(e->d_name, "sg", 2))
      snprintf(paths[n++], 300, "%s/%s", dev_root(), e->d_name);
  if (dir) closedir(dir);
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
 * jl-uboot-tool data; docs/gamesir-nova-2-lite/). */
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
  if ((getenv("JOLLYMAAM_DEBUG") || getenv("JLNEWFW_DEBUG"))) {
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
static void jl_cdb(uint8_t cdb[16], uint16_t cmd, const uint8_t *args, size_t n) {
  if (!jm_jieli_cdb(cdb, cmd, args, n)) die("invalid Jieli command arguments");
}
/* A command without data; `resp` gets the 14 bytes after the echo. */
static bool jl_exec(struct dev *d, uint16_t cmd, const uint8_t *args, size_t n,
                    uint8_t resp[14]) {
  uint8_t cdb[16], r[16];
  jl_cdb(cdb, cmd, args, n);
  if (!sg(d->fd, cdb, 16, SG_DXFER_FROM_DEV, r, 16, 20000) ||
      !jm_jieli_exec_reply(r, sizeof r, cmd, NULL))
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
  wr16(a + 7, jm_crc16(data, len));
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


/* A running JieLi SDK firmware that shows itself as a disk (vendor: the
 * chip name) reboots into its ROM loader on any of the loader's commands, as
 * the SDK's mass-storage code does (and ours, on its HID tool device).  Sent to every such device;
 * which one is written is then decided by PID.  Repeat while waiting so
 * firmware connected after the command starts is also restarted; --device
 * limits restart requests to that path. */
/* IDENTIFY byte 6 distinguishes our pad and dongle builds. Keep firmware for
 * the other board running; stock firmware is selected by PID in the loader. */
/* ---- what is connected -------------------------------------------------------
 * INQUIRY identifies Jieli ROM loaders and SDK firmware; product
 * "UBOOT1.00" its ROM loader.  Other Jieli SDK firmware's PID is known only
 * once its loader reads the flash.  Our firmware is a HID tool device and
 * answers IDENTIFY ("C2PD", the image's PID at 40).  A stock pad that shows
 * itself only as HID is known by the table below, in each of its USB modes;
 * in some of them it takes a command into its loader. */

enum disk_class { DISK_OTHER, DISK_LOADER, DISK_JIELI };

static enum disk_class classify_disk(device_fd fd, char pid[17], int *chip) {
  uint8_t cdb[6] = {0x12, 0, 0, 0, 40, 0}, q[40] = {0};
  pid[0] = 0;
  *chip = -1;
  if (!sg(fd, cdb, 6, SG_DXFER_FROM_DEV, q, sizeof q, 2000))
    return DISK_OTHER;
  *chip = chip_of(q + 8);
  if (*chip >= 0 && !memcmp(q + 16, "UBOOT", 5))
    return DISK_LOADER;
  return *chip >= 0 ? DISK_JIELI : DISK_OTHER;
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
  {"GS_C2_ADC_DEVICE", "Cyclone 2", "Xbox 360 (green)", 0x3537, 0x1053, 0x0352,
   NULL, NULL, 0xfff0, 64, {0x0f, 0x17, 0x55, 0x88, 0x00}, NULL},
  {"GS_C2_ADC_DEVICE", "Cyclone 2", "Switch (red)", 0x057e, 0x2009, 0x0352,
   NULL, "Gamepad", 0, 0, {0}, NULL},
  {"GS_C2_ADC_DEVICE", "Cyclone 2", "DS4 (blue)", 0x054c, 0x09cc, 0x0352,
   "Guangzhou Chicken Run Network Technology Co., Ltd.", NULL, 0, 0, {0}, NULL},
  /* docs/gamesir-nova-2-lite/ */
  {"AC695X_TRANS", "Nova 2 Lite", "Xbox 360 (green)", 0x3537, 0x100f, 0x0100,
   NULL, NULL, 0xff7a, 65, {0x00, 0xf2, 0x00, 0x04, 0xf6}, NULL},
  {"AC695X_TRANS", "Nova 2 Lite", "Switch (red)", 0x057e, 0x2009, 0x0200,
   "Zikway", "Pro Controller", 0, 0, {0}, NULL},
  {"AC695X_TRANS", "Nova 2 Lite", "DS4 (blue)", 0x054c, 0x09cc, 0x0100,
   "Sony Computer Entertainment", "Wireless Controller", 0, 0, {0}, NULL},
  {"AC632N_TRANS", "Nova 2 Lite Dongle", "", 0x3537, 0x2106, 0x0100, "Zikway", "HID zk", 0,
   0, {0}, "hold its pairing button while connecting USB"},
};
#define STOCK_HID_COUNT (sizeof stock_hid / sizeof stock_hid[0])

/* A HID interface (Linux) or top-level collection (Windows) as the table
 * needs it. */
struct hid_dev {
  char path[300];
  unsigned vid, product, bcd;
  char manufacturer[64], product_name[64];
  unsigned feature_len;         /* its feature report, without the ID byte */
#ifdef _WIN32
  unsigned usage_page;          /* the collection's */
  unsigned out_len;             /* its output report, with the ID byte */
#else
  uint8_t desc[4096];           /* the report descriptor */
  size_t desc_n;
#endif
};

#ifndef _WIN32
/* A report descriptor's feature reports, in bytes: report size times count
 * for each feature item (short items). */
static unsigned feature_bytes(const uint8_t *d, size_t n) {
  unsigned size = 0, count = 0, bits = 0;
  for (size_t i = 0; i < n;) {
    size_t k = (size_t[]){0, 1, 2, 4}[d[i] & 3];
    unsigned v = 0;
    for (size_t j = 0; j < k && i + 1 + j < n; j++)
      v |= (unsigned)d[i + 1 + j] << 8 * j;
    switch (d[i] & 0xfc) {
    case 0x74: size = v; break;
    case 0x94: count = v; break;
    case 0xb0: bits += size * count; break;
    }
    i += 1 + k;
  }
  return bits / 8;
}

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

#endif

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
      d->feature_len = caps.FeatureReportByteLength ? caps.FeatureReportByteLength - 1u : 0;
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
  DIR *dir = opendir(dev_root());
  struct dirent *e;
  while (dir && (e = readdir(dir)) && n < max) {
    char sys[512], line[128], usb[400];
    unsigned bus;
    if (strncmp(e->d_name, "hidraw", 6))
      continue;
    struct hid_dev *d = &out[n];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s/%s", dev_root(), e->d_name);
    if (access(d->path, R_OK | W_OK)) /* the node we may use */
      continue;
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
      d->feature_len = feature_bytes(d->desc, d->desc_n);
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

/* Our firmware's tool device: a HID interface, usage page 0xffc2 (usb.c and
 * the platform's tool device).  A command is a feature report (the CDB, u16
 * length, flags, tag, u32 client), up to 512 bytes; its answer comes on
 * 64-byte input reports: the client, the tag, a state (2 a piece and more,
 * 3 the last, 4 refused: another program's command runs), u16 offset and
 * 56 bytes of answer.  Every program with the device open gets them all. */
#define TOOL_PAGE   0xffc2
#define TOOL_REPORT 512
#define TOOL_INPUT  2

static bool is_tool(const struct hid_dev *h) {
#ifdef _WIN32
  return h->usage_page == TOOL_PAGE;
#else
  return h->desc_n >= 3 && h->desc[0] == 0x06 && rd16(h->desc + 1) == TOOL_PAGE;
#endif
}

/* This process's client in the answers. */
static uint32_t tool_client(void) {
  static uint32_t c;
  if (!c) {
#ifdef _WIN32
    c = (uint32_t)GetCurrentProcessId() * 0x9e3779b9u ^ (uint32_t)GetTickCount();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    c = (uint32_t)getpid() * 0x9e3779b9u ^ (uint32_t)t.tv_nsec;
#endif
    if (!c)
      c = 1;
  }
  return c;
}

/* One command; with ans, up to n bytes of its answer.  Without, nothing is
 * read back (the loader opcodes: the device restarts). */
static bool tool_cmd(const struct hid_dev *h, const uint8_t cdb[16], uint8_t *ans, size_t n) {
  static uint8_t tag;
  uint8_t buf[TOOL_REPORT + 1], in[65];
  size_t size = h->feature_len >= 64 && h->feature_len < TOOL_REPORT ? h->feature_len + 1 : sizeof buf;
  uint32_t client = tool_client();
  bool ok = false;
#ifdef _WIN32
  HANDLE fh = CreateFileA(h->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if (fh == INVALID_HANDLE_VALUE)
    return false;
  HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
  int fd = open(h->path, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    return false;
#endif
  for (unsigned tries = 0; tries < 1000; tries++) {
    memset(buf, 0, sizeof buf);
    memcpy(buf + 1, cdb, 16);
    wr16(buf + 17, (uint16_t)n);
    buf[19] = TOOL_INPUT;
    buf[20] = ++tag;
    for (unsigned i = 0; i < 4; i++)
      buf[21 + i] = (uint8_t)(client >> 8 * i);
#ifdef _WIN32
    if (!HidD_SetFeature(fh, buf, (ULONG)size))
      break;
#else
    while (read(fd, in, sizeof in) > 0) /* earlier answers: dropped */
      ;
    if (ioctl(fd, HIDIOCSFEATURE(size), buf) < 0)
      break;
#endif
    if (!ans) {
      ok = true;
      break;
    }
    size_t got = 0;
    bool refused = false, failed = false;
    for (unsigned idle = 0; idle < 30 && !refused && !failed && !ok;) { /* 3 s without a piece */
      int k = 0;
#ifdef _WIN32
      OVERLAPPED ov = {0};
      DWORD done = 0;
      ov.hEvent = ev;
      ResetEvent(ev);
      if (!ReadFile(fh, in, sizeof in, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) {
        failed = true;
        break;
      }
      if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) {
        CancelIo(fh);
        GetOverlappedResult(fh, &ov, &done, TRUE);
        idle++;
        continue;
      }
      if (!GetOverlappedResult(fh, &ov, &done, TRUE)) {
        failed = true;
        break;
      }
      k = (int)done;
      if (k > 0) { /* Windows: the report ID byte (0) first */
        memmove(in, in + 1, (size_t)k - 1);
        k--;
      }
#else
      struct pollfd p = {fd, POLLIN, 0};
      if (poll(&p, 1, 100) <= 0) {
        idle++;
        continue;
      }
      k = (int)read(fd, in, sizeof in);
      if (k < 0) {
        failed = errno != EAGAIN;
        continue;
      }
#endif
      if (k < 8 || (uint32_t)(in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24) != client ||
          in[4] != tag)
        continue;
      if (in[5] == 4) {
        refused = true;
        break;
      }
      if ((in[5] != 2 && in[5] != 3) || rd16(in + 6) != got) {
        failed = true;
        break;
      }
      size_t m = (size_t)k - 8;
      if (m > n - got)
        m = n - got;
      memcpy(ans + got, in + 8, m);
      got += m;
      if (in[5] == 3)
        ok = true;
    }
    if (!refused)
      break;
    dev_sleep(1 + (client >> (tries % 24) & 7));
  }
#ifdef _WIN32
  CloseHandle(ev);
  CloseHandle(fh);
#else
  close(fd);
#endif
  return ok;
}

/* Our firmware behind a tool device: its image's PID. */
static bool tool_pid(const struct hid_dev *h, char pid[17]) {
  uint8_t cdb[16] = {0xf5, 0x00}, ident[56] = {0};
  if (!tool_cmd(h, cdb, ident, sizeof ident) || memcmp(ident, "C2PD", 4))
    return false;
  memcpy(pid, ident + 40, 16);
  pid[16] = 0;
  return true;
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
/* The RAM loader for a chip: --loader (or JOLLYMAAM_LOADER) names the SDK
 * (its cpu/<chip>/tools/), a folder holding <chip>loader.bin, or one of the
 * SDK's loader files (the others are found beside it). */
static const char *loader_path(const char *loader, int chip) {
  static char p[1100], dir[1024];
  if (!loader)
    loader = getenv("JOLLYMAAM_LOADER");
  if (!loader) loader=getenv("JLNEWFW_LOADER"); /* old environment alias */
  if (!loader)
    die("give the SDK (or a folder of <chip>loader.bin) with --loader or JOLLYMAAM_LOADER");
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
  fprintf(stderr, "jollymaam: no %s for the %s under %s\n", chips[chip].blob, chips[chip].chip, loader);
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
  if ((getenv("JOLLYMAAM_DEBUG") || getenv("JLNEWFW_DEBUG"))) { /* what landed, as the ROM reads it back */
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
  jm_jieli_crc_cipher(k, 2, 0xffffffffu);
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
    if (jm_jieli_header_at(p, n, candidates[i], clear))
      o = candidates[i], found = true;
  if (!found)
    return false;
  uint32_t e = jm_jieli_eoffset(p, n, o) * 0x1000;
  if (e && e != o && jm_jieli_header_at(p, n, e, clear))
    o = e;
  memcpy(pid, p + o + 16, 16);
  pid[16] = 0;
  for (char *q = pid; *q; q++)
    if ((uint8_t)*q == 0xff)
      *q = 0;
  for (uint32_t off = o + 32; key && off + 32 <= n && off < o + 0x400; off += 32) {
    uint8_t h[32];
    struct jm_jieli_entry en;
    char name[17];
    memcpy(h, p + off, 32);
    jm_jieli_enc(h, 32, 0xffff);
    if (!jm_jieli_decode_entry(h, &en))
      break;
    jm_jieli_name_string(name, en.name);
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
    } else if (k == DISK_JIELI) {
      x->kind = CAND_JIELI;
    } else
      continue;
    strcpy(x->pid, pid);
    if (want && k != DISK_JIELI && strcmp(pid, want))
      continue;
    n++;
  }
  size_t nh = hid_devices(h, 64);
  for (size_t i = 0; i < nh && n < max; i++) {
    struct cand *x = &c[n];
    if (!is_tool(&h[i]) || restarting(h[i].path) || !tool_pid(&h[i], pid))
      continue;
    memset(x, 0, sizeof *x);
    x->kind = CAND_OURS, x->hid = &h[i];
    snprintf(x->path, sizeof x->path, "%.299s", h[i].path);
    snprintf(x->pid, sizeof x->pid, "%s", pid);
    if (!usb_port(x->path, x->port))
      x->port[0] = 0;
    if (!cand_chosen(x, o->device))
      continue;
    if (want && strcmp(pid, want)) {
      if (!told_before(x->path))
        printf("%s: our firmware for %s, left running\n", x->path, pid);
      continue;
    }
    n++;
  }
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
      fprintf(stderr, "jollymaam: more than one device %s%s:\n", want ? "for " : "",
              want ? want : "connected");
      for (size_t i = 0; i < n; i++)
        fprintf(stderr, "  %-24s %s%s%s  (%s)\n", c[i].port[0] ? c[i].port : c[i].path,
                cand_what(&c[i], buf), c[i].pid[0] ? ", " : "", c[i].pid, c[i].path);
      die("pick one with --device PORT (make: DEVICE=PORT); jollymaam jieli scan lists them");
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
        } else if (x->hid) {
          uint8_t cmd[16];
          memset(cmd, 0xff, 16);
          cmd[0] = 0xfc, cmd[1] = 0x0a; /* running firmware: any loader command */
          sent = tool_cmd(x->hid, cmd, NULL, 0);
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
      fprintf(stderr, "jollymaam: no ROM loader %s%s\n", want ? "holding " : "found",
              want ? want : "");
      die(n ? "the device did not come up in its ROM loader"
            : "no device for it found (jollymaam jieli scan lists what is connected)");
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
  static const char *const names[] = {"", "ROM loader", "Jieli firmware"};
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
  for (size_t i = 0; i < nh; i++) {
    if (!is_tool(&h[i]) || !tool_pid(&h[i], pid))
      continue;
    if (!usb_port(h[i].path, port))
      snprintf(port, sizeof port, "-");
    printf("%-24s our firmware, %s  (%s)\n", port, pid, h[i].path);
  }
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

/* A step's progress, in place on a terminal: "writing  42%". */
static void progress(const char *what, uint32_t done, uint32_t total) {
  static int last = -1;
#ifdef _WIN32
  if (!_isatty(_fileno(stderr)))
#else
  if (!isatty(STDERR_FILENO))
#endif
    return;
  int pct = total ? (int)((uint64_t)done * 100 / total) : 100;
  if (pct == last && done < total)
    return;
  last = done < total ? pct : -1;
  fprintf(stderr, "\r%-9s %3d%%%s", what, pct, done < total ? "" : "\n");
}

static void erase(struct dev *d, uint32_t from, uint32_t to) {
  uint8_t a[4];
  for (uint32_t at = from; at < to;) {
    bool block = !(at & 0xffff) && to - at >= 0x10000;
    progress("erasing", at - from, to - from);
    be32(a, at);
    if (!jl_exec(d, block ? 0xfb00 : 0xfb01, a, 4, NULL))
      die("erase failed");
    at += block ? 0x10000 : 0x1000;
  }
  progress("erasing", to - from, to - from);
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
    progress("writing", at, (uint32_t)end);
    if (!blank && !jl_write(d, 0xfb04, at, w + at, m))
      die("write failed");
    at += m;
  }
  progress("writing", (uint32_t)end, (uint32_t)end);
  uint8_t *back = malloc(end);
  if (!back)
    die("out of memory");
  for (uint32_t at = 0; at < end; at += 0x4000) {
    uint32_t m = (uint32_t)(end - at < 0x4000 ? end - at : 0x4000);
    progress("verifying", at, (uint32_t)end);
    if (!flash_read(d, at, back + at, m))
      die("readback failed: the device is left in its loader");
  }
  progress("verifying", (uint32_t)end, (uint32_t)end);
  if (memcmp(back, w, end))
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
    die("usage: jollymaam jieli backup --pid PID [FLASH OPTIONS] OUTPUT");
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
  printf("backup: %s (%u bytes); jollymaam jieli run or a USB reconnect leaves the loader\n", output, d.flash_size);
  free(all);
  dev_close(d.fd);
}

/* Running firmware for PID restarts into its ROM loader, which stays until
 * USB is reconnected. */
static void cmd_loader(int argc, char **argv) {
  struct flash_opts o = {.wait = 30};
  for (int i = 0; i < argc; i++)
    if (!flash_arg(&o, argc, argv, &i))
      die("usage: jollymaam jieli loader --pid PID [--device PATH] [--wait SECONDS] [--loader BLOB]");
  if (!o.pid)
    die("usage: jollymaam jieli loader --pid PID [--device PATH] [--wait SECONDS] [--loader BLOB]");
  o.enter_loader = true;
  struct dev d = {0};
  select_dev(&d, &o, o.pid);
  printf("%s: in its ROM loader, holding %s; jollymaam jieli run or a USB reconnect leaves it\n", d.path, o.pid);
  dev_close(d.fd);
}

/* A device in its ROM loader starts the firmware in its flash, as after a
 * flash, without a reconnect. */
static void cmd_run(int argc, char **argv) {
  struct flash_opts o = {0};
  for (int i = 0; i < argc; i++)
    if (!flash_arg(&o, argc, argv, &i))
      die("usage: jollymaam jieli run [--pid PID] [--device PATH] [--loader BLOB]");
  struct dev d = {0};
  select_dev(&d, &o, o.pid ? o.pid : "");
  uint8_t a[4];
  be32(a, 1);
  jl_exec(&d, 0xfc0c, a, 4, NULL); /* run: no answer expected */
  printf("%s: running its firmware\n", d.path);
  dev_close(d.fd);
}

static void cmd_flash(int argc, char **argv) {
  struct flash_opts o = {0};
  const char *image = NULL;
  for (int i = 0; i < argc; i++) {
    if (flash_arg(&o, argc, argv, &i))
      continue;
    if (argv[i][0] == '-' || image)
      die("usage: jollymaam jieli flash [options] IMAGE");
    image = argv[i];
  }
  if (!image)
    die("usage: jollymaam jieli flash [options] IMAGE");
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
  struct pack_opts preflight=p;preflight.out=NULL;preflight.main_out=NULL;
  struct blob checked=build_with_log(&preflight,NULL,NULL);uint8_t header[32];uint32_t base;
  if(!jm_jieli_find_base(checked.p,checked.n,header,&base)) die("built image has no flash header");
  char want[17];jm_jieli_name_string(want,checked.p+base+16);free(checked.p);
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
static void cmd_run(int argc, char **argv) { cmd_flash(argc, argv); }
static void cmd_scan(void) { cmd_flash(0, NULL); }
#endif

static void usage(void) {
  puts(
      "usage: jollymaam jieli <subcommand>\n"
      "  jollymaam jieli rekey INPUT OUTPUT KEY\n"
      "  jollymaam jieli install --yes DEVICE LOADER IMAGE\n"
      "  jollymaam jieli check-stock PRODUCT IMAGE\n"
      "  jollymaam jieli restore --yes DEVICE BR23|BD19 PRODUCT LOADER IMAGE NEW_BACKUP\n"
      "  jollymaam jieli list IMAGE\n"
      "  jollymaam jieli verify IMAGE\n"
      "  jollymaam jieli unpack IMAGE DIRECTORY\n"
      "  jollymaam jieli pack -c isd_config.ini [-u uboot.boot] -o OUT [app.bin "
      "[cfg_tool.bin ...]]\n"
      "               [-r RESOURCE]... [-k KEY] [--flash-size SIZE] "
      "[--code-limit ADDR]\n"
      "               [--at ADDR] [--mark VALUE] [--copy FROM:TO:LEN]...\n"
      "               [--main OFFSET:FILE [--main-out FILE]]\n"
      "  jollymaam jieli backup --pid PID [FLASH OPTIONS] OUTPUT\n"
      "  jollymaam jieli loader --pid PID [--device PATH] [--wait SECONDS] [--loader BLOB]\n"
      "  jollymaam jieli run [--pid PID] [--device PATH] [--loader BLOB]\n"
      "  jollymaam jieli scan\n"
      "  jollymaam jieli flash [FLASH OPTIONS] IMAGE\n"
      "  jollymaam jieli flashpack [PACK OPTIONS] [FLASH OPTIONS] [app.bin ...]\n"
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
      "  --main    a main app OFFSET above the app area's start, behind a\n"
      "            header; the app area's app.bin is then its updater.\n"
      "            --main-out writes that header and app in the clear, for\n"
      "            the updater (padtool update)\n"
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

/* Command ownership is explicit: the main application retains its cross-chip
 * scan/update commands while this module provides Jieli image/device workflows. */
int jollymaam_jieli_command(const char *command) {
  static const char *const commands[]={"pack","list","verify","unpack","backup","loader","run","flash","flashpack","scan","help","--help"};
  if(!command) return 0;
  for(size_t i=0;i<sizeof commands/sizeof *commands;++i) if(!strcmp(command,commands[i])) return 1;
  return 0;
}
/* Execute a selected Jieli CLI command. Buffers and device handles belong to
 * this process. Fatal CLI errors exit; the underlying library returns errors. */
int jollymaam_jieli_app(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0); /* progress before any error */
  if(argc==2 && (!strcmp(argv[1],"help") || !strcmp(argv[1],"--help"))) { usage();return 0; }
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
  if (!strcmp(argv[1], "run")) {
    cmd_run(argc - 2, argv + 2);
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

#endif /* JOLLYMAAM_PACK_ONLY */
