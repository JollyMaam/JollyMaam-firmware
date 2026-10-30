#include "transport_custom.h"
#include "transport_hid.h"
#include "platform.h"
#include <hidapi.h>

/* Read-only desktop discovery; callback arguments are borrowed during the call. */
int jm_hid_custom_devices(jm_custom_visitor visit,void *user) {
  if(!visit || hid_init()) return -1;
  struct hid_device_info *list=hid_enumerate(0x16c0,0x05df); int count=0;
  for(struct hid_device_info *p=list;p;p=p->next) {
    char path[1024]; if(!jm_hid_device_path(p->path,path)) continue;
    struct jm_platform_connection *c=jm_platform_open(path,JM_SESSION_CUSTOM);
    if(!c) continue;
    struct jm_identity id; int ok=jm_session_identify(jm_platform_io(),c,&id);
    jm_platform_close(c);
    if(ok) { ++count; if(visit(user,path,&id)) break; }
  }
  hid_free_enumeration(list); return count;
}
/* Compatibility wrapper over the shared core session. */
int jm_hid_custom_update(const char *path,const uint8_t uid[16],const uint8_t *image,size_t size,
                           void (*progress)(void *,size_t,size_t),void *user) {
  struct jm_platform_connection *c=jm_platform_open(path,JM_SESSION_CUSTOM);
  if(!c) return -2;
  int result=jm_session_custom(jm_platform_io(),c,uid,image,size,progress,user);
  jm_platform_close(c); return result;
}
