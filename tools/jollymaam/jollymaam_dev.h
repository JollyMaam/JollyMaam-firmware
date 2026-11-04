#ifndef JOLLYMAAM_DEV_H
#define JOLLYMAAM_DEV_H
/* The jollymaam app's devices (jollymaam_dev.c): Linux and Windows. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef intptr_t device_fd;
const char *dev_root(void);
void dev_sleep(unsigned ms);
device_fd dev_open(const char *path);
void dev_close(device_fd fd);

/* The USB device behind a disk or HID node: its port (Linux "5-2.1",
 * Windows "Port_#0002.Hub_#0003"), and an ID that also changes with each
 * enumeration ("" if not known). */
#define USB_ID_MAX 300
bool usb_port(const char *path, char port[64]);
void usb_device(const char *path, char id[USB_ID_MAX]);
/* Devices asked to restart into their loader get nothing more until they
 * have left the bus. */
bool restarting(const char *path);
void dev_mark_restarting(const char *path);
/* Whether path's device was already named in a message. */
bool told_before(const char *path);

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
size_t hid_devices(struct hid_dev *out, size_t max);
#ifndef _WIN32
bool has_usage_page(const uint8_t *d, size_t n, unsigned page);
#endif

#define TOOL_PAGE   0xffc2
#define TOOL_REPORT 512
#define TOOL_INPUT  2
bool is_tool(const struct hid_dev *h);
bool tool_cmd(const struct hid_dev *h, const uint8_t cdb[16], uint8_t *ans, size_t n);
bool tool_pid(const struct hid_dev *h, char pid[17]);

struct hid_io;
struct hid_io *hid_io_open(const char *path);
void hid_io_close(struct hid_io *io);
bool hid_io_write(struct hid_io *io, const uint8_t *data, size_t n);
int hid_io_read(struct hid_io *io, uint8_t *out, size_t n, unsigned timeout_ms);
#endif
