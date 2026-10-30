#define _POSIX_C_SOURCE 200809L
#include "transport_jieli.h"
#include "jieli_image/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef HANDLE device_handle;
#define INVALID_DEVICE INVALID_HANDLE_VALUE
#define close_device CloseHandle
#define scsi_transfer jm_scsi_win32_transfer
static device_handle open_device(const char *path) {
  return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
}
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef int device_handle;
#define INVALID_DEVICE (-1)
#define close_device close
#define scsi_transfer jm_scsi_linux_transfer
static device_handle open_device(const char *path) { return open(path, O_RDWR | O_CLOEXEC); }
#endif

static int chip_of(const uint8_t *vendor, enum jm_jieli_chip *chip) {
  if (!memcmp(vendor, "BR23", 4)) *chip = JM_JIELI_BR23;
  else if (!memcmp(vendor, "BD19", 4)) *chip = JM_JIELI_BD19;
  else return 0;
  return 1;
}
#ifndef _WIN32
static int read_text(const char *path, uint8_t *data, size_t size) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  size_t n = fread(data, 1, size, f); fclose(f); return n == size;
}
/* Enumerate Jieli UBOOT block-device candidates through sysfs metadata only.
 * No node is opened, RAM loader uploaded or device rebooted. Paths are borrowed
 * during the callback. Honor DEV_ROOT for container-exported device nodes.
 * A nonzero callback result stops enumeration. Return count, or -1 on error. */
int jm_jieli_candidates(jm_jieli_candidate_visitor visit, void *user) {
  if (!visit) return -1;
  DIR *dir = opendir("/sys/class/block");
  if (!dir) return -1;
  const char *root = getenv("DEV_ROOT"); if (!root) root = "/dev";
  struct dirent *entry; int count = 0;
  while ((entry = readdir(dir))) {
    if (strncmp(entry->d_name, "sd", 2)) continue;
    char path[1024]; uint8_t vendor[4], model[9]; enum jm_jieli_chip chip;
    if (snprintf(path, sizeof path, "/sys/class/block/%s/device/vendor", entry->d_name) >= (int)sizeof path ||
        !read_text(path, vendor, sizeof vendor) || !chip_of(vendor, &chip)) continue;
    snprintf(path, sizeof path, "/sys/class/block/%s/device/model", entry->d_name);
    if (!read_text(path, model, sizeof model) || memcmp(model, "UBOOT1.00", 9)) continue;
    if (snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= (int)sizeof path) continue;
    ++count;
    if (visit(user, path, chip)) break;
  }
  closedir(dir); return count;
}
#endif
static int transfer(device_handle fd, const struct jm_jieli_request *request, uint8_t *reply, size_t *size) {
  #ifdef _WIN32
  Sleep(request->delay_ms);
  #else
  struct timespec delay = {request->delay_ms / 1000, (long)(request->delay_ms % 1000) * 1000000};
  while (nanosleep(&delay, &delay) && errno == EINTR) {}
  #endif
  struct jm_scsi_result result;
  uint8_t *data = request->direction == JM_SCSI_OUT ? (uint8_t *)request->data : reply;
  if (scsi_transfer(fd, request->cdb, 16, request->direction, data,
                              request->size, request->timeout_ms, &result) ||
      result.status || result.host_status || result.driver_status) return 0;
  *size = result.transferred; return 1;
}
static int flash_read(device_handle fd, uint32_t address, uint8_t *data, size_t size, uint32_t io) {
  while (size) {
    struct jm_jieli_request request;
    memset(&request, 0, sizeof request);
    request.direction = JM_SCSI_IN; request.timeout_ms = 20000;
    request.size = size < io ? size : io;
    if (!jm_jieli_read_cdb(request.cdb, 0xfd05, address, request.size)) return 0;
    size_t n = 0;
    if (!transfer(fd, &request, data, &n) || n != request.size) return 0;
    size -= n; data += n; address += (uint32_t)n;
  }
  return 1;
}
/* Blocking desktop convenience driver for an explicitly selected stock loader.
 * Validate the image before opening the device, verify INQUIRY, upload the
 * supplied chip-specific RAM loader, read the installed product, rekey locally,
 * and drive the portable flash operation. Nova pad stock data is preserved.
 * The caller must authorize flashing. Zero means verified; firmware remains
 * in the RAM loader. Starting it is deliberately separate. Negative results
 * identify preflight (-1), opening (-2), INQUIRY (-3), loader (-4), installed
 * product (-5), image preparation (-6), or flash/verification (-7) failure.
 * No automatic boot entry, device selection or reconnect occurs. */
int jm_jieli_upgrade(const char *path, const uint8_t *loader, size_t loader_size,
                             const uint8_t *image, size_t size) {
  if (!path || !loader || !loader_size || !size || size > 32 * 1024 * 1024) return -1;
  uint8_t *workspace = malloc(size), *packed = malloc(size), *head = NULL;
  struct jm_jieli_image view;
  int result = -1; device_handle fd = INVALID_DEVICE;
  if (!workspace || !packed || jm_jieli_image_open(&view, image, size, workspace, size)) goto done;
  fd = open_device(path);
  if (fd == INVALID_DEVICE) { result = -2; goto done; }
  uint8_t inquiry[36], cdb[6] = {0x12, 0, 0, 0, 36, 0}; struct jm_scsi_result scsi;
  enum jm_jieli_chip chip;
  if (scsi_transfer(fd, cdb, 6, JM_SCSI_IN, inquiry, sizeof inquiry, 2000, &scsi) ||
      scsi.status || scsi.host_status || scsi.driver_status || scsi.transferred != sizeof inquiry ||
      memcmp(inquiry + 16, "UBOOT1.00", 9) || !chip_of(inquiry + 8, &chip)) { result = -3; goto done; }
  struct jm_jieli_loader startup;
  if (!jm_jieli_loader_init(&startup, chip, loader, loader_size)) { result = -4; goto done; }
  struct jm_jieli_request request; uint8_t reply[32768]; enum jm_jieli_step step;
  while ((step = jm_jieli_loader_next(&startup, &request)) == JM_JIELI_TRANSFER) {
    size_t n = 0; int ok = transfer(fd, &request, reply, &n);
    jm_jieli_loader_complete(&startup, ok, reply, n);
  }
  if (step != JM_JIELI_DONE) { result = -4; goto done; }
  size_t head_size = startup.device.flash_size < 0x11000 ? startup.device.flash_size : 0x11000;
  head = malloc(head_size); uint8_t header[32]; uint32_t base;
  if (!head || !flash_read(fd, 0, head, head_size, startup.device.io) ||
      !jm_jieli_find_base(head, head_size, header, &base)) { result = -5; goto done; }
  char product[17], wanted[17];
  jm_jieli_name_string(product, head + base + 16);
  jm_jieli_name_string(wanted, image + view.base + 16);
  if (strcmp(product, wanted)) { result = -5; goto done; }
  if (jm_jieli_image_pack(&view, startup.device.key, packed, size)) { result = -6; goto done; }
  struct jm_jieli_keep keep = {0x64000, 0x1c000};
  size_t keep_count = !strcmp(product, "AC695X_TRANS") ? 1 : 0;
  size_t end = size;
  if (keep_count && end < 0x80000) end = 0x80000;
  if (end > startup.device.flash_size) { result = -6; goto done; }
  end = (end + 4095) & ~(size_t)4095;
  uint8_t *grown = realloc(workspace, end);
  if (!grown) { result = -6; goto done; } workspace = grown;
  struct jm_jieli_flash flash;
  if (!jm_jieli_flash_init(&flash, &startup.device, product, packed, size, workspace, end,
                            &keep, keep_count)) { result = -6; goto done; }
  while ((step = jm_jieli_flash_next(&flash, &request)) == JM_JIELI_TRANSFER) {
    size_t n = 0; int ok = transfer(fd, &request, reply, &n);
    jm_jieli_flash_complete(&flash, ok, reply, n);
  }
  result = step == JM_JIELI_DONE ? 0 : -7;
done:
  if (fd != INVALID_DEVICE) close_device(fd);
  free(workspace); free(packed); free(head); return result;
}
