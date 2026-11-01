#include "../../src/lib/jollymaam.h"
#include "../../src/lib/jieli_image/crypto.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
struct fake { unsigned mode,cmd,tag,updater,begun,written,committed,reconnects,starts,dropped,rom; uint8_t flash[600]; };
static void wr32(uint8_t *p,uint32_t v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static size_t feature_size(void *user) { (void)user;return 356; }
static void wait_ms(void *user,unsigned ms) { (void)user;(void)ms; }
static int reconnect(void *user,const uint8_t uid[16]) { struct fake *f=user;assert(uid[0]==42);++f->reconnects;return 1; }
static int set(void *user,const uint8_t *data,size_t size) {
  struct fake *f=user;assert(size==357 && data[0]==0 && data[1]==0xf5);
  f->cmd=data[2];f->tag=data[20];unsigned arg=rd32(data+3);
  size_t n=data[17]|(size_t)data[18]<<8;
  switch(f->cmd) {
  case 0:case 0x41:break;
  case 0x40:f->updater=1;break;
  case 0x11:++f->rom;break;
  case 0x42:assert(arg==600 && n==44);++f->begun;break;
  case 0x43:
    assert(f->begun && arg==f->written);
    if(f->mode==3) return -1;
    if(f->mode==4 && !f->dropped) { f->dropped=1;break; }
    assert(n<=600-f->written);memcpy(f->flash+f->written,data+21,n);f->written+=(unsigned)n;break;
  case 0x44:assert(f->written==600);f->committed=1;break;
  case 0x10:assert(f->committed);++f->starts;break;
  default:assert(0);
  }
  return (int)size;
}
static int get(void *user,uint8_t *out,size_t size) {
  struct fake *f=user;assert(size==357);memset(out,0,size);out[1]=(uint8_t)f->tag;out[2]=3;
  if(f->cmd==0) {
    out[3]=88;uint8_t *id=out+5;memcpy(id,"C2PD",4);id[7]=(uint8_t)f->updater;
    id[24]=f->mode==1 || (f->mode==2 && f->reconnects)?43:42;memcpy(id+40,"test",4);
  } else if(f->cmd==0x41) {
    out[3]=32;uint8_t *info=out+5;info[0]=1;info[1]=f->committed?2:f->begun?1:0;info[3]=f->committed;
    wr32(info+8,600);wr32(info+12,f->written);wr32(info+16,f->begun?600:0);wr32(info+24,128);
  }
  return (int)size;
}
static void image_make(uint8_t image[8192]) {
  memset(image, 0xff, 8192);
  memcpy(image + 8, "KNLT", 4);
  wr32(image + 24, 32);
  uint32_t crc = 0xffffffff;
  for (unsigned i = 0; i < 28; ++i) {
    crc ^= image[i];
    for (unsigned j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  wr32(image + 28, crc);
  memcpy(image + 4096, "MAIN", 4);
  memset(image + 4120, 0, 16);
  memcpy(image + 4120, "U2C_WIRELESS", 12);
  wr32(image + 4100, 4096 - 0x120);
  wr32(image + 4104, jm_crc16(image + 4096 + 0x120, 4096 - 0x120));
  wr32(image + 4112, 1);
  wr32(image + 4136, jm_crc16(image + 4100, 36));

}
struct fake8 { uint8_t flash[8192],request[64]; unsigned starts; };
static int write8(void *user,const uint8_t *data,size_t size) {
  struct fake8 *f=user;assert(size==64);memcpy(f->request,data,64);
  uint32_t at=rd32(data+13);unsigned cmd=data[1],n=data[5];
  if(cmd==4) { assert(at>=0x6000 && at+4096<=0x8000);memset(f->flash+at-0x6000,0xff,4096); }
  if(cmd==3) { assert(at>=0x6000 && at+n<=0x8000);memcpy(f->flash+at-0x6000,data+17,n); }
  if(cmd==7) ++f->starts;
  return 64;
}
static int read8(void *user,uint8_t *data,size_t size,unsigned timeout) {
  struct fake8 *f=user;if(!timeout) return 0;assert(size>=64);memset(data,0,64);
  data[1]=5;data[4]=f->request[1];memcpy(data+14,f->request+13,4);
  if(data[4]==8) { data[6]=2;data[18]=0x1b;data[19]=0x30; }
  if(data[4]==5) { unsigned n=f->request[5];uint32_t at=rd32(f->request+13);
    assert(at>=0x6000 && at+n<=0x8000);data[6]=(uint8_t)n;memcpy(data+18,f->flash+at-0x6000,n); }
  return 64;
}
static void test8(void) {
  uint8_t image[8192];image_make(image);struct fake8 f;memset(&f,0,sizeof f);
  struct jm_firmware_target target;memset(&target,0,sizeof target);strcpy(target.chip,"TLSR8208");
  target.install.data=image;target.install.size=sizeof image;
  struct jm_session_io io={0};io.hid_read=read8;io.hid_write=write8;
  assert(!jm_session_run(&target,JM_SESSION_8BITDO,NULL,&io,&f,NULL,NULL));
  assert(f.starts==1 && !memcmp(f.flash,image,sizeof image));
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t rd32be(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void entry(uint8_t *p, const char *name, unsigned offset, unsigned size,
                   unsigned flags, unsigned last, unsigned crc) {
  struct jm_jieli_entry e = {0};
  memcpy(e.name, name, strlen(name)); e.offset = offset; e.size = size;
  e.flags = (uint8_t)flags; e.index = (uint16_t)last; e.data_crc = (uint16_t)crc;
  jm_jieli_encode_entry(p, &e);
}
static uint8_t *fixture(size_t *size) {
  *size = 4096;
  uint8_t *image = malloc(*size); assert(image); memset(image, 0xff, *size);
  memset(image + 16, 0, 16); memcpy(image + 16, "test", 4);
  jm_jieli_enc(image, 32, 0xffff); image[2] = 1;
  wr16(image, jm_crc16(image + 2, 30)); jm_jieli_enc(image, 32, 0xffff);
  memset(image + 128, 0, 34); jm_jieli_set_record_key(image + 128, 0xffff);
  entry(image + 32, "isd_config.ini", 128, 34, 2, 0, jm_crc16(image + 128, 34));
  entry(image + 64, "app_dir_head", 512, 0xffffffff, 0x81, 1, 0xffff);
  memset(image + 512 + 64, 0xa5, 32);
  entry(image + 512 + 32, "app.bin", 64, 32, 0x82, 1, jm_crc16(image + 512 + 64, 32));
  entry(image + 512, "app_area_head", 0x1e00120, 96, 0x83, 1, jm_crc16(image + 512 + 32, 64));
  jm_jieli_enc(image + 32, 32, 0xffff); jm_jieli_enc(image + 64, 32, 0xffff);
  jm_jieli_sfc(image, 512, (uint32_t)*size - 512, 512, 0xffff);
  return image;
}
struct fakej { uint8_t flash[65536]; unsigned erases,writes,reads,starts,fail_read,mismatch,uploads; };
static int scsi_fake(void *user,const uint8_t *cdb,size_t cdb_size,enum jm_scsi_direction direction,
                      uint8_t *data,size_t size,unsigned timeout,struct jm_scsi_result *result) {
  struct fakej *f=user;(void)timeout;memset(result,0,sizeof *result);result->transferred=size;
  if(cdb_size==6) { assert(cdb[0]==0x12 && size==36);memset(data,0,size);volatile uint8_t *out=data;for(unsigned i=0;i<4;++i) out[8+i]=(uint8_t)(f->mismatch?"BD19":"BR23")[i];for(unsigned i=0;i<9;++i) out[16+i]=(uint8_t)"UBOOT1.00"[i];return 0; }
  unsigned cmd=(unsigned)cdb[0]<<8|cdb[1];uint32_t at=rd32be(cdb+2);
  if(cmd==0xfb06) { ++f->uploads;return 0; }
  if(cmd==0xfd05) { ++f->reads;if(f->fail_read) { result->transferred=size-1;return 0; } assert(at+size<=sizeof f->flash);memcpy(data,f->flash+at,size);return 0; }
  if(cmd==0xfb04) { assert(direction==JM_SCSI_OUT && at+size<=sizeof f->flash);memcpy(f->flash+at,data,size);++f->writes;return 0; }
  assert(size==16);memset(data,0,size);data[0]=cdb[0];data[1]=cdb[1];
  if(cmd==0xfc0a) { data[4]=16;data[5]=0x40;data[6]=0xef; }
  else if(cmd==0xfc14) data[4]=2;
  else if(cmd==0xfc09) { uint8_t key[2]={0xff,0xff};jm_jieli_crc_cipher(key,2,0xffffffff);data[7]=key[0];data[6]=key[1]; }
  else if(cmd==0xfb00 || cmd==0xfb01) { unsigned n=cmd==0xfb00?65536:4096;assert(at+n<=sizeof f->flash);memset(f->flash+at,0xff,n);++f->erases; }
  else if(cmd==0xfc0c) { assert(at==1);++f->starts; }
  else assert(cmd==0xfb08);
  return 0;
}
struct backup { uint8_t data[65536];size_t size;int reject; };
static int backup_sink(void *user,uint32_t at,const uint8_t *data,size_t size) {
  struct backup *b=user;assert(at==b->size && size<=sizeof b->data-b->size);
  if(b->reject) return 0;
  memcpy(b->data+at,data,size);b->size+=size;return 1;
}
static void testj(void) {
  size_t n;uint8_t *image=fixture(&n),loader[512]={0};struct fakej f;memset(&f,0x55,sizeof f);
  f.erases=f.writes=f.reads=f.starts=f.fail_read=f.mismatch=f.uploads=0;memcpy(f.flash,image,n);
  struct jm_firmware_target target;memset(&target,0,sizeof target);strcpy(target.chip,"BR23");
  target.install.data=image;target.install.size=n;target.loader.data=loader;target.loader.size=sizeof loader;
  struct jm_session_io io={0};io.scsi=scsi_fake;io.wait_ms=wait_ms;
  f.mismatch=1;
  assert(jm_session_run(&target,JM_SESSION_JIELI,NULL,&io,&f,NULL,NULL)==-3);
  assert(!f.uploads && !f.erases && !f.writes);f.mismatch=0;
  assert(!jm_session_run(&target,JM_SESSION_JIELI,NULL,&io,&f,NULL,NULL));
  assert(f.erases && f.writes && !memcmp(f.flash,image,n));
  for(size_t i=n;i<sizeof f.flash;++i) assert(f.flash[i]==0x55);
  struct jm_jieli_device device;
  assert(!jm_session_jieli_open(&io,&f,loader,sizeof loader,&device));
  assert(device.flash_size==sizeof f.flash && device.io==512 && device.key==0xffff);
  struct backup b={0};
  assert(!jm_session_jieli_backup(&io,&f,&device,backup_sink,&b,NULL,NULL));
  assert(b.size==sizeof f.flash && !memcmp(b.data,f.flash,sizeof f.flash));
  unsigned reads=f.reads;
  assert(!jm_session_jieli_read(&io,&f,&device,device.flash_size-1,b.data,2));assert(reads==f.reads);
  b.size=0;b.reject=1;assert(jm_session_jieli_backup(&io,&f,&device,backup_sink,&b,NULL,NULL)==-3);
  f.fail_read=1;assert(jm_session_jieli_backup(&io,&f,&device,backup_sink,&b,NULL,NULL)==-2);
  assert(jm_session_jieli_start(&io,&f) && f.starts==1);
  free(image);
}
int main(void) {
  uint8_t image[600]={0},uid[16]={42};memcpy(image,"MAIN",4);wr32(image+4,312);wr32(image+16,1);
  memcpy(image+24,"test",4);memset(image+288,0xa5,312);wr32(image+8,jm_crc16(image+288,312));wr32(image+40,jm_crc16(image+4,36));
  struct jm_firmware_target target;memset(&target,0,sizeof target);target.update.data=image;target.update.size=600;
  struct jm_session_io io={0};io.feature_size=feature_size;io.feature_set=set;io.feature_get=get;io.wait_ms=wait_ms;io.reconnect=reconnect;
  for(unsigned mode=0;mode<5;++mode) {
    struct fake f;memset(&f,0,sizeof f);f.mode=mode;
    int result=jm_session_run(&target,JM_SESSION_CUSTOM,uid,&io,&f,NULL,NULL);
    if(mode==0 || mode==4) { assert(!result && f.starts==1 && f.reconnects==1 && !memcmp(image,f.flash,600)); }
    else { assert(result && !f.starts && !f.committed);if(mode==1 || mode==2) assert(!f.begun); }
  }
  for(unsigned updater=0;updater<2;++updater) {
    struct fake f;memset(&f,0,sizeof f);f.updater=updater;
    uint8_t selected[16]={43};
    assert(jm_session_stock_loader(&io,&f,selected)==-2 && !f.rom && !f.begun);
    selected[0]=42;
    assert(!jm_session_stock_loader(&io,&f,selected) && f.rom==1 && !f.begun);
  }
  test8();testj();
  puts("Callback-only complete update sessions passed");return 0;
}
