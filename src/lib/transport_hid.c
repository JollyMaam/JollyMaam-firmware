#include "transport_hid.h"
#include <hidapi.h>
#include <string.h>

int jm_hid_enumerate_8bitdo(jm_hid_visit visit, void *user) {
  if (!visit || hid_init())
    return -1;
  struct hid_device_info *list = hid_enumerate(0x2dc8, 0x3208);
  int count = 0;
  for (struct hid_device_info *p = list; p; p = p->next) {
    ++count;
    if (visit(user, p->path, p->vendor_id, p->product_id))
      break;
  }
  hid_free_enumeration(list);
  return count;
}
int jm_hid_upgrade_8bitdo(const char *path, const uint8_t *image, size_t size,
                          void (*progress)(void *, unsigned, unsigned), void *user) {
  struct jm_8bitdo_upgrade op;
  if (!jm_8bitdo_upgrade_init(&op, image, size))
    return JM_8BITDO_ARGUMENT;
  hid_device *dev = hid_open_path(path);
  if (!dev)
    return -1;
  int result = -1;
  uint8_t report[65], reply[128];
  int needs_reply;
  enum jm_8bitdo_step step;
  while ((step = jm_8bitdo_upgrade_next(&op, report + 1, &needs_reply)) == JM_8BITDO_TRANSFER) {
    report[0] = 0;
    /* Bounded drain: avoid sending while stale input remains queued. */
    unsigned drained = 0;
    int n;
    while ((n = hid_read_timeout(dev, reply, sizeof reply, 0)) > 0 && drained < 64)
      ++drained;
    if (n < 0 || drained == 64 || hid_write(dev, report, sizeof report) != sizeof report)
      goto out;
    if (!needs_reply) {
      jm_8bitdo_upgrade_complete(&op, 1, NULL, 0);
      continue;
    }
    /* Bound stale replies as well as each read. No destructive command retries. */
    unsigned stale = 0;
    enum jm_8bitdo_result r;
    do {
      n = hid_read_timeout(dev, reply, sizeof reply, 10000);
      if (n <= 0)
        goto out;
      r = jm_8bitdo_upgrade_complete(&op, 1, reply, (size_t)n);
    } while (r == JM_8BITDO_STALE && ++stale < 8);
    if (r != JM_8BITDO_OK) {
      result = r;
      goto out;
    }
    if (progress)
      progress(user, op.completed, op.sectors);
  }
  result = step == JM_8BITDO_DONE ? 0 : op.error ? op.error : -1;
out:
  hid_close(dev);
  return result;
}
