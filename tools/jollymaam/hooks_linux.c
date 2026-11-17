/* libjollymaam's hooks on Linux: HID interfaces (hidraw) and USB disks (SCSI
 * generic, SG_IO), their USB ports from sysfs.  DEV_ROOT: the directory
 * holding the device nodes (default /dev). */
#define _GNU_SOURCE
#include "hooks.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const char *root(void) {
  const char *r = getenv("DEV_ROOT");
  return r && *r ? r : "/dev";
}

static void line(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "r");
  out[0] = 0;
  if (f && fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0;
  if (f) fclose(f);
}

/* The sysfs directory of the USB device behind a node: the part above its
 * interface (".../usb5/5-2/5-2.1/5-2.1:1.0/..."). */
static int usb_dir(const char *class, const char *name, char dir[400]) {
  char link[400], *real, *c;
  int ok = 0;
  snprintf(link, sizeof link, "/sys/class/%s/%s/device", class, name);
  real = realpath(link, NULL);
  for (c = real && strstr(real, "/usb") ? strchr(real, '/') : NULL; c; c = strchr(c + 1, '/')) {
    char *end = strchr(c + 1, '/');
    size_t len = end ? (size_t)(end - c - 1) : strlen(c + 1);
    char *colon = memchr(c + 1, ':', len), *dash = memchr(c + 1, '-', len);
    if (c[1] >= '0' && c[1] <= '9' && colon && dash && dash < colon) {
      *c = 0;
      snprintf(dir, 400, "%s", real);
      ok = 1;
      break;
    }
  }
  free(real);
  return ok;
}

/* Port, the enumeration's ID (port and device number), IDs and strings. */
static void usb_info(const char *class, const char *name, struct jm_device_info *d) {
  char dir[400], path[480], text[80];
  if (!usb_dir(class, name, dir)) return;
  snprintf(d->port, sizeof d->port, "%s", strrchr(dir, '/') + 1);
  snprintf(path, sizeof path, "%s/devnum", dir);
  line(path, text, sizeof text);
  snprintf(d->id, sizeof d->id, "%.60s#%.20s", d->port, text);
  snprintf(path, sizeof path, "%s/idVendor", dir), line(path, text, sizeof text), d->vid = (uint16_t)strtoul(text, NULL, 16);
  snprintf(path, sizeof path, "%s/idProduct", dir), line(path, text, sizeof text), d->pid = (uint16_t)strtoul(text, NULL, 16);
  snprintf(path, sizeof path, "%s/bcdDevice", dir), line(path, text, sizeof text), d->bcd = (uint16_t)strtoul(text, NULL, 16);
  snprintf(path, sizeof path, "%s/manufacturer", dir), line(path, d->manufacturer, sizeof d->manufacturer);
  snprintf(path, sizeof path, "%s/product", dir), line(path, d->product, sizeof d->product);
}

static int list(void *ctx, int (*visit)(void *, const struct jm_device_info *), void *user) {
  DIR *dir = opendir(root());
  struct dirent *e;
  int n = 0;
  (void)ctx;
  while (dir && (e = readdir(dir))) {
    struct jm_device_info d;
    int hid = !strncmp(e->d_name, "hidraw", 6);
    if (!hid && strncmp(e->d_name, "sg", 2)) continue;
    memset(&d, 0, sizeof d);
    d.kind = hid ? JM_DEVICE_HID : JM_DEVICE_DISK;
    snprintf(d.path, sizeof d.path, "%s/%s", root(), e->d_name);
    if (access(d.path, R_OK | W_OK)) continue;      /* the nodes we may use */
    usb_info(hid ? "hidraw" : "scsi_generic", e->d_name, &d);
    if (hid) {
      char sys[300];
      uint8_t desc[4096];
      snprintf(sys, sizeof sys, "/sys/class/hidraw/%s/device/report_descriptor", e->d_name);
      FILE *f = fopen(sys, "rb");
      if (f) jm_hid_descriptor(desc, fread(desc, 1, sizeof desc, f), &d), fclose(f);
    }
    n++;
    if (visit(user, &d)) break;
  }
  if (dir) closedir(dir);
  return n;
}

static void *open_device(void *ctx, const struct jm_device_info *d) {
  (void)ctx;
  int fd = open(d->path, O_RDWR | O_CLOEXEC | (d->kind == JM_DEVICE_HID ? O_NONBLOCK : 0));
  return fd < 0 ? NULL : (void *)(intptr_t)(fd + 1);
}
static int fd_of(void *dev) { return (int)(intptr_t)dev - 1; }
static void close_device(void *ctx, void *dev) { (void)ctx, close(fd_of(dev)); }

static int write_report(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)ctx;
  return (int)write(fd_of(dev), r, n);
}
static int read_report(void *ctx, void *dev, uint8_t *out, size_t n, unsigned timeout_ms) {
  struct pollfd p = {fd_of(dev), POLLIN, 0};
  (void)ctx;
  if (poll(&p, 1, (int)timeout_ms) <= 0) return 0;
  ssize_t k = read(fd_of(dev), out, n);
  return k < 0 ? (errno == EAGAIN ? 0 : -1) : (int)k;
}
static int set_feature(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)ctx;
  return ioctl(fd_of(dev), HIDIOCSFEATURE(n), r) < 0 ? -1 : (int)n;
}

/* A command's status alone: the Jieli loaders report a bogus residue, so a
 * good command moved what was asked. */
static int scsi(void *ctx, void *dev, const uint8_t *cdb, size_t cdb_size, enum jm_scsi_direction dir,
                uint8_t *data, size_t size, unsigned timeout_ms, size_t *done) {
  uint8_t sense[32];
  sg_io_hdr_t h;
  (void)ctx;
  memset(&h, 0, sizeof h);
  h.interface_id = 'S';
  h.cmd_len = (unsigned char)cdb_size, h.cmdp = (unsigned char *)cdb;
  h.dxfer_direction = !size ? SG_DXFER_NONE : dir == JM_SCSI_IN ? SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;
  h.dxferp = data, h.dxfer_len = (unsigned)size;
  h.sbp = sense, h.mx_sb_len = sizeof sense, h.timeout = timeout_ms;
  if (ioctl(fd_of(dev), SG_IO, &h) || (h.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
  *done = size;
  return 0;
}

static void nap(void *ctx, unsigned ms) {
  (void)ctx;
  nanosleep(&(struct timespec){ms / 1000, (long)(ms % 1000) * 1000000}, NULL);
}

void hooks_device(struct jm_hooks *h) {
  h->list = list, h->open = open_device, h->close = close_device;
  h->write = write_report, h->read = read_report, h->set_feature = set_feature;
  h->scsi = scsi, h->sleep = nap;
}
