#include "restore.h"
#include "upgrade_8bitdo.h"
#include "jieli_image/image.h"
#include "jieli_image/ufw.h"
#include <stdlib.h>
#include <string.h>

/* Restore an explicitly selected raw/UFW Jieli image, with a mandatory full
 * pre-erase backup. The write callback accepts borrowed blocks; finish must durably finish and
 * close the output before returning nonzero. No erase is sent until it does.
 * Check image CRCs, chip, installed product, capacity and exact chip key first.
 * Stock application check records cannot be safely rekeyed by this API.
 * Write supplied bytes unchanged, including settings carried by a full dump;
 * sectors beyond a partial vendor image remain untouched. No Nova keep override
 * applies to restoration. Return zero only after readback verification, leaving
 * the RAM helper running. -1 arguments/image, -2 chip, -3 helper, -4 product,
 * -5 key/capacity, -6 allocation/preparation, -7 backup, -8 flash/verification.
 * Input and callback context remain borrowed for this synchronous operation. */
int jm_restore_jieli(const struct jm_session_io *io,void *context,enum jm_jieli_chip chip,
                       const char *product,const uint8_t *loader,size_t loader_size,
                       const uint8_t *input,size_t input_size,const struct jm_restore_backup *backup,
                       jm_session_progress progress,void *progress_user) {
  if(!io || !io->scsi || !io->wait_ms || !product || !*product || strlen(product)>16 ||
     !loader || !loader_size || !input || !input_size || input_size>32*1024*1024 || !backup || !backup->write || !backup->finish ||
     (chip!=JM_JIELI_BR23 && chip!=JM_JIELI_BD19)) return -1;
  const uint8_t *image;size_t size;
  enum jm_jieli_ufw_result ufw=jm_jieli_ufw_open(input,input_size,&image,&size);
  if(ufw!=JM_JIELI_UFW_RAW && ufw!=JM_JIELI_UFW_OK) return -1;
  uint8_t *workspace=malloc(size?size:1),*head=NULL;struct jm_jieli_image view;int result=-1;
  if(!workspace || jm_jieli_image_open(&view,image,size,workspace,size)) goto done;
  char wanted[17];jm_jieli_name_string(wanted,image+view.base+16);
  if(strcmp(product,wanted)) goto done;
  uint8_t cdb[6]={0x12,0,0,0,36,0},inquiry[36];struct jm_scsi_result status;
  if(io->scsi(context,cdb,6,JM_SCSI_IN,inquiry,36,2000,&status) || status.status ||
     status.host_status || status.driver_status || status.transferred!=36 ||
     memcmp(inquiry+8,chip==JM_JIELI_BR23?"BR23":"BD19",4) || memcmp(inquiry+16,"UBOOT1.00",9)) { result=-2;goto done; }
  struct jm_jieli_device device;
  if(jm_session_jieli_open(io,context,loader,loader_size,&device)) { result=-3;goto done; }
  if(size>device.flash_size || view.key!=device.key) { result=-5;goto done; }
  size_t head_size=device.flash_size<0x11000?device.flash_size:0x11000;
  head=malloc(head_size);uint8_t header[32];uint32_t base;char installed[17];
  if(!head || !jm_session_jieli_read(io,context,&device,0,head,head_size) ||
     !jm_jieli_find_base(head,head_size,header,&base)) { result=-4;goto done; }
  jm_jieli_name_string(installed,head+base+16);
  if(strcmp(installed,product)) { result=-4;goto done; }
  size_t capacity=(size+4095)&~(size_t)4095;
  uint8_t *grown=realloc(workspace,capacity);
  if(!grown) { result=-6;goto done; } workspace=grown;
  struct jm_jieli_flash flash;
  if(!jm_jieli_flash_init(&flash,&device,installed,image,size,workspace,capacity,NULL,0)) { result=-6;goto done; }
  if(jm_session_jieli_backup(io,context,&device,backup->write,backup->user,NULL,NULL) || !backup->finish(backup->user)) { result=-7;goto done; }
  struct jm_jieli_request request;uint8_t reply[32768];enum jm_jieli_step step;
  while((step=jm_jieli_flash_next(&flash,&request))==JM_JIELI_TRANSFER) {
    io->wait_ms(context,request.delay_ms);
    uint8_t *data=request.direction==JM_SCSI_OUT?(uint8_t *)request.data:reply;
    struct jm_scsi_result transfer;
    int ok=!io->scsi(context,request.cdb,16,request.direction,data,request.size,request.timeout_ms,&transfer);
    ok=ok && !transfer.status && !transfer.host_status && !transfer.driver_status;
    jm_jieli_flash_complete(&flash,ok,reply,ok?transfer.transferred:0);
    if(progress) progress(progress_user,flash.offset,flash.size);
  }
  result=step==JM_JIELI_DONE?0:-8;
done:
  free(head);free(workspace);return result;
}
static int exchange(const struct jm_session_io *io,void *context,uint16_t command,uint32_t address,
                       uint8_t *out,size_t expected) {
  uint8_t request[64],reply[128];
  unsigned drained=0;int n;
  while((n=io->hid_read(context,reply,sizeof reply,0))>0 && drained<64) ++drained;
  if(n<0 || drained==64 || jm_8bitdo_request(request,command,address,NULL,command==8?4:expected) ||
     io->hid_write(context,request,sizeof request)!=64) return 0;
  for(unsigned stale=0;stale<8;++stale) {
    n=io->hid_read(context,reply,sizeof reply,10000);
    if(n<=0) return 0;
    const uint8_t *payload;size_t size;
    enum jm_8bitdo_result result=jm_8bitdo_reply(reply,(size_t)n,command,address,&payload,&size);
    if(result==JM_8BITDO_STALE) continue;
    if(result || size!=expected) return 0;
    memcpy(out,payload,size);return 1;
  }
  return 0;
}
/* Back up the pad's application region [0x6000,0x6f000) without touching its
 * stock loader or settings. Sink offsets are relative to 0x6000, suitable for
 * saving our complete current installation and restoring with the normal full
 * image updater. Check live model before reads; refuse receiver model 301c.
 * Caller finishes/closes output after this read-only function returns. Return
 * zero on success, -1
 * arguments, -2 model/protocol, -3 read, -4 output. No erase or boot occurs. */
int jm_backup_8bitdo(const struct jm_session_io *io,void *context,uint16_t model,jm_session_sink sink,
                       void *user,jm_session_progress progress,void *progress_user) {
  if(!io || !io->hid_read || !io->hid_write || model!=0x301b || !sink) return -1;
  uint8_t identity[2];
  if(!exchange(io,context,8,0,identity,2) || identity[0]!=0x1b || identity[1]!=0x30) return -2;
  const uint32_t size=0x6f000-0x6000;uint8_t data[JM_8BITDO_CHUNK];
  for(uint32_t at=0;at<size;) {
    size_t n=size-at;if(n>sizeof data) n=sizeof data;
    if(!exchange(io,context,5,0x6000+at,data,n)) return -3;
    if(!sink(user,at,data,n)) return -4;
    at+=(uint32_t)n;if(progress) progress(progress_user,at,size);
  }
  return 0;
}
/* Restore a CRC-checked stock pad app, keeping the stock loader/settings.
 * Require a complete saved application-region backup before any erases. The
 * backup finish callback must close its output successfully. The live model is
 * checked by both backup and flashing. Return zero after verification and boot
 * request, -1 image/model arguments, -2 backup, otherwise the update error. */
int jm_restore_8bitdo(const struct jm_session_io *io,void *context,uint16_t model,
                        const uint8_t *image,size_t size,const struct jm_restore_backup *backup,
                        jm_session_progress progress,void *progress_user) {
  struct jm_8bitdo_upgrade op;
  if(!backup || !backup->write || !backup->finish || !jm_8bitdo_restore_init(&op,image,size,model)) return -1;
  if(jm_backup_8bitdo(io,context,model,backup->write,backup->user,NULL,NULL) || !backup->finish(backup->user)) return -2;
  return jm_session_8bitdo_stock(io,context,image,size,model,progress,progress_user);
}

#ifdef UNITTEST
#include "jieli_image/builder.h"
#include "jieli_image/crypto.h"
#include <assert.h>
#include <stdio.h>
struct fake_restore {
  uint8_t flash[0x80000],saved[0x80000],request[64];
  size_t saved_size;unsigned mode,finished,uploads,reads,writes,erases,starts;
};
static void put16(uint8_t *p,unsigned v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get_be(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint32_t get_le(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int save_test(void *user,uint32_t at,const uint8_t *data,size_t size) {
  struct fake_restore *f=user;assert(!f->finished && at==f->saved_size && size<=sizeof f->saved-at);
  if(f->mode==1) return 0;
  memcpy(f->saved+at,data,size);f->saved_size+=size;return 1;
}
static int finish_test(void *user) { struct fake_restore *f=user;if(f->mode==2) return 0;f->finished=1;return 1; }
static void wait_test(void *user,unsigned ms) { (void)user;(void)ms; }
static int scsi_test(void *user,const uint8_t *cdb,size_t cdb_size,enum jm_scsi_direction dir,
                       uint8_t *data,size_t size,unsigned timeout,struct jm_scsi_result *result) {
  struct fake_restore *f=user;(void)timeout;memset(result,0,sizeof *result);result->transferred=size;
  if(cdb_size==6) {
    assert(size==36);memset(data,0,size);memcpy(data+8,f->mode==5?"BD19":"BR23",4);memcpy(data+16,"UBOOT1.00",9);return 0;
  }
  unsigned cmd=(unsigned)cdb[0]<<8|cdb[1];uint32_t at=get_be(cdb+2);
  if(cmd==0xfb06) { ++f->uploads;return 0; }
  if(cmd==0xfd05) {
    assert(at+size<=0x10000);++f->reads;memcpy(data,f->flash+at,size);
    if(f->mode==7) result->transferred=size-1;
    if(f->mode==6 && f->writes) data[0]^=1;
    return 0;
  }
  if(cmd==0xfb04) {
    assert(f->finished && f->saved_size==0x10000 && dir==JM_SCSI_OUT && at+size<=0x10000);
    ++f->writes;memcpy(f->flash+at,data,size);return 0;
  }
  assert(size==16);memset(data,0,size);data[0]=cdb[0];data[1]=cdb[1];
  if(cmd==0xfc0a) { data[4]=16;data[5]=0x40;data[6]=0xef; }
  else if(cmd==0xfc14) data[4]=2;
  else if(cmd==0xfc09) {
    uint8_t key[2]={f->mode==3?0:0xff,0xff};jm_jieli_crc_cipher(key,2,0xffffffff);data[7]=key[0];data[6]=key[1];
  } else if(cmd==0xfb00 || cmd==0xfb01) {
    unsigned n=cmd==0xfb00?65536:4096;
    assert(f->finished && f->saved_size==0x10000 && at+n<=0x10000);
    ++f->erases;memset(f->flash+at,0xff,n);
  } else assert(cmd==0xfb08);
  return 0;
}
struct inputs { const char *ini;uint8_t boot[20],app[32]; };
static int read_test(void *user,const char *name,const uint8_t **data,size_t *size) {
  struct inputs *f=user;
  if(!strcmp(name,"config")) { *data=(const uint8_t *)f->ini;*size=strlen(f->ini); }
  else if(!strcmp(name,"boot")) { *data=f->boot;*size=sizeof f->boot; }
  else if(!strcmp(name,"app.bin")) { *data=f->app;*size=sizeof f->app; }
  else return 0;
  return 1;
}
static void test_jieli(void) {
  struct inputs inputs={0};inputs.ini="[EXTRA_CFG_PARAM]\nPID=TEST;\nENTRY=0x1e00120;\n";
  put16(inputs.boot,1);put16(inputs.boot+2,4);put32(inputs.boot+8,16);put16(inputs.boot+14,jm_crc16(inputs.boot,14));memcpy(inputs.boot+16,"boot",4);
  struct jm_jieli_build_options options={0};options.ini="config";options.uboot="boot";options.files[0]="app.bin";options.nf=1;
  struct jm_jieli_build_result image;char error[256];int built=jm_jieli_build(&options,read_test,NULL,&inputs,&image,error);if(!built) fprintf(stderr,"fixture: %s\n",error);assert(built);
  struct jm_session_io io={0};io.scsi=scsi_test;io.wait_ms=wait_test;uint8_t loader[512]={0};
  for(unsigned mode=0;mode<8;++mode) {
    struct fake_restore *f=calloc(1,sizeof *f);assert(f);memset(f->flash,0x55,sizeof f->flash);memcpy(f->flash,image.image,image.image_size);f->mode=mode;
    struct jm_restore_backup backup={save_test,finish_test,f};
    const char *product=mode==4?"WRONG":"TEST";
    int result=jm_restore_jieli(&io,f,JM_JIELI_BR23,product,loader,sizeof loader,image.image,image.image_size,&backup,NULL,NULL);
    if(!mode) {
      assert(!result && f->finished && f->erases && f->writes && !memcmp(f->flash,image.image,image.image_size));
      assert(!memcmp(f->saved,image.image,image.image_size));
      for(size_t i=(image.image_size+4095)&~(size_t)4095;i<0x10000;++i) assert(f->flash[i]==0x55);
    } else {
      assert(result);
      if(mode!=6) assert(!f->erases && !f->writes);
      if(mode==4 || mode==5) assert(!f->uploads);
    }
    free(f);
  }
  jm_jieli_build_free(&image);
}
static int hid_write_test(void *user,const uint8_t *data,size_t size) {
  struct fake_restore *f=user;assert(size==64);memcpy(f->request,data,64);
  uint32_t at=get_le(data+13);unsigned command=data[1],n=data[5];
  if(command==4) {
    assert(f->finished && f->saved_size==0x69000 && at>=0x6000 && at+4096<=0x6f000);
    ++f->erases;memset(f->flash+at,0xff,4096);
  } else if(command==3) {
    assert(f->finished && at>=0x6000 && at+n<=0x6f000);++f->writes;memcpy(f->flash+at,data+17,n);
  } else if(command==7) ++f->starts;
  else assert(command==8 || command==5);
  return 64;
}
static int hid_read_test(void *user,uint8_t *data,size_t size,unsigned timeout) {
  struct fake_restore *f=user;if(!timeout) return 0;assert(size>=64);memset(data,0,size);data[1]=5;data[4]=f->request[1];
  uint32_t at=get_le(f->request+13);unsigned n=f->request[5];put32(data+14,at);
  if(data[4]==8) { data[6]=2;data[18]=f->mode==5?0x1c:0x1b;data[19]=0x30; }
  else if(data[4]==3 || data[4]==4) return 64;
  else {
    if(!(data[4]==5 && at>=0x6000 && at+n<=0x6f000)) fprintf(stderr,"fake read mode=%u cmd=%u at=%x n=%u\n",f->mode,data[4],at,n);
    assert(data[4]==5 && at>=0x6000 && at+n<=0x6f000);++f->reads;data[6]=(uint8_t)n;memcpy(data+18,f->flash+at,n);
    if(f->mode==6 && f->writes) data[18]^=1;
    if(f->mode==7) return 18+(int)n-1;
  }
  return 64;
}
static void test_8bitdo(void) {
  uint8_t image[5000];memset(image,0xa5,sizeof image);memcpy(image+8,"KNLT",4);put32(image+24,sizeof image);
  uint32_t crc=0xffffffff;
  for(size_t i=0;i<sizeof image-4;++i) { crc^=image[i];for(unsigned j=0;j<8;++j) crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
  put32(image+sizeof image-4,crc);
  uint8_t dat[28+sizeof image+16];memset(dat,0,sizeof dat);put32(dat+4,0x6000);
  put32(dat+8,sizeof dat-28);put32(dat+12,0x301b);memcpy(dat+28,image,sizeof image);
  const uint8_t *raw;size_t raw_size;
  assert(jm_8bitdo_stock_image(dat,sizeof dat,0x301b,&raw,&raw_size) && raw_size==sizeof image);
  put32(dat+12,0x301c);assert(!jm_8bitdo_stock_image(dat,sizeof dat,0x301b,&raw,&raw_size));put32(dat+12,0x301b);
  dat[sizeof dat-1]=1;assert(!jm_8bitdo_stock_image(dat,sizeof dat,0x301b,&raw,&raw_size));dat[sizeof dat-1]=0;
  struct jm_session_io io={0};io.hid_read=hid_read_test;io.hid_write=hid_write_test;
  for(unsigned mode=0;mode<8;++mode) {
    struct fake_restore *f=calloc(1,sizeof *f);assert(f);f->mode=mode;memset(f->flash,0x55,sizeof f->flash);
    struct jm_restore_backup backup={save_test,finish_test,f};
    int result=jm_restore_8bitdo(&io,f,mode==3?0x301c:0x301b,mode?image:dat,mode==4?sizeof image-1:mode?sizeof image:sizeof dat,&backup,NULL,NULL);
    if(!mode) {
      assert(!result && f->finished && f->starts==1 && !memcmp(f->flash+0x6000,image,sizeof image));
      for(size_t i=0;i<0x6000;++i) assert(f->flash[i]==0x55);
      for(size_t i=0x6f000;i<sizeof f->flash;++i) assert(f->flash[i]==0x55);
      for(size_t i=0;i<f->saved_size;++i) assert(f->saved[i]==0x55);
    } else {
      assert(result && !f->starts);
      if(mode!=6) assert(!f->erases && !f->writes);
    }
    free(f);
  }
}
int main(void) { test_jieli();test_8bitdo();puts("Stock restore: backup barriers, identity, key, bounds and readback passed");return 0; }
#endif
