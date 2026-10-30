#include "transport_hid.h"
#include "platform.h"
#include <hidapi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Optional hidapi desktop backend. Enumeration does not open or modify devices.
 * Path is borrowed for the duration of visit. Return nonzero to stop visiting. */
int jm_hid_enumerate_8bitdo(jm_hid_visit visit, void *user) {
  if (!visit || hid_init())
    return -1;
  struct hid_device_info *list = hid_enumerate(0x2dc8, 0x3208);
  int count = 0;
  for (struct hid_device_info *p = list; p; p = p->next) {
    char path[1024];
    if (!jm_hid_device_path(p->path,path)) continue;
    ++count;
    if (visit(user, path, p->vendor_id, p->product_id))
      break;
  }
  hid_free_enumeration(list);
  return count;
}
/* Blocking desktop convenience driver. Path must be explicitly selected.
 * Returns zero on success, negative on transport failure, positive on protocol
 * failure. Image is validated before opening the device. Progress may be NULL.
 * Caller must obtain user authorization before invoking this function. */
struct progress_adapter { void (*callback)(void *,unsigned,unsigned); void *user; };
static void progress_bridge(void *user,size_t done,size_t total) {
  struct progress_adapter *p=user; if(p->callback) p->callback(p->user,(unsigned)done,(unsigned)total);
}
int jm_hid_upgrade_8bitdo(const char *path, const uint8_t *image, size_t size,
                          void (*progress)(void *,unsigned,unsigned), void *user) {
  struct jm_platform_connection *c=jm_platform_open(path,JM_SESSION_8BITDO);
  if(!c) return -1;
  struct progress_adapter p={progress,user};
  int result=jm_session_8bitdo(jm_platform_io(),c,image,size,progress_bridge,&p);
  jm_platform_close(c); return result;
}

/* Read-only bootloader model probe. Output is untouched on failure. */
int jm_hid_8bitdo_model(const char *path, uint16_t *model) {
  hid_device *dev=hid_open_path(path);
  if (!dev) return 0;
  uint8_t report[65]={0}, reply[128];
  jm_8bitdo_request(report+1,8,0,NULL,4);
  int ok=0;
  if (hid_write(dev,report,sizeof report)==sizeof report) {
    int n=hid_read_timeout(dev,reply,sizeof reply,1000);
    const uint8_t *data; size_t size;
    if (n>0 && jm_8bitdo_reply(reply,(size_t)n,8,0,&data,&size)==JM_8BITDO_OK && size==2) {
      *model=(uint16_t)(data[0] | data[1]<<8); ok=1;
    }
  }
  hid_close(dev); return ok;
}

/* Map hidapi's Linux device path into the application's exported node root.
 * Windows and other platforms preserve the opaque path. Output is caller-owned. */
int jm_hid_device_path(const char *path, char out[1024]) {
#ifdef __linux__
  const char *root=getenv("DEV_ROOT");
  if (root) {
    const char *base=strrchr(path,'/');
    return snprintf(out,1024,"%s/%s",root,base?base+1:path)<1024;
  }
#endif
  return snprintf(out,1024,"%s",path)<1024;
}
