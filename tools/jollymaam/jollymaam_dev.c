/* The jollymaam app's devices on Linux and Windows: HID interfaces and disk
 * nodes, the USB port behind each (what --device and DEVICE= take), devices
 * asked to restart into their loader, our firmware's tool device, and raw
 * HID reports. */
#define _GNU_SOURCE
#include "jollymaam_dev.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#else
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "hid.lib")
#endif
#endif

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }

/* Where device nodes are: $DEV_ROOT when set (every node in one directory,
 * as a container exports them), else /dev. */
const char *dev_root(void) {
  const char *r = getenv("DEV_ROOT");
  return r && *r ? r : "/dev";
}


void dev_sleep(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  nanosleep(&(struct timespec){ms / 1000, (ms % 1000) * 1000000L}, NULL);
#endif
}

device_fd dev_open(const char *path) {
#ifdef _WIN32
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  return h == INVALID_HANDLE_VALUE ? -1 : (device_fd)h;
#else
  return open(path, O_RDWR);
#endif
}
void dev_close(device_fd fd) {
#ifdef _WIN32
  CloseHandle((HANDLE)fd);
#else
  close((int)fd);
#endif
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

bool usb_port(const char *path, char port[64]) {
  return usb_node(path, port, NULL);
}

/* "" if not known. */
void usb_device(const char *path, char id[USB_ID_MAX]) {
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
bool restarting(const char *path) {
  char id[USB_ID_MAX];
  usb_device(path, id);
  for (size_t i = 0; id[0] && i < n_asked; i++)
    if (!strcmp(asked[i], id))
      return true;
  return false;
}

/* path's device was asked to restart into its loader. */
void dev_mark_restarting(const char *path) {
  if (n_asked < 16)
    usb_device(path, asked[n_asked++]);
}


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
bool has_usage_page(const uint8_t *d, size_t n, unsigned page) {
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
size_t hid_devices(struct hid_dev *out, size_t max) {
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
bool told_before(const char *path) {
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


/* Our firmware's tool device: a HID interface, usage page 0xffc2 (usb.c and
 * the platform's tool device).  A command is a feature report (the CDB, u16
 * length, flags, tag, u32 client), up to 512 bytes; its answer comes on
 * 64-byte input reports: the client, the tag, a state (2 a piece and more,
 * 3 the last, 4 refused: another program's command runs), u16 offset and
 * 56 bytes of answer.  Every program with the device open gets them all. */
bool is_tool(const struct hid_dev *h) {
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
bool tool_cmd(const struct hid_dev *h, const uint8_t cdb[16], uint8_t *ans, size_t n) {
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
bool tool_pid(const struct hid_dev *h, char pid[17]) {
  uint8_t cdb[16] = {0xf5, 0x00}, ident[56] = {0};
  if (!tool_cmd(h, cdb, ident, sizeof ident) || memcmp(ident, "C2PD", 4))
    return false;
  memcpy(pid, ident + 40, 16);
  pid[16] = 0;
  return true;
}


/* ---- raw HID reports (the 8BitDo boot loader, stock firmware) ------------ */

struct hid_io {
#ifdef _WIN32
  HANDLE h, ev;
#else
  int fd;
#endif
};

struct hid_io *hid_io_open(const char *path) {
  struct hid_io *io = calloc(1, sizeof *io);
  if (!io)
    return NULL;
#ifdef _WIN32
  io->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  io->ev = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (io->h == INVALID_HANDLE_VALUE || !io->ev) {
    if (io->h != INVALID_HANDLE_VALUE)
      CloseHandle(io->h);
    if (io->ev)
      CloseHandle(io->ev);
    free(io);
    return NULL;
  }
#else
  io->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (io->fd < 0) {
    free(io);
    return NULL;
  }
#endif
  return io;
}

void hid_io_close(struct hid_io *io) {
  if (!io)
    return;
#ifdef _WIN32
  CloseHandle(io->ev);
  CloseHandle(io->h);
#else
  close(io->fd);
#endif
  free(io);
}

/* A report: its ID byte (0: none), then the report; n bytes in all.
 * Windows takes the whole output report: shorter ones are padded. */
bool hid_io_write(struct hid_io *io, const uint8_t *data, size_t n) {
#ifdef _WIN32
  uint8_t buf[1025] = {0};
  OVERLAPPED ov = {0};
  DWORD done = 0;
  if (n > sizeof buf)
    return false;
  memcpy(buf, data, n);
  if (n < 65)
    n = 65;
  ov.hEvent = io->ev;
  ResetEvent(io->ev);
  if (!WriteFile(io->h, buf, (DWORD)n, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
    return false;
  if (WaitForSingleObject(io->ev, 2000) != WAIT_OBJECT_0) {
    CancelIo(io->h);
    GetOverlappedResult(io->h, &ov, &done, TRUE);
    return false;
  }
  return GetOverlappedResult(io->h, &ov, &done, TRUE) && done == n;
#else
  return write(io->fd, data, n) == (ssize_t)n;
#endif
}

/* One input report as hidraw gives it (its ID byte only when numbered), at
 * most n bytes: its length, 0 after timeout_ms, -1 on an error. */
int hid_io_read(struct hid_io *io, uint8_t *out, size_t n, unsigned timeout_ms) {
#ifdef _WIN32
  uint8_t buf[1025];
  OVERLAPPED ov = {0};
  DWORD done = 0;
  ov.hEvent = io->ev;
  ResetEvent(io->ev);
  if (!ReadFile(io->h, buf, sizeof buf, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
    return -1;
  if (WaitForSingleObject(io->ev, timeout_ms) != WAIT_OBJECT_0) {
    CancelIo(io->h);
    GetOverlappedResult(io->h, &ov, &done, TRUE);
    if (!done)
      return 0;
  } else if (!GetOverlappedResult(io->h, &ov, &done, TRUE)) {
    return -1;
  }
  if (!done)
    return 0;
  /* Windows: the ID byte first, 0 for an unnumbered report. */
  size_t at = buf[0] == 0 ? 1 : 0, k = done - at;
  if (k > n)
    k = n;
  memcpy(out, buf + at, k);
  return (int)k;
#else
  struct pollfd p = {io->fd, POLLIN, 0};
  if (poll(&p, 1, (int)timeout_ms) <= 0)
    return 0;
  ssize_t k = read(io->fd, out, n);
  if (k < 0)
    return errno == EAGAIN ? 0 : -1;
  return (int)k;
#endif
}
