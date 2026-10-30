#define _POSIX_C_SOURCE 200809L
#include "transport_custom.h"
#include "transport_hid.h"
#include <hidapi.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#else
#include <time.h>
#endif
#include <errno.h>

struct custom { hid_device *dev; size_t report; uint8_t tag; };
static void pause_ms(unsigned ms) {
  #ifdef _WIN32
  Sleep(ms);
  #else
  struct timespec t = {ms/1000, (long)(ms%1000)*1000000};
  while (nanosleep(&t, &t) && errno == EINTR) {}
  #endif
}
static int open_custom(struct custom *c, const char *path) {
  memset(c, 0, sizeof *c); c->dev = hid_open_path(path);
  if (!c->dev) return 0;
  #ifdef _WIN32
  HANDLE handle=CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  PHIDP_PREPARSED_DATA preparsed=NULL;
  HIDP_CAPS caps;
  int ok=handle!=INVALID_HANDLE_VALUE && HidD_GetPreparsedData(handle,&preparsed);
  if (ok) {
    ok=HidP_GetCaps(preparsed,&caps)==HIDP_STATUS_SUCCESS && caps.UsagePage==0xffc2 &&
       (caps.FeatureReportByteLength==357 || caps.FeatureReportByteLength==513);
    if (ok) c->report=caps.FeatureReportByteLength-1;
    HidD_FreePreparsedData(preparsed);
  }
  if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
  if (ok) return 1;
  #else
  uint8_t descriptor[256];
  int n = hid_get_report_descriptor(c->dev, descriptor, sizeof descriptor);
  /* Accept only our known unnumbered byte-array feature descriptor. */
  if (n >= 3 && descriptor[0] == 6 && descriptor[1] == 0xc2 && descriptor[2] == 0xff) {
    for (int i=3; i+8<n; ++i) {
      if (descriptor[i]==0x75 && descriptor[i+1]==8 && descriptor[i+2]==0x96 &&
          descriptor[i+5]==9 && descriptor[i+6]==1 && descriptor[i+7]==0xb1) {
        c->report = descriptor[i+3] | (size_t)descriptor[i+4]<<8;
        if (c->report == 356 || c->report == 512) return 1;
      }
    }
  }
  #endif
  hid_close(c->dev); c->dev = NULL; return 0;
}
static int command(struct custom *c, const struct jm_update_request *r, uint8_t *answer) {
  uint8_t buffer[513] = {0};
  if (r->out_size > c->report-20 || r->in_size > 512) return 0;
  memcpy(buffer+1,r->cdb,16);
  size_t n = r->out_size ? r->out_size : r->in_size;
  buffer[17]=(uint8_t)n; buffer[18]=(uint8_t)(n>>8);
  buffer[19]=r->out_size ? 1 : 0; buffer[20]=++c->tag;
  if (r->out_size) memcpy(buffer+21,r->data,r->out_size);
  int sent=hid_send_feature_report(c->dev,buffer,c->report+1);
  /* COMMIT has already been checked before REBOOT. A disconnect racing the
   * reboot status does not invalidate the verified firmware; do not retry. */
  if (r->cdb[1]==0x10) return 1;
  if (sent != (int)c->report+1) return 0;
  if (r->cdb[1]==0x40) return 1;
  size_t got=0;
  for (unsigned polls=0;polls<10000;++polls) {
    buffer[0]=0;
    int count=hid_get_feature_report(c->dev,buffer,c->report+1);
    if (count<5 || buffer[1]!=c->tag) return 0;
    if (buffer[2]==1) { pause_ms(1); continue; }
    if (buffer[2]!=2 && buffer[2]!=3) return 0;
    size_t length=buffer[3] | (size_t)buffer[4]<<8;
    if (length>(size_t)count-5 || length>r->in_size-got) return 0;
    if (length) memcpy(answer+got,buffer+5,length);
    got+=length;
    if (buffer[2]==3) return got==r->in_size;
  }
  return 0;
}
static int identity(struct custom *c, struct jm_identity *id) {
  uint8_t reply[88]; struct jm_update_request r;
  memset(&r,0,sizeof r); jm_command(r.cdb,0,0); r.in_size=88;
  return command(c,&r,reply) && jm_identify_decode(reply,sizeof reply,id);
}
/* Enumerate our HID tool interfaces and read IDENTIFY, without changing mode
 * or persistent state. The callback borrows path and identity only during its
 * call. Unknown descriptors or inaccessible interfaces are skipped. */
int jm_hid_custom_devices(jm_custom_visitor visit, void *user) {
  if (!visit || hid_init()) return -1;
  struct hid_device_info *list=hid_enumerate(0x16c0,0x05df);
  int count=0;
  for (struct hid_device_info *p=list;p;p=p->next) {
    struct custom c; struct jm_identity id; char path[1024];
    if (!jm_hid_device_path(p->path,path) || !open_custom(&c,path)) continue;
    int ok=identity(&c,&id); hid_close(c.dev);
    if (ok) { ++count; if (visit(user,path,&id)) break; }
  }
  hid_free_enumeration(list); return count;
}
static int reopen_uid(struct custom *c, const uint8_t uid[16]) {
  for (unsigned attempt=0;attempt<30;++attempt) {
    pause_ms(500);
    struct hid_device_info *list=hid_enumerate(0x16c0,0x05df);
    int found=0;
    for (struct hid_device_info *p=list;p;p=p->next) {
      struct jm_identity id; char path[1024];
      if (!jm_hid_device_path(p->path,path) || !open_custom(c,path)) continue;
      if (identity(c,&id) && !memcmp(uid,id.uid,16) && id.updater==1) { found=1; break; }
      hid_close(c->dev); c->dev=NULL;
    }
    hid_free_enumeration(list);
    if (found) return 1;
  }
  return 0;
}
/* Update an explicitly selected custom-firmware device. Validate the image and
 * product first; enter its updater if necessary and reconnect by flash UID,
 * never by an arbitrary newly enumerated path. Caller must authorize updating.
 * Returns zero on success, negative for transport/identity errors, or the
 * updater's error. Progress counts bytes acknowledged through UPDATE_INFO. */
int jm_hid_custom_update(const char *path, const uint8_t expected_uid[16], const uint8_t *image, size_t size,
                           void (*progress)(void *, size_t, size_t), void *user) {
  struct custom c; struct jm_identity id;
  if (!expected_uid || !jm_update_image_check(image,size,NULL)) return -1;
  if (!open_custom(&c,path)) return -2;
  int result=-3;
  if (!identity(&c,&id) || memcmp(id.uid,expected_uid,16) || memcmp(id.product,image+24,16)) goto done;
  if (!id.updater) {
    struct jm_update_request r; memset(&r,0,sizeof r); jm_command(r.cdb,0x40,0);
    /* Reset can beat control-transfer completion, so reconnect by UID even
     * when the final USB status was lost. No data writes have happened yet. */
    command(&c,&r,NULL); hid_close(c.dev); c.dev=NULL;
    if (!reopen_uid(&c,id.uid)) return -4;
  }
  struct jm_update op;
  if (!jm_update_init(&op,image,size,c.report-20)) goto done;
  struct jm_update_request request; uint8_t reply[512]; enum jm_update_step step;
  while ((step=jm_update_next(&op,&request))==JM_UPDATE_TRANSFER) {
    int ok=command(&c,&request,reply);
    jm_update_complete(&op,ok,reply,request.in_size);
    if (progress) progress(user,op.offset,op.size);
  }
  result=step==JM_UPDATE_DONE ? 0 : op.error ? op.error : -5;
done:
  if (c.dev) hid_close(c.dev);
  return result;
}
