#define _POSIX_C_SOURCE 200809L
#include "transport_jieli.h"
#include "jieli_image/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#ifndef _WIN32
static int chip_of(const uint8_t *vendor, enum jm_jieli_chip *chip) {
  if (!memcmp(vendor, "BR23", 4)) *chip = JM_JIELI_BR23;
  else if (!memcmp(vendor, "BD19", 4)) *chip = JM_JIELI_BD19;
  else return 0;
  return 1;
}
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
#include "platform.h"
/* Compatibility entry point; all upgrade policy and sequencing lives in core. */
int jm_jieli_upgrade(const char *path, const uint8_t *loader, size_t loader_size,
                       const uint8_t *image, size_t size) {
  struct jm_platform_connection *connection=jm_platform_open(path,JM_SESSION_JIELI);
  if (!connection) return -2;
  int result=jm_session_jieli(jm_platform_io(),connection,loader,loader_size,image,size);
  jm_platform_close(connection); return result;
}
