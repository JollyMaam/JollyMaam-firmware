/* libjollymaam's hooks through hidapi, for systems without native ones
 * (macOS): HID only, so no Jieli ROM loaders (USB disks), and no USB ports
 * (a device is its path). */
#define _POSIX_C_SOURCE 200809L
#include "hooks.h"
#include <hidapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

static int list(void *ctx, int (*visit)(void *, const struct jm_device_info *), void *user) {
  struct hid_device_info *all, *p;
  int n = 0;
  (void)ctx;
  if (hid_init()) return -1;
  if (!(all = hid_enumerate(0, 0))) return 0;
  for (p = all; p; p = p->next) {
    struct jm_device_info d;
    memset(&d, 0, sizeof d);
    d.kind = JM_DEVICE_HID;
    snprintf(d.path, sizeof d.path, "%s", p->path);
    snprintf(d.id, sizeof d.id, "%.95s", p->path);
    d.vid = p->vendor_id, d.pid = p->product_id, d.bcd = p->release_number;
    if (p->manufacturer_string) wcstombs(d.manufacturer, p->manufacturer_string, sizeof d.manufacturer - 1);
    if (p->product_string) wcstombs(d.product, p->product_string, sizeof d.product - 1);
    hid_device *dev = hid_open_path(p->path);
    uint8_t desc[4096];
    int k = dev ? hid_get_report_descriptor(dev, desc, sizeof desc) : -1;
    if (k > 0) jm_hid_descriptor(desc, (size_t)k, &d);
    if (dev) hid_close(dev);
    d.usage_page = p->usage_page ? p->usage_page : d.usage_page;
    n++;
    if (visit(user, &d)) break;
  }
  hid_free_enumeration(all);
  return n;
}

static void *open_device(void *ctx, const struct jm_device_info *d) {
  (void)ctx;
  return hid_open_path(d->path);
}
static void close_device(void *ctx, void *dev) { (void)ctx, hid_close(dev); }
static int write_report(void *ctx, void *dev, const uint8_t *r, size_t n) { (void)ctx; return hid_write(dev, r, n); }
static int read_report(void *ctx, void *dev, uint8_t *out, size_t n, unsigned timeout_ms) {
  (void)ctx;
  return hid_read_timeout(dev, out, n, (int)timeout_ms);
}
static int set_feature(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)ctx;
  return hid_send_feature_report(dev, r, n);
}
static void nap(void *ctx, unsigned ms) {
  (void)ctx;
  nanosleep(&(struct timespec){ms / 1000, (long)(ms % 1000) * 1000000}, NULL);
}

void hooks_device(struct jm_hooks *h) {
  h->list = list, h->open = open_device, h->close = close_device;
  h->write = write_report, h->read = read_report, h->set_feature = set_feature;
  h->scsi = NULL, h->sleep = nap;
}
