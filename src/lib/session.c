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
 * hid_read gives one input report without it (timeout 0: none waiting);
 * feature buffers include the leading report ID byte (zero for our device).
 * Our tool device: a command is a feature report, its answer comes on input
 * reports addressed to this program's client (several programs can share a
 * device), so tool commands need feature_set, hid_read and feature_size.
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

/* This program's client in the tool device's answers: the adapter's, or
 * made up from addresses and a counter (ASLR keeps programs apart). */
static uint32_t client_of(const struct jm_session_io *io, void *context) {
  static uint32_t made, count;
  uint32_t c;
  if (io->client && (c = io->client(context))) return c;
  if (!made) {
    uintptr_t mix = (uintptr_t)&made ^ (uintptr_t)context << 7 ^ (uintptr_t)io << 13 ^ (uintptr_t)&c;
    made = (uint32_t)mix ^ (uint32_t)((uint64_t)mix >> 32) ^ 0x9e3779b9u * ++count;
    if (!made) made = 1;
  }
  return made;
}
static int tool_ready(const struct jm_session_io *io) {
  return io && io->feature_size && io->feature_set && io->hid_read && io->wait_ms;
}
/* One tool command: the feature report (CDB, u16 length, flags, tag, u32
 * client, data), then its answer from the input reports (client, tag, state,
 * u16 offset, 56 bytes), others' passing by.  REFUSED (another program's
 * command runs): sent again after a short wait. */
static int command(const struct jm_session_io *io, void *context, uint8_t *tag, const struct jm_update_request *r, uint8_t *answer) {
  size_t report=io->feature_size(context);
  if (report<64 || report>JM_HID_REPORT_JIELI) return 0;
  uint8_t buffer[JM_HID_REPORT_JIELI+1], in[128];
  if (r->out_size > report-JM_HID_CLIENT_HEAD_SIZE || r->in_size > 512) return 0;
  uint32_t client=client_of(io,context);
  size_t n = r->out_size ? r->out_size : r->in_size;
  for (unsigned tries=0; tries<2000; ++tries) {
    memset(buffer,0,sizeof buffer);
    memcpy(buffer+1,r->cdb,16);
    buffer[17]=(uint8_t)n; buffer[18]=(uint8_t)(n>>8);
    buffer[19]=(uint8_t)(JM_HID_INPUT | (r->out_size ? JM_HID_OUT : 0)); buffer[20]=++*tag;
    for (unsigned i=0;i<4;++i) buffer[21+i]=(uint8_t)(client>>8*i);
    if (r->out_size) memcpy(buffer+1+JM_HID_CLIENT_HEAD_SIZE,r->data,r->out_size);
    for (unsigned drained=0; drained<256 && io->hid_read(context,in,sizeof in,0)>0; ++drained) {}
    int sent=io->feature_set(context,buffer,report+1);
    /* COMMIT has already been checked before REBOOT. A disconnect racing the
     * reboot status does not invalidate the verified firmware; do not retry. */
    if (r->cdb[1]==JM_CMD_REBOOT) return 1;
    if (sent != (int)report+1) return 0;
    if (r->cdb[1]==JM_CMD_UPDATER) return 1;
    if (r->cdb[1]==JM_CMD_REBOOT_LOADER) return 1;
    size_t got=0; int refused=0;
    for (unsigned waits=0; waits<150 && !refused;) {
      int count=io->hid_read(context,in,sizeof in,100);
      if (count<0) return 0;
      if (count==0) { ++waits; continue; }     /* 15 s without a piece of ours */
      if (count<JM_HID_INPUT_HEAD_SIZE) continue;
      uint32_t whose=(uint32_t)in[0]|(uint32_t)in[1]<<8|(uint32_t)in[2]<<16|(uint32_t)in[3]<<24;
      if (whose!=client || in[4]!=*tag) continue;
      if (in[5]==JM_HID_REFUSED) { refused=1; break; }
      size_t offset=in[6] | (size_t)in[7]<<8;
      if ((in[5]!=JM_HID_MORE && in[5]!=JM_HID_LAST) || offset!=got) return 0;
      size_t length=(size_t)count-JM_HID_INPUT_HEAD_SIZE;
      if (length>r->in_size-got) length=r->in_size-got;
      if (length) memcpy(answer+got,in+JM_HID_INPUT_HEAD_SIZE,length);
      got+=length;
      if (in[5]==JM_HID_LAST) return got==r->in_size;
    }
    if (!refused) return 0;
    io->wait_ms(context,1+(client>>(tries%24)&7));
  }
  return 0;
}
/* Read IDENTIFY over the tool device's framing. No OS API calls. */
int jm_session_identify(const struct jm_session_io *io, void *context, struct jm_identity *id) {
  if (!tool_ready(io)) return 0;
  uint8_t reply[JM_ID_EXT_SIZE], tag=0; struct jm_update_request r;
  memset(&r,0,sizeof r); jm_command(r.cdb,0,0); r.in_size=JM_ID_EXT_SIZE;
  return command(io,context,&tag,&r,reply) && jm_identify_decode(reply,sizeof reply,id);
}
/* Request stock bootloader entry from our app or updater after checking the
 * previously selected UID. No flash is written. Return zero when the complete
 * feature request was sent, -1 for arguments, -2 for identity mismatch, -3 for
 * an unconfirmed transfer. A disconnect can race USB completion: rescan after
 * either result, never automatically resend. No bootloader discovery occurs. */
int jm_session_stock_loader(const struct jm_session_io *io,void *context,const uint8_t uid[16]) {
  if(!uid || !tool_ready(io)) return -1;
  struct jm_identity id;
  if(!jm_session_identify(io,context,&id) || memcmp(id.uid,uid,16)) return -2;
  struct jm_update_request request;memset(&request,0,sizeof request);jm_command(request.cdb,JM_CMD_REBOOT_LOADER,0);
  uint8_t tag=0;
  return command(io,context,&tag,&request,NULL)?0:-3;
}
/* Run a custom update with shared identity, framing and reconnect policy.
 * Callbacks may block; none may retain supplied buffers after returning. */
int jm_session_custom(const struct jm_session_io *io, void *context, const uint8_t expected_uid[16], const uint8_t *image, size_t size,
                           void (*progress)(void *, size_t, size_t), void *user) {
  struct jm_identity id; uint8_t tag=0;
  if (!expected_uid || !jm_update_image_check(image,size,NULL)) return -1;
  if (!tool_ready(io) || !io->reconnect) return -2;
  int result=-3;
  if (!jm_session_identify(io,context,&id) || memcmp(id.uid,expected_uid,16) || memcmp(id.product,image+JM_MAIN_PRODUCT_OFF,JM_PRODUCT_SIZE)) goto done;
  if (!id.updater) {
    struct jm_update_request r; memset(&r,0,sizeof r); jm_command(r.cdb,JM_CMD_UPDATER,0);
    /* Reset can beat control-transfer completion, so reconnect by UID even
     * when the final USB status was lost. No data writes have happened yet. */
    command(io,context,&tag,&r,NULL);
    int ready=0;
    for(unsigned attempt=0;attempt<30 && !ready;++attempt) {
      io->wait_ms(context,500);
      ready=io->reconnect(context,expected_uid) && jm_session_identify(io,context,&id) &&
        !memcmp(id.uid,expected_uid,16) && id.updater==1 && !memcmp(id.product,image+JM_MAIN_PRODUCT_OFF,JM_PRODUCT_SIZE);
    }
    if(!ready) return -4;
  }
  struct jm_update op;
  if (!jm_update_init(&op,image,size,io->feature_size(context)-JM_HID_CLIENT_HEAD_SIZE)) goto done;
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
