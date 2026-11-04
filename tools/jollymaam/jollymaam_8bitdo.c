/* jollymaam 8bitdo: the 8BitDo pads' images and their boot loader (USB
 * 2dc8:3208, HID).
 *
 * Images (offline, also in jollymaam-pack):
 *   pack  a Telink app from objcopy as the boot loader starts it (stamp,
 *         size, CRC: jm_8bitdo_telink_pack)
 *   join  the updater's packed image and the app's as one image for the
 *         loader (firmware.img): the updater, 0xff to --at, then update.bin
 *         (the MAIN header and the app, jm_update_image_make)
 * Devices (Linux, Windows):
 *   flash   into the boot loader, from our firmware or stock firmware when
 *           the pad runs one; the Telink pad (U2C_WIRELESS) takes
 *           firmware.img sector by sector, the Yichip pad (U2C_BLUETOOTH) a
 *           raw app as one encrypted stream (libjollymaam's upgrade_8bitdo)
 *   loader  into the boot loader only, and wait there
 *   start   out of the boot loader: it starts the app
 *
 * A device is found by its USB port, as the Jieli commands do (--device
 * takes a port or a node): the boot loader by the model it answers, our
 * firmware by its tool device's PID, stock firmware by the model its vendor
 * interface (output report 0x81) answers.  More than one is refused. */
#define _GNU_SOURCE
#include "jollymaam_8bitdo.h"
#include "../../src/lib/proto_8bitdo.h"
#include "../../src/lib/proto_jollymaam.h"
#include "../../src/lib/upgrade_8bitdo.h"
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct product {
  const char *pid, *chip;       /* chip: what flash writes (NULL: loader entry only) */
  uint16_t model;
} products[] = {
  {"U2C_WIRELESS", "TLSR8208", JM_8BITDO_MODEL_U2C_WIRELESS},
  {"U2C_RECEIVER", NULL, JM_8BITDO_MODEL_U2C_RECEIVER},
  {"U2C_BLUETOOTH", "YC3121", JM_8BITDO_MODEL_U2C_BLUETOOTH},
};
#define PRODUCT_COUNT (sizeof products / sizeof products[0])
#define BY_HAND "USB unplugged, hold L1+R1, tap Home, plug in USB"

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("jollymaam: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

static const struct product *product_named(const char *pid) {
  for (size_t i = 0; pid && i < PRODUCT_COUNT; i++)
    if (!strcmp(products[i].pid, pid))
      return &products[i];
  return NULL;
}

static const struct product *product_of(uint16_t model) {
  for (size_t i = 0; i < PRODUCT_COUNT; i++)
    if (products[i].model == model)
      return &products[i];
  return NULL;
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  long n;
  uint8_t *p;
  if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 32 * 1024 * 1024 || fseek(f, 0, SEEK_SET))
    die("%s: %s", path, f ? "cannot read" : strerror(errno));
  p = malloc(n ? (size_t)n : 1);
  if (!p || fread(p, 1, (size_t)n, f) != (size_t)n)
    die("%s: cannot read", path);
  fclose(f);
  *size = (size_t)n;
  return p;
}

static void write_file(const char *path, const uint8_t *p, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(p, 1, n, f) != n || fclose(f))
    die("%s: cannot write", path);
}

static uint32_t number(const char *s) {
  char *end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 0);
  if (errno || !*s || *end || v > 0xffffffffu)
    die("not a number: %s", s);
  return (uint32_t)v;
}

/* ---- images ------------------------------------------------------------------ */

/* pack --stamp HEX --max N -o OUT APP */
static int cmd_pack(int argc, char **argv) {
  const char *stamp = NULL, *out = NULL, *app = NULL;
  uint32_t max = 0;
  uint8_t bytes[6];
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--stamp") && i + 1 < argc) stamp = argv[++i];
    else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = number(argv[++i]);
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (argv[i][0] != '-' && !app) app = argv[i];
    else die("pack: unknown argument %s", argv[i]);
  }
  if (!stamp || !max || !out || !app)
    die("usage: jollymaam 8bitdo pack --stamp HEX --max BYTES -o OUT APP");
  if (strlen(stamp) != 12)
    die("--stamp: 6 bytes in hex (header bytes 2..7)");
  for (int i = 0; i < 6; i++) {
    char two[3] = {stamp[2 * i], stamp[2 * i + 1], 0}, *end;
    bytes[i] = (uint8_t)strtoul(two, &end, 16);
    if (*end)
      die("--stamp: not hex: %s", stamp);
  }
  size_t n, made;
  uint8_t *data = read_file(app, &n), *image = malloc(n + 20);
  if (!image)
    die("out of memory");
  if (!jm_8bitdo_telink_pack(data, n, bytes, image, n + 20, &made))
    die("%s: not a Telink app (no KNLT at 8)", app);
  if (made > max)
    die("%s: the image is %zu bytes, more than the %u it may take", app, made, max);
  write_file(out, image, made);
  free(data);
  free(image);
  return 0;
}

/* join --pid PID --at ADDR --entry ADDR [--main-out UPDATE] -o OUT UPDATER APP */
static int cmd_join(int argc, char **argv) {
  const char *pid = NULL, *out = NULL, *main_out = NULL, *files[2] = {0};
  uint32_t at = 0, entry = 0;
  int nf = 0;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = argv[++i];
    else if (!strcmp(argv[i], "--at") && i + 1 < argc) at = number(argv[++i]);
    else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = number(argv[++i]);
    else if (!strcmp(argv[i], "--main-out") && i + 1 < argc) main_out = argv[++i];
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (argv[i][0] != '-' && nf < 2) files[nf++] = argv[i];
    else die("join: unknown argument %s", argv[i]);
  }
  if (!pid || !at || !entry || !out || nf != 2)
    die("usage: jollymaam 8bitdo join --pid PID --at OFFSET --entry ADDR [--main-out UPDATE.bin] "
        "-o OUT UPDATER.bin APP.bin");
  size_t un, an;
  uint8_t *updater = read_file(files[0], &un), *app = read_file(files[1], &an);
  if (!jm_8bitdo_image_check(updater, un))
    die("%s: not a packed Telink image", files[0]);
  if (at % JM_8BITDO_SECTOR || un > at)
    die("--at: a sector boundary past the updater (%zu bytes)", un);
  size_t total = at + JM_MAIN_HEAD_SIZE + an;
  uint8_t *image = malloc(total);
  if (!image)
    die("out of memory");
  memcpy(image, updater, un);
  memset(image + un, 0xff, at - un);
  if (!jm_update_image_make(app, an, pid, at, entry, image + at, total - at))
    die("--pid: 1 to 16 characters");
  if (main_out)
    write_file(main_out, image + at, total - at);
  write_file(out, image, total);
  free(updater);
  free(app);
  free(image);
  return 0;
}

#ifndef JOLLYMAAM_PACK_ONLY
#include "jollymaam_dev.h"
#include "../../src/lib/session.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define LOADER_VID 0x2dc8
#define LOADER_PID 0x3208
static const uint16_t stock_pids[] = {0x310a, 0x301b, 0x301c, 0x301d};

/* ---- the boot loader and stock firmware, by raw HID ------------------------------ */

/* One loader request and its reply (with the loader's leading 0); false on
 * no reply or a bad one. */
static bool loader_request(struct hid_io *io, const uint8_t request[64], uint16_t command,
                           uint32_t address, const uint8_t **payload, size_t *n,
                           uint8_t reply[65], unsigned timeout_ms) {
  uint8_t out[65] = {0};
  memcpy(out + 1, request, 64);
  while (hid_io_read(io, reply, 65, 0) > 0)
    ;
  if (!hid_io_write(io, out, sizeof out))
    return false;
  for (int tries = 0; tries < 8; tries++) {
    int k = hid_io_read(io, reply, 65, timeout_ms);
    if (k <= 0)
      return false;
    enum jm_8bitdo_result r = jm_8bitdo_reply(reply, (size_t)k, command, address, payload, n);
    if (r == JM_8BITDO_OK)
      return true;
    if (r != JM_8BITDO_STALE)
      return false;
  }
  return false;
}

/* The boot loader's model at path, or 0. */
static uint16_t loader_model(const char *path) {
  struct hid_io *io = hid_io_open(path);
  uint8_t request[64], reply[65];
  const uint8_t *data;
  size_t n;
  uint16_t model = 0;
  if (!io)
    return 0;
  jm_8bitdo_request(request, 8, 0, NULL, 4);
  if (loader_request(io, request, 8, 0, &data, &n, reply, 1000) && n >= 2)
    model = (uint16_t)(data[0] | data[1] << 8);
  hid_io_close(io);
  return model;
}

/* Leave the loader: it starts the app (no reply). */
static bool loader_start(const char *path) {
  struct hid_io *io = hid_io_open(path);
  uint8_t request[64], out[65] = {0};
  bool ok;
  if (!io)
    return false;
  jm_8bitdo_request(request, 7, 0, NULL, 0);
  memcpy(out + 1, request, 64);
  ok = hid_io_write(io, out, sizeof out);
  hid_io_close(io);
  return ok;
}

#ifndef _WIN32
/* Whether a report descriptor has an Output item for report ID 0x81. */
static bool has_stock_output(const uint8_t *d, size_t n) {
  unsigned id = 0;
  for (size_t i = 0; i < n;) {
    if (d[i] == 0xfe) {
      i += 3 + (i + 1 < n ? d[i + 1] : 0);
      continue;
    }
    size_t size = (size_t[]){0, 1, 2, 4}[d[i] & 3];
    unsigned v = 0;
    for (size_t k = 0; k < size && i + 1 + k < n; k++)
      v |= (unsigned)d[i + 1 + k] << 8 * k;
    if ((d[i] & 0xfc) == 0x84)
      id = v;
    else if ((d[i] & 0xfc) == 0x90 && id == 0x81)
      return true;
    i += 1 + size;
  }
  return false;
}
#endif

/* Stock firmware's vendor interface: output report 0x81 takes the loader's
 * commands (8: the model, answered on input report 0x02; 0x51: restart
 * into the boot loader). */
static bool stock_interface(const struct hid_dev *h) {
  bool pid = false;
  for (size_t i = 0; i < sizeof stock_pids / sizeof stock_pids[0]; i++)
    pid |= h->product == stock_pids[i];
  if (h->vid != LOADER_VID || !pid)
    return false;
#ifdef _WIN32
  return h->out_len != 0;
#else
  return has_stock_output(h->desc, h->desc_n);
#endif
}

static uint16_t stock_model(const char *path) {
  struct hid_io *io = hid_io_open(path);
  uint8_t out[64] = {0x81, 0x05, 0x08}, in[65];
  uint16_t model = 0;
  if (!io)
    return 0;
  while (hid_io_read(io, in, sizeof in, 0) > 0)
    ;
  if (hid_io_write(io, out, sizeof out))
    for (int tries = 0; tries < 16; tries++) {
      int k = hid_io_read(io, in, sizeof in, 500);
      if (k <= 0)
        break;
      if (k >= 20 && in[0] == 0x02 && in[1] == 0x05 && !in[2] && !in[3] && in[4] == 8 && !in[5] &&
          in[6] == 2) {
        model = (uint16_t)(in[18] | in[19] << 8);
        break;
      }
    }
  hid_io_close(io);
  return model;
}

static bool stock_to_loader(const char *path) {
  struct hid_io *io = hid_io_open(path);
  uint8_t out[64] = {0x81, 0x05, 0x00, 0x51, 0x00};
  bool ok;
  if (!io)
    return false;
  ok = hid_io_write(io, out, sizeof out);
  hid_io_close(io);
  /* The restart may beat the write's completion. */
  return ok || errno == ENODEV || errno == EPIPE || errno == ESHUTDOWN || errno == ECONNRESET;
}

/* ---- finding the device -------------------------------------------------------------- */

enum kind { LOADER, OURS, STOCK };
struct cand {
  enum kind kind;
  char path[300], port[64];
  const struct product *product;
};

static bool chosen(const struct cand *c, const char *device) {
  return !device || !strcmp(device, c->path) || (c->port[0] && !strcmp(device, c->port));
}

static const char *what(const struct cand *c) {
  return c->kind == LOADER ? "boot loader" : c->kind == OURS ? "our firmware" : "stock firmware";
}

/* Everything for want (NULL: any), on device (NULL: anywhere). */
static size_t survey(const struct product *want, const char *device, struct cand *c, size_t max) {
  static struct hid_dev h[64];
  size_t nh = hid_devices(h, 64), n = 0;
  char pid[17];
  for (size_t i = 0; i < nh && n < max; i++) {
    struct cand *x = &c[n];
    memset(x, 0, sizeof *x);
    snprintf(x->path, sizeof x->path, "%.299s", h[i].path);
    if (!usb_port(x->path, x->port))
      x->port[0] = 0;
    if (!chosen(x, device) || restarting(x->path))
      continue;
    if (h[i].vid == LOADER_VID && h[i].product == LOADER_PID) {
      x->kind = LOADER;
      x->product = product_of(loader_model(x->path));
    } else if (is_tool(&h[i]) && tool_pid(&h[i], pid) && product_named(pid)) {
      x->kind = OURS;
      x->product = product_named(pid);
    } else if (stock_interface(&h[i])) {
      x->kind = STOCK;
      x->product = product_of(stock_model(x->path));
    } else {
      continue;
    }
    if (!x->product || (want && x->product != want))
      continue;
    bool dup = false;
    for (size_t k = 0; k < n && !dup; k++)
      dup = c[k].port[0] && !strcmp(c[k].port, x->port) && c[k].kind == x->kind;
    if (!dup)
      n++;
  }
  return n;
}

/* The boot loader holding want on device (NULL: the only one), into path:
 * our firmware or stock firmware is asked to restart into it, then it is
 * waited for, wait_s seconds at most.  Exits on more than one or none. */
static void enter_loader(const struct product *want, const char *device, unsigned wait_s, char path[300]) {
  struct cand c[16];
  for (unsigned t = 0;; t++) {
    size_t n = survey(want, device, c, 16);
    if (n > 1) {
      fprintf(stderr, "jollymaam: more than one device for %s:\n", want->pid);
      for (size_t i = 0; i < n; i++)
        fprintf(stderr, "  %-24s %s  (%s)\n", c[i].port[0] ? c[i].port : c[i].path, what(&c[i]), c[i].path);
      die("pick one with --device PORT (make: DEVICE=PORT); jollymaam scan lists them");
    }
    if (n == 1 && c[0].kind == LOADER) {
      printf("%s: boot loader, %s\n", c[0].path, want->pid);
      strcpy(path, c[0].path);
      return;
    }
    if (n == 1) {
      bool sent;
      printf("%s: %s, %s: asking it to restart into its boot loader\n", c[0].path, what(&c[0]), want->pid);
      if (c[0].kind == OURS) {
        static struct hid_dev h[64];
        size_t nh = hid_devices(h, 64);
        uint8_t cdb[16] = {0};
        jm_command(cdb, JM_CMD_REBOOT_LOADER, 0);
        sent = false;
        for (size_t i = 0; i < nh && !sent; i++)
          if (!strcmp(h[i].path, c[0].path))
            sent = tool_cmd(&h[i], cdb, NULL, 0);
      } else {
        sent = stock_to_loader(c[0].path);
      }
      if (sent)
        dev_mark_restarting(c[0].path);
      if (!device && c[0].port[0])
        device = strcpy(malloc(64), c[0].port);   /* its loader comes back on that port */
    }
    if (t >= wait_s * 2)
      die("no boot loader for %s%s\n  by hand: %s", want->pid, device ? " there" : "", BY_HAND);
    if (!t && !n)
      printf("waiting up to %u s for %s\n", wait_s, want->pid);
    dev_sleep(500);
  }
}

/* ---- flash, through libjollymaam's session ------------------------------------------------ */

static int io_write(void *user, const uint8_t *data, size_t n) {
  uint8_t out[65] = {0};
  if (n != 64)
    return -1;
  memcpy(out + 1, data, 64);
  return hid_io_write(user, out, sizeof out) ? 64 : -1;
}
static int io_read(void *user, uint8_t *out, size_t n, unsigned timeout_ms) {
  return hid_io_read(user, out, n, timeout_ms);
}
static void io_wait(void *user, unsigned ms) {
  (void)user;
  dev_sleep(ms);
}

static void progress(void *user, size_t done, size_t total) {
  int *last = user;
  int pct = total ? (int)(done * 100 / total) : 100;
#ifdef _WIN32
  int tty = _isatty(_fileno(stderr));
#else
  int tty = isatty(fileno(stderr));
#endif
  if (pct == *last)
    return;
  *last = pct;
  if (tty)
    fprintf(stderr, "\rwriting and reading back %3d%%%s", pct, pct == 100 ? "\n" : "");
  else if (pct % 25 == 0)
    fprintf(stderr, "writing and reading back %d%%\n", pct);
}

struct opts {
  const struct product *product;
  const char *device, *image;
  unsigned wait;
};

static void parse(int argc, char **argv, struct opts *o, bool image) {
  memset(o, 0, sizeof *o);
  o->wait = 15;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
      if (!(o->product = product_named(argv[++i])))
        die("--pid: U2C_WIRELESS, U2C_BLUETOOTH or U2C_RECEIVER, not %s", argv[i]);
    } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
      o->device = argv[++i];
    } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
      o->wait = number(argv[++i]);
    } else if (image && argv[i][0] != '-' && !o->image) {
      o->image = argv[i];
    } else {
      die("unknown argument %s", argv[i]);
    }
  }
  if (!o->product)
    die("--pid PID is needed (U2C_WIRELESS, U2C_BLUETOOTH, U2C_RECEIVER)");
  if (image && !o->image)
    die("flash needs an IMAGE");
}

static int cmd_flash(int argc, char **argv) {
  struct opts o;
  char path[300];
  size_t n;
  int last = -1;
  parse(argc, argv, &o, true);
  if (!o.product->chip)
    die("%s: flashing it is not established; loader entry only", o.product->pid);
  uint8_t *image = read_file(o.image, &n);
  struct jm_firmware_target target;
  struct jm_8bitdo_upgrade check;
  memset(&target, 0, sizeof target);
  snprintf(target.chip, sizeof target.chip, "%s", o.product->chip);
  target.install.data = image;
  target.install.size = n;
  if (!strcmp(o.product->chip, "YC3121") ? !jm_8bitdo_yichip_init(&check, image, n)
                                         : !jm_8bitdo_upgrade_init(&check, image, n))
    die("%s: not an image for %s", o.image, o.product->pid);
  enter_loader(o.product, o.device, o.wait, path);
  struct hid_io *io = hid_io_open(path);
  if (!io)
    die("%s: cannot open", path);
  struct jm_session_io sio = {0};
  sio.hid_write = io_write;
  sio.hid_read = io_read;
  sio.wait_ms = io_wait;
  int result = jm_session_run(&target, JM_SESSION_8BITDO, NULL, &sio, io, progress, &last);
  hid_io_close(io);
  free(image);
  if (result)
    die("flashing failed (%d); the pad stays in its boot loader (jollymaam 8bitdo flash again)", result);
  puts("written and read back; the pad starts it");
  return 0;
}

static int cmd_loader(int argc, char **argv) {
  struct opts o;
  char path[300];
  parse(argc, argv, &o, false);
  enter_loader(o.product, o.device, o.wait, path);
  return 0;
}

static int cmd_start(int argc, char **argv) {
  struct opts o;
  struct cand c[16];
  parse(argc, argv, &o, false);
  size_t n = survey(o.product, o.device, c, 16), k = 0;
  for (size_t i = 0; i < n; i++)
    if (c[i].kind == LOADER)
      c[k++] = c[i];
  if (k != 1)
    die(k ? "more than one boot loader for %s: pick one with --device PORT" : "no boot loader for %s",
        o.product->pid);
  if (!loader_start(c[0].path))
    die("%s: cannot write", c[0].path);
  printf("%s: started the app\n", c[0].path);
  return 0;
}

/* jollymaam scan's part: boot loaders and stock firmware. */
void jollymaam_8bitdo_scan(void) {
  struct cand c[16];
  size_t n = survey(NULL, NULL, c, 16);
  for (size_t i = 0; i < n; i++)
    if (c[i].kind != OURS)
      printf("%-24s 8BitDo %s, %s  (%s)\n", c[i].port[0] ? c[i].port : "-", what(&c[i]),
             c[i].product->pid, c[i].path);
}
#endif /* JOLLYMAAM_PACK_ONLY */

static void usage(FILE *out) {
  fputs("usage: jollymaam 8bitdo <subcommand>\n"
        "  pack --stamp HEX --max BYTES -o OUT APP.bin\n"
        "        a Telink app from objcopy as the boot loader starts it\n"
        "  join --pid PID --at OFFSET --entry ADDR [--main-out UPDATE.bin] -o OUT UPDATER.bin APP.bin\n"
        "        the updater and the app (behind its MAIN header, at OFFSET) as one image\n"
#ifndef JOLLYMAAM_PACK_ONLY
        "  flash --pid PID [--device PORT] [--wait SECONDS] IMAGE\n"
        "        into the boot loader (from our or stock firmware), written, read back, started:\n"
        "        U2C_WIRELESS firmware.img, U2C_BLUETOOTH a raw app\n"
        "  loader --pid PID [--device PORT] [--wait SECONDS]\n"
        "  start --pid PID [--device PORT]\n"
        "  check-stock 0x301b STOCK.bin|DAT\n"
        "  restore --yes HID_PATH 0x301b STOCK.bin|DAT NEW_BACKUP\n"
        "PID: U2C_WIRELESS (Telink), U2C_BLUETOOTH (Yichip), U2C_RECEIVER (loader only).\n"
        "By hand: " BY_HAND ".\n"
#endif
        , out);
}

int jollymaam_8bitdo_command(const char *command) {
  static const char *const commands[] = {"pack", "join",
#ifndef JOLLYMAAM_PACK_ONLY
                                         "flash", "loader", "start",
#endif
                                         "help", "--help"};
  for (size_t i = 0; command && i < sizeof commands / sizeof *commands; i++)
    if (!strcmp(command, commands[i]))
      return 1;
  return 0;
}

int jollymaam_8bitdo_app(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
    usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? 2 : 0;
  }
  if (!strcmp(argv[1], "pack"))
    return cmd_pack(argc - 2, argv + 2);
  if (!strcmp(argv[1], "join"))
    return cmd_join(argc - 2, argv + 2);
#ifndef JOLLYMAAM_PACK_ONLY
  if (!strcmp(argv[1], "flash"))
    return cmd_flash(argc - 2, argv + 2);
  if (!strcmp(argv[1], "loader"))
    return cmd_loader(argc - 2, argv + 2);
  if (!strcmp(argv[1], "start"))
    return cmd_start(argc - 2, argv + 2);
#endif
  usage(stderr);
  return 2;
}

void jollymaam_8bitdo_usage(FILE *out) {
  usage(out);
}
