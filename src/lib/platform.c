#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "platform_scsi.h"
#include "transport_hid.h"
#include <hidapi.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
typedef HANDLE native_handle;
#define INVALID_NATIVE INVALID_HANDLE_VALUE
#else
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
typedef int native_handle;
#define INVALID_NATIVE (-1)
#endif
struct jm_platform_connection { hid_device *dev; size_t report; native_handle disk; enum jm_session_kind kind; };
static int open_custom(struct jm_platform_connection *c, const char *path) {
  memset(c, 0, sizeof *c); c->dev = hid_open_path(path);
  if (!c->dev) return 0;
  #ifdef _WIN32
  HANDLE handle=CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  PHIDP_PREPARSED_DATA preparsed=NULL;
  HIDP_CAPS caps;
  int ok=handle!=INVALID_HANDLE_VALUE && HidD_GetPreparsedData(handle,&preparsed);
  if (ok) {
    /* Our tool device: a feature report for commands, 64-byte input
     * reports for the answers. */
    ok=HidP_GetCaps(preparsed,&caps)==HIDP_STATUS_SUCCESS && caps.UsagePage==0xffc2 &&
       caps.FeatureReportByteLength>=65 && caps.FeatureReportByteLength<=513 &&
       caps.InputReportByteLength==65;
    if (ok) c->report=caps.FeatureReportByteLength-1;
    HidD_FreePreparsedData(preparsed);
  }
  if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
  if (ok) return 1;
  #else
  uint8_t descriptor[256];
  int n = hid_get_report_descriptor(c->dev, descriptor, sizeof descriptor);
  /* Accept only our unnumbered byte-array feature report (the commands)
   * followed by the 64-byte input report (the answers). */
  if (n >= 3 && descriptor[0] == 6 && descriptor[1] == 0xc2 && descriptor[2] == 0xff) {
    for (int i=3; i+13<n; ++i) {
      if (descriptor[i]==0x75 && descriptor[i+1]==8 && descriptor[i+2]==0x96 &&
          descriptor[i+5]==9 && descriptor[i+6]==1 && descriptor[i+7]==0xb1 &&
          descriptor[i+9]==0x95 && descriptor[i+10]==64 && descriptor[i+13]==0x81) {
        c->report = descriptor[i+3] | (size_t)descriptor[i+4]<<8;
        if (c->report >= 64 && c->report <= 512) return 1;
      }
    }
  }
  #endif
  hid_close(c->dev); c->dev = NULL; return 0;
}
/* Open a desktop endpoint; permission acquisition remains the application's job.
 * The returned object owns its OS handle until close, including reconnects. */
struct jm_platform_connection *jm_platform_open(const char *path, enum jm_session_kind kind) {
  if(!path || kind<JM_SESSION_CUSTOM || kind>JM_SESSION_JIELI) return NULL;
  struct jm_platform_connection *c=calloc(1,sizeof *c); if(!c) return NULL;
  c->disk=INVALID_NATIVE; c->kind=kind;
  if(kind==JM_SESSION_JIELI) {
#ifdef _WIN32
    c->disk=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
#else
    c->disk=open(path,O_RDWR|O_CLOEXEC);
#endif
    if(c->disk==INVALID_NATIVE) { free(c);return NULL; }
  } else if(kind==JM_SESSION_CUSTOM) {
    if(!open_custom(c,path)) { free(c);return NULL; }
    c->disk=INVALID_NATIVE; c->kind=kind;
  } else if(!(c->dev=hid_open_path(path))) { free(c);return NULL; }
  return c;
}
/* Release the desktop connection. No firmware or persistent state is changed. */
void jm_platform_close(struct jm_platform_connection *c) {
  if(!c) return;
  if(c->dev) hid_close(c->dev);
  if(c->disk!=INVALID_NATIVE) {
#ifdef _WIN32
    CloseHandle(c->disk);
#else
    close(c->disk);
#endif
  }
  free(c);
}
static int scsi(void *user,const uint8_t *cdb,size_t n,enum jm_scsi_direction direction,
                 uint8_t *data,size_t size,unsigned timeout,struct jm_scsi_result *result) {
  struct jm_platform_connection *c=user;
#ifdef _WIN32
  return jm_scsi_win32_transfer(c->disk,cdb,n,direction,data,size,timeout,result);
#else
  return jm_scsi_linux_transfer(c->disk,cdb,n,direction,data,size,timeout,result);
#endif
}
static int write_report(void *user,const uint8_t *data,size_t size) {
  struct jm_platform_connection *c=user; uint8_t report[65]={0};
  if(size!=64) return -1;
  memcpy(report+1,data,size); int n=hid_write(c->dev,report,65); return n==65?64:-1;
}
static int read_report(void *user,uint8_t *data,size_t size,unsigned timeout) {
  struct jm_platform_connection *c=user; return hid_read_timeout(c->dev,data,size,(int)timeout);
}
static int feature_set(void *user,const uint8_t *data,size_t size) {
  struct jm_platform_connection *c=user; return hid_send_feature_report(c->dev,data,size);
}
/* The tool device's answers are addressed by client: this process's. */
static uint32_t client(void *user) {
  (void)user;
  static uint32_t value;
  if (!value) {
#ifdef _WIN32
    value=(uint32_t)GetCurrentProcessId()*0x9e3779b9u ^ (uint32_t)GetTickCount();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    value=(uint32_t)getpid()*0x9e3779b9u ^ (uint32_t)t.tv_nsec;
#endif
    if (!value) value=1;
  }
  return value;
}
static size_t feature_size(void *user) { return ((struct jm_platform_connection *)user)->report; }
static void wait_ms(void *user,unsigned ms) {
  (void)user;
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec t={ms/1000,(long)(ms%1000)*1000000};
  while(nanosleep(&t,&t) && errno==EINTR) {}
#endif
}
static int reconnect(void *user,const uint8_t uid[16]) {
  struct jm_platform_connection *c=user;
  if(c->dev) { hid_close(c->dev); c->dev=NULL; }
  {
    struct hid_device_info *list=hid_enumerate(0x16c0,0x05df);
    for(struct hid_device_info *p=list;p;p=p->next) {
      char path[1024];
      if(!jm_hid_device_path(p->path,path)) continue;
      struct jm_platform_connection *candidate=jm_platform_open(path,JM_SESSION_CUSTOM);
      if(!candidate) continue;
      struct jm_identity id;
      if(jm_session_identify(jm_platform_io(),candidate,&id) && !memcmp(uid,id.uid,16)) {
        *c=*candidate; free(candidate); hid_free_enumeration(list); return 1;
      }
      jm_platform_close(candidate);
    }
    hid_free_enumeration(list);
  }
  return 0;
}
/* Desktop implementation of the core callback contract. No protocol update
 * sequencing is implemented here. Applications may supply another table. */
const struct jm_session_io *jm_platform_io(void) {
  static const struct jm_session_io io={scsi,write_report,read_report,feature_set,wait_ms,reconnect,feature_size,client};
  return &io;
}
