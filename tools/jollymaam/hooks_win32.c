/* libjollymaam's hooks on Windows: HID interfaces (hid.dll, overlapped I/O)
 * and USB disks (SCSI pass-through), their USB ports from SetupAPI. */
#include "hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>


/* The USB device above a device instance: its location (port) and instance ID. */
static int usb_info(DEVINST d, struct jm_device_info *out) {
  char instance[200], location[64];
  ULONG len = sizeof location;
  DEVINST parent;
  for (;;) {
    if (CM_Get_Device_IDA(d, instance, sizeof instance, 0) != CR_SUCCESS) return 0;
    if (!strncmp(instance, "USB\\", 4) && !strstr(instance, "&MI_")) break;
    if (CM_Get_Parent(&parent, d, 0) != CR_SUCCESS) return 0;
    d = parent;
  }
  if (CM_Get_DevNode_Registry_PropertyA(d, CM_DRP_LOCATION_INFORMATION, NULL, location, &len, 0) != CR_SUCCESS)
    return 0;
  snprintf(out->port, sizeof out->port, "%s", location);
  snprintf(out->id, sizeof out->id, "%.40s#%.50s", location, instance);
  return 1;
}

static void hid_attributes(HANDLE h, struct jm_device_info *d) {
  HIDD_ATTRIBUTES a = {.Size = sizeof a};
  PHIDP_PREPARSED_DATA pp;
  HIDP_CAPS caps = {0};
  WCHAR w[64];
  if (!HidD_GetAttributes(h, &a)) return;
  d->vid = a.VendorID, d->pid = a.ProductID, d->bcd = a.VersionNumber;
  if (HidD_GetPreparsedData(h, &pp)) HidP_GetCaps(pp, &caps), HidD_FreePreparsedData(pp);
  d->usage_page = caps.UsagePage;
  d->feature_size = caps.FeatureReportByteLength ? caps.FeatureReportByteLength - 1 : 0;
  d->output_size = caps.OutputReportByteLength ? caps.OutputReportByteLength - 1 : 0;
  if (HidD_GetManufacturerString(h, w, sizeof w))
    WideCharToMultiByte(CP_UTF8, 0, w, -1, d->manufacturer, sizeof d->manufacturer, NULL, NULL);
  if (HidD_GetProductString(h, w, sizeof w))
    WideCharToMultiByte(CP_UTF8, 0, w, -1, d->product, sizeof d->product, NULL, NULL);
}

/* The interfaces of a class: HID ones, or disks (by \\.\PhysicalDriveN). */
static int list_class(const GUID *guid, enum jm_device_kind kind, int (*visit)(void *, const struct jm_device_info *),
                      void *user, int *stop) {
  HDEVINFO set = SetupDiGetClassDevsA(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  SP_DEVICE_INTERFACE_DATA it = {.cbSize = sizeof it};
  int n = 0;
  for (DWORD k = 0; !*stop && set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, guid, k, &it); k++) {
    union {
      SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
      char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 300];
    } detail;
    SP_DEVINFO_DATA info = {.cbSize = sizeof info};
    struct jm_device_info d;
    detail.d.cbSize = sizeof detail.d;
    if (!SetupDiGetDeviceInterfaceDetailA(set, &it, &detail.d, sizeof detail, NULL, &info)) continue;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    if (!usb_info(info.DevInst, &d)) continue;
    /* No access asked: attributes need none, even for a keyboard Windows keeps. */
    HANDLE h = CreateFileA(detail.d.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) continue;
    if (kind == JM_DEVICE_HID) {
      snprintf(d.path, sizeof d.path, "%s", detail.d.DevicePath);
      hid_attributes(h, &d);
    } else {
      STORAGE_DEVICE_NUMBER num;
      DWORD got;
      if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &num, sizeof num, &got, NULL)) {
        CloseHandle(h);
        continue;
      }
      snprintf(d.path, sizeof d.path, "\\\\.\\PhysicalDrive%lu", (unsigned long)num.DeviceNumber);
    }
    CloseHandle(h);
    n++;
    *stop = visit(user, &d);
  }
  if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
  return n;
}

static int list(void *ctx, int (*visit)(void *, const struct jm_device_info *), void *user) {
  static const GUID disk = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
  GUID hid;
  int stop = 0;
  (void)ctx;
  HidD_GetHidGuid(&hid);
  int n = list_class(&hid, JM_DEVICE_HID, visit, user, &stop);
  return n + list_class(&disk, JM_DEVICE_DISK, visit, user, &stop);
}

struct dev { HANDLE h, ev; };
static void *open_device(void *ctx, const struct jm_device_info *d) {
  struct dev *v = calloc(1, sizeof *v);
  (void)ctx;
  if (!v) return NULL;
  v->h = CreateFileA(d->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                     OPEN_EXISTING, d->kind == JM_DEVICE_HID ? FILE_FLAG_OVERLAPPED : 0, NULL);
  v->ev = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (v->h == INVALID_HANDLE_VALUE || !v->ev) {
    if (v->h != INVALID_HANDLE_VALUE) CloseHandle(v->h);
    if (v->ev) CloseHandle(v->ev);
    free(v);
    return NULL;
  }
  return v;
}
static void close_device(void *ctx, void *dev) {
  struct dev *v = dev;
  (void)ctx;
  CloseHandle(v->ev), CloseHandle(v->h), free(v);
}

/* One overlapped transfer: the bytes moved, 0 after timeout_ms, -1. */
static int overlapped(struct dev *v, int reading, uint8_t *buf, DWORD n, unsigned timeout_ms) {
  OVERLAPPED ov = {0};
  DWORD done = 0;
  ov.hEvent = v->ev;
  ResetEvent(v->ev);
  BOOL ok = reading ? ReadFile(v->h, buf, n, NULL, &ov) : WriteFile(v->h, buf, n, NULL, &ov);
  if (!ok && GetLastError() != ERROR_IO_PENDING) return -1;
  if (WaitForSingleObject(v->ev, timeout_ms) != WAIT_OBJECT_0) {
    CancelIo(v->h);
    GetOverlappedResult(v->h, &ov, &done, TRUE);
    return (int)done;
  }
  return GetOverlappedResult(v->h, &ov, &done, TRUE) ? (int)done : -1;
}

/* Windows takes whole reports: a shorter one is padded to 65 bytes. */
static int write_report(void *ctx, void *dev, const uint8_t *r, size_t n) {
  uint8_t buf[1025] = {0};
  (void)ctx;
  if (n > sizeof buf) return -1;
  memcpy(buf, r, n);
  int k = overlapped(dev, 0, buf, n < 65 ? 65 : (DWORD)n, 2000);
  return k > 0 ? (int)n : -1;
}
/* Windows gives the ID byte first, 0 for an unnumbered report: dropped. */
static int read_report(void *ctx, void *dev, uint8_t *out, size_t n, unsigned timeout_ms) {
  uint8_t buf[1025];
  (void)ctx;
  int k = overlapped(dev, 1, buf, sizeof buf, timeout_ms);
  if (k <= 0) return k;
  size_t at = buf[0] == 0, m = (size_t)k - at;
  memcpy(out, buf + at, m < n ? m : n);
  return (int)(m < n ? m : n);
}
static int set_feature(void *ctx, void *dev, const uint8_t *r, size_t n) {
  struct dev *v = dev;
  (void)ctx;
  return HidD_SetFeature(v->h, (PVOID)r, (ULONG)n) ? (int)n : -1;
}

/* Buffered pass-through: no alignment rules for the data. */
static int scsi(void *ctx, void *dev, const uint8_t *cdb, size_t cdb_size, enum jm_scsi_direction dir,
                uint8_t *data, size_t size, unsigned timeout_ms, size_t *done) {
  struct dev *v = dev;
  size_t offset = sizeof(SCSI_PASS_THROUGH) + 32, total = offset + size;
  uint8_t *packet = calloc(1, total);
  DWORD got;
  (void)ctx;
  if (!packet) return -1;
  SCSI_PASS_THROUGH *h = (SCSI_PASS_THROUGH *)packet;
  h->Length = sizeof *h, h->CdbLength = (UCHAR)cdb_size;
  h->SenseInfoLength = 32, h->SenseInfoOffset = sizeof *h;
  h->DataIn = !size ? SCSI_IOCTL_DATA_UNSPECIFIED : dir == JM_SCSI_IN ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
  h->DataTransferLength = (ULONG)size, h->DataBufferOffset = offset;
  h->TimeOutValue = (timeout_ms + 999) / 1000;
  memcpy(h->Cdb, cdb, cdb_size);
  if (size && dir == JM_SCSI_OUT) memcpy(packet + offset, data, size);
  int ok = DeviceIoControl(v->h, IOCTL_SCSI_PASS_THROUGH, packet, (DWORD)total, packet, (DWORD)total, &got, NULL) &&
           !h->ScsiStatus;
  if (ok && size && dir == JM_SCSI_IN) memcpy(data, packet + offset, size);
  free(packet);
  if (!ok) return -1;
  *done = size;
  return 0;
}

static void nap(void *ctx, unsigned ms) {
  (void)ctx;
  Sleep(ms);
}

void hooks_device(struct jm_hooks *h) {
  h->list = list, h->open = open_device, h->close = close_device;
  h->write = write_report, h->read = read_report, h->set_feature = set_feature;
  h->scsi = scsi, h->sleep = nap;
}
