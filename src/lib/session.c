#include "session.h"
#include "upgrade_jieli.h"
#include "upgrade_8bitdo.h"
#include "upgrade_jollymaam.h"
#include "jieli_image/image.h"
#include <stdlib.h>
#include <string.h>
/* Session callback contract: context belongs to the application/adapter and
 * remains alive through run. SCSI returns zero on completed I/O with status and
 * actual length in result, negative on transport failure. HID/feature callbacks
 * return actual byte counts, zero for read timeout, negative for errors.
 * hid_write receives 64 protocol bytes without hidapi's synthetic report ID;
 * feature buffers include the leading report ID byte (zero for our device).
 * Android adapters translate that byte to USB control-transfer report metadata.
 * wait_ms schedules a delay; reconnect attempts one discovery/open pass for the
 * requested UID, replacing the adapter's connection. Core rechecks identity and
 * owns retry/delay policy. feature_size excludes the report ID byte.
 * Only callbacks needed by a protocol must be present. These callbacks run on
 * the caller's thread; JNI applications should use a worker, not the UI thread.
 */
static int chip_of(const uint8_t *vendor, enum jm_jieli_chip *chip) {
  if (!memcmp(vendor,"BR23",4)) *chip=JM_JIELI_BR23;
  else if (!memcmp(vendor,"BD19",4)) *chip=JM_JIELI_BD19;
  else return 0;
  return 1;
}
static int transfer(const struct jm_session_io *io, void *context,
                     const struct jm_jieli_request *request, uint8_t *reply, size_t *size) {
  io->wait_ms(context,request->delay_ms);
  struct jm_scsi_result result;
  uint8_t *data=request->direction==JM_SCSI_OUT?(uint8_t *)request->data:reply;
  if (io->scsi(context,request->cdb,16,request->direction,data,request->size,request->timeout_ms,&result) ||
      result.status || result.host_status || result.driver_status) return 0;
  *size=result.transferred; return 1;
}
static int flash_read(const struct jm_session_io *io, void *context, uint32_t address, uint8_t *data, size_t size, uint32_t limit) {
  while (size) {
    struct jm_jieli_request request;
    memset(&request, 0, sizeof request);
    request.direction = JM_SCSI_IN; request.timeout_ms = 20000;
    request.size = size < limit ? size : limit;
    if (!jm_jieli_read_cdb(request.cdb, 0xfd05, address, request.size)) return 0;
    size_t n = 0;
    if (!transfer(io,context, &request, data, &n) || n != request.size) return 0;
    size -= n; data += n; address += (uint32_t)n;
  }
  return 1;
}
/* Verify UBOOT1.00 and start its matching RAM helper on a caller-selected
 * connection. No flash is modified. device is untouched on failure. Return
 * zero on success, -1 for arguments, -3 for INQUIRY, -4 for helper startup.
 * Keep the same connection for subsequent reads, backup, flash or start. */
int jm_session_jieli_open(const struct jm_session_io *io, void *context, const uint8_t *loader,
                            size_t loader_size, struct jm_jieli_device *device) {
  if(!io || !io->scsi || !io->wait_ms || !loader || !loader_size || !device) return -1;
  uint8_t inquiry[36], cdb[6] = {0x12, 0, 0, 0, 36, 0}; struct jm_scsi_result scsi;
  enum jm_jieli_chip chip;
  if (io->scsi(context, cdb, 6, JM_SCSI_IN, inquiry, sizeof inquiry, 2000, &scsi) ||
      scsi.status || scsi.host_status || scsi.driver_status || scsi.transferred != sizeof inquiry ||
      memcmp(inquiry + 16, "UBOOT1.00", 9) || !chip_of(inquiry + 8, &chip)) return -3;
  struct jm_jieli_loader startup;
  if (!jm_jieli_loader_init(&startup, chip, loader, loader_size)) return -4;
  struct jm_jieli_request request; uint8_t reply[32768]; enum jm_jieli_step step;
  while ((step = jm_jieli_loader_next(&startup, &request)) == JM_JIELI_TRANSFER) {
    size_t n = 0; int ok = transfer(io,context, &request, reply, &n);
    jm_jieli_loader_complete(&startup, ok, reply, n);
  }
  if (step != JM_JIELI_DONE) return -4;
  *device=startup.device;return 0;
}
/* Read a bounded flash range through an already-running RAM helper. Return one
 * on success, zero on argument, transport or short-read failure. The buffer may
 * contain a partial read on failure. Never retries an ambiguous command. */
int jm_session_jieli_read(const struct jm_session_io *io, void *context,
                            const struct jm_jieli_device *device, uint32_t address,uint8_t *data,size_t size) {
  if(!io || !io->scsi || !io->wait_ms || !device || (!data && size) ||
     device->io<64 || device->io>32768 || address>device->flash_size || size>device->flash_size-address) return 0;
  return flash_read(io,context,address,data,size,device->io);
}
/* Stream a complete raw flash backup to a caller-owned sink. Data is borrowed
 * only during each callback; return nonzero from sink after accepting a block.
 * No flash writes or restart occur. Return zero on success, -1 for invalid
 * arguments, -2 for read failure, -3 for rejected output. Progress counts bytes. */
int jm_session_jieli_backup(const struct jm_session_io *io,void *context,
                              const struct jm_jieli_device *device,jm_session_sink sink,void *user,
                              jm_session_progress progress,void *progress_user) {
  if(!io || !io->scsi || !io->wait_ms || !device || !device->flash_size ||
     device->flash_size>64*1024*1024 || device->io<64 || device->io>32768 || !sink) return -1;
  uint8_t data[32768];uint32_t at=0;
  while(at<device->flash_size) {
    size_t n=device->flash_size-at;if(n>sizeof data) n=sizeof data;
    if(!jm_session_jieli_read(io,context,device,at,data,n)) return -2;
    if(!sink(user,at,data,n)) return -3;
    at+=(uint32_t)n;if(progress) progress(progress_user,at,device->flash_size);
  }
  return 0;
}
/* Request a flash boot from the RAM helper. The device may disconnect before
 * replying, so send once and return the transport result without retrying.
 * A successful transfer does not establish that the firmware subsequently boots. */
int jm_session_jieli_start(const struct jm_session_io *io,void *context) {
  if(!io || !io->scsi) return 0;
  uint8_t args[4]={0,0,0,1},cdb[16],reply[16];struct jm_scsi_result result;
  jm_jieli_cdb(cdb,0xfc0c,args,sizeof args);
  return !io->scsi(context,cdb,sizeof cdb,JM_SCSI_IN,reply,sizeof reply,2000,&result) &&
    !result.status && !result.host_status && !result.driver_status;
}

/* Portable blocking Jieli session for an explicitly selected stock loader.
 * Validate the image on a caller-opened connection, verify INQUIRY, upload the
 * supplied chip-specific RAM loader, read the installed product, rekey locally,
 * and drive the portable flash operation. Nova pad stock data is preserved.
 * The caller must authorize flashing. Zero means verified; firmware remains
 * in the RAM loader. Starting it is deliberately separate. Negative results
 * identify preflight (-1), INQUIRY (-3), loader (-4), installed
 * product (-5), image preparation (-6), or flash/verification (-7) failure.
 * No automatic boot entry, device selection or reconnect occurs. */
int jm_session_jieli(const struct jm_session_io *io, void *context, const uint8_t *loader, size_t loader_size,
                             const uint8_t *image, size_t size) {
  if (!io || !io->scsi || !io->wait_ms || !loader || !loader_size || !image || !size || size > 32 * 1024 * 1024) return -1;
  uint8_t *workspace = malloc(size), *packed = malloc(size), *head = NULL;
  struct jm_jieli_image view;
  int result = -1;
  if (!workspace || !packed || jm_jieli_image_open(&view, image, size, workspace, size)) goto done;
  struct jm_jieli_device device;
  result=jm_session_jieli_open(io,context,loader,loader_size,&device);
  if(result) goto done;
  struct jm_jieli_request request;uint8_t reply[32768];enum jm_jieli_step step;
  size_t head_size = device.flash_size < 0x11000 ? device.flash_size : 0x11000;
  head = malloc(head_size); uint8_t header[32]; uint32_t base;
  if (!head || !flash_read(io,context, 0, head, head_size, device.io) ||
      !jm_jieli_find_base(head, head_size, header, &base)) { result = -5; goto done; }
  char product[17], wanted[17];
  jm_jieli_name_string(product, head + base + 16);
  jm_jieli_name_string(wanted, image + view.base + 16);
  if (strcmp(product, wanted)) { result = -5; goto done; }
  if (jm_jieli_image_pack(&view, device.key, packed, size)) { result = -6; goto done; }
  struct jm_jieli_keep keep = {0x64000, 0x1c000};
  size_t keep_count = !strcmp(product, "AC695X_TRANS") ? 1 : 0;
  size_t end = size;
  if (keep_count && end < 0x80000) end = 0x80000;
  if (end > device.flash_size) { result = -6; goto done; }
  end = (end + 4095) & ~(size_t)4095;
  uint8_t *grown = realloc(workspace, end);
  if (!grown) { result = -6; goto done; } workspace = grown;
  struct jm_jieli_flash flash;
  if (!jm_jieli_flash_init(&flash, &device, product, packed, size, workspace, end,
                            &keep, keep_count)) { result = -6; goto done; }
  while ((step = jm_jieli_flash_next(&flash, &request)) == JM_JIELI_TRANSFER) {
    size_t n = 0; int ok = transfer(io,context, &request, reply, &n);
    jm_jieli_flash_complete(&flash, ok, reply, n);
  }
  result = step == JM_JIELI_DONE ? 0 : -7;
done:
  free(workspace); free(packed); free(head); return result;
}

static int command(const struct jm_session_io *io, void *context, uint8_t *tag, const struct jm_update_request *r, uint8_t *answer) {
  size_t report=io->feature_size(context);
  if (report!=356 && report!=512) return 0;
  uint8_t buffer[513] = {0};
  if (r->out_size > report-20 || r->in_size > 512) return 0;
  memcpy(buffer+1,r->cdb,16);
  size_t n = r->out_size ? r->out_size : r->in_size;
  buffer[17]=(uint8_t)n; buffer[18]=(uint8_t)(n>>8);
  buffer[19]=r->out_size ? 1 : 0; buffer[20]=++*tag;
  if (r->out_size) memcpy(buffer+21,r->data,r->out_size);
  int sent=io->feature_set(context,buffer,report+1);
  /* COMMIT has already been checked before REBOOT. A disconnect racing the
   * reboot status does not invalidate the verified firmware; do not retry. */
  if (r->cdb[1]==0x10) return 1;
  if (sent != (int)report+1) return 0;
  if (r->cdb[1]==0x40) return 1;
  if (r->cdb[1]==0x11) return 1;
  size_t got=0;
  for (unsigned polls=0;polls<10000;++polls) {
    buffer[0]=0;
    int count=io->feature_get(context,buffer,report+1);
    if (count<5 || buffer[1]!=*tag) return 0;
    if (buffer[2]==1) { io->wait_ms(context,1); continue; }
    if (buffer[2]!=2 && buffer[2]!=3) return 0;
    size_t length=buffer[3] | (size_t)buffer[4]<<8;
    if (length>(size_t)count-5 || length>r->in_size-got) return 0;
    if (length) memcpy(answer+got,buffer+5,length);
    got+=length;
    if (buffer[2]==3) return got==r->in_size;
  }
  return 0;
}
/* Read IDENTIFY over the portable feature-report framing. No OS API calls. */
int jm_session_identify(const struct jm_session_io *io, void *context, struct jm_identity *id) {
  if (!io || !io->feature_size || !io->feature_set || !io->feature_get || !io->wait_ms) return 0;
  uint8_t reply[88], tag=0; struct jm_update_request r;
  memset(&r,0,sizeof r); jm_command(r.cdb,0,0); r.in_size=88;
  return command(io,context,&tag,&r,reply) && jm_identify_decode(reply,sizeof reply,id);
}
/* Request stock bootloader entry from our app or updater after checking the
 * previously selected UID. No flash is written. Return zero when the complete
 * feature request was sent, -1 for arguments, -2 for identity mismatch, -3 for
 * an unconfirmed transfer. A disconnect can race USB completion: rescan after
 * either result, never automatically resend. No bootloader discovery occurs. */
int jm_session_stock_loader(const struct jm_session_io *io,void *context,const uint8_t uid[16]) {
  if(!uid || !io || !io->feature_size || !io->feature_set || !io->feature_get || !io->wait_ms) return -1;
  struct jm_identity id;
  if(!jm_session_identify(io,context,&id) || memcmp(id.uid,uid,16)) return -2;
  struct jm_update_request request;memset(&request,0,sizeof request);jm_command(request.cdb,0x11,0);
  uint8_t tag=0;
  return command(io,context,&tag,&request,NULL)?0:-3;
}
/* Run a custom update with shared identity, framing and reconnect policy.
 * Callbacks may block; none may retain supplied buffers after returning. */
int jm_session_custom(const struct jm_session_io *io, void *context, const uint8_t expected_uid[16], const uint8_t *image, size_t size,
                           void (*progress)(void *, size_t, size_t), void *user) {
  struct jm_identity id; uint8_t tag=0;
  if (!expected_uid || !jm_update_image_check(image,size,NULL)) return -1;
  if (!io || !io->feature_size || !io->feature_set || !io->feature_get || !io->wait_ms || !io->reconnect) return -2;
  int result=-3;
  if (!jm_session_identify(io,context,&id) || memcmp(id.uid,expected_uid,16) || memcmp(id.product,image+24,16)) goto done;
  if (!id.updater) {
    struct jm_update_request r; memset(&r,0,sizeof r); jm_command(r.cdb,0x40,0);
    /* Reset can beat control-transfer completion, so reconnect by UID even
     * when the final USB status was lost. No data writes have happened yet. */
    command(io,context,&tag,&r,NULL);
    int ready=0;
    for(unsigned attempt=0;attempt<30 && !ready;++attempt) {
      io->wait_ms(context,500);
      ready=io->reconnect(context,expected_uid) && jm_session_identify(io,context,&id) &&
        !memcmp(id.uid,expected_uid,16) && id.updater==1 && !memcmp(id.product,image+24,16);
    }
    if(!ready) return -4;
  }
  struct jm_update op;
  if (!jm_update_init(&op,image,size,io->feature_size(context)-20)) goto done;
  struct jm_update_request request; uint8_t reply[512]; enum jm_update_step step;
  while ((step=jm_update_next(&op,&request))==JM_UPDATE_TRANSFER) {
    int ok=command(io,context,&tag,&request,reply);
    jm_update_complete(&op,ok,reply,request.in_size);
    if (progress) progress(user,op.offset,op.size);
  }
  result=step==JM_UPDATE_DONE ? 0 : op.error ? op.error : -5;
done:
  return result;
}
static int run_8bitdo(const struct jm_session_io *io,void *context,struct jm_8bitdo_upgrade *operation,
                       jm_session_progress progress,void *user) {
  struct jm_8bitdo_upgrade op=*operation;
  if (!io || !io->hid_read || !io->hid_write) return -1;
  int result = -1;
  uint8_t report[65], reply[128];
  int needs_reply;
  enum jm_8bitdo_step step;
  while ((step = jm_8bitdo_upgrade_next(&op, report + 1, &needs_reply)) == JM_8BITDO_TRANSFER) {
    report[0] = 0;
    /* Bounded drain: avoid sending while stale input remains queued. */
    unsigned drained = 0;
    int n;
    while ((n = io->hid_read(context, reply, sizeof reply, 0)) > 0 && drained < 64)
      ++drained;
    if (n < 0 || drained == 64 || io->hid_write(context, report+1, 64) != 64)
      goto out;
    if (!needs_reply) {
      jm_8bitdo_upgrade_complete(&op, 1, NULL, 0);
      continue;
    }
    /* Bound stale replies as well as each read. No destructive command retries. */
    unsigned stale = 0;
    enum jm_8bitdo_result r;
    do {
      n = io->hid_read(context, reply, sizeof reply, 10000);
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
  return result;
}

/* Drive our full pad installation with raw HID callbacks, verifying sectors.
 * Borrow image storage until return; no HID library or OS calls are required. */
int jm_session_8bitdo(const struct jm_session_io *io,void *context,const uint8_t *image,size_t size,
                        jm_session_progress progress,void *user) {
  struct jm_8bitdo_upgrade op;
  if(!jm_8bitdo_upgrade_init(&op,image,size)) return JM_8BITDO_ARGUMENT;
  return run_8bitdo(io,context,&op,progress,user);
}
/* Run a validated stock pad application using the same compare/erase/program/
 * verify sequence and header-last ordering as an ordinary installation. The
 * raw image's model must be supplied explicitly; the live model is rechecked. */
int jm_session_8bitdo_stock(const struct jm_session_io *io,void *context,const uint8_t *image,size_t size,
                              uint16_t model,jm_session_progress progress,void *user) {
  struct jm_8bitdo_upgrade op;
  if(!jm_8bitdo_restore_init(&op,image,size,model)) return JM_8BITDO_ARGUMENT;
  return run_8bitdo(io,context,&op,progress,user);
}
/* Execute a caller-approved target over an application-supplied connection.
 * This convenience runner blocks in callbacks; use the existing next/complete
 * protocol state machines for asynchronous scheduling. Artifact buffers remain
 * borrowed. The core never opens an OS path or requests UI approval itself. */
int jm_session_run(const struct jm_firmware_target *target, enum jm_session_kind kind,
                     const uint8_t uid[16], const struct jm_session_io *io, void *context,
                     jm_session_progress progress, void *user) {
  if(!target || !io) return -1;
  if(kind==JM_SESSION_CUSTOM) return jm_session_custom(io,context,uid,target->update.data,target->update.size,progress,user);
  if(kind==JM_SESSION_8BITDO && !strcmp(target->chip,"TLSR8208"))
    return jm_session_8bitdo(io,context,target->install.data,target->install.size,progress,user);
  if(kind==JM_SESSION_JIELI && (!strcmp(target->chip,"BR23") || !strcmp(target->chip,"BD19"))) {
    /* A verified package can contain both chips. Reject a mismatched selection
     * before uploading its chip-specific helper, not after flash inspection. */
    if(!io->scsi) return -1;
    uint8_t cdb[6]={0x12,0,0,0,36,0},inquiry[36];struct jm_scsi_result result;
    if(io->scsi(context,cdb,6,JM_SCSI_IN,inquiry,sizeof inquiry,2000,&result) ||
       result.status || result.host_status || result.driver_status || result.transferred!=sizeof inquiry ||
       memcmp(inquiry+8,target->chip,4) || memcmp(inquiry+16,"UBOOT1.00",9)) return -3;
    return jm_session_jieli(io,context,target->loader.data,target->loader.size,target->install.data,target->install.size);
  }
  return -1;
}
