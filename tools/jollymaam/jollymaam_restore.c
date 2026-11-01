#define _POSIX_C_SOURCE 200809L
#include "jollymaam_restore.h"
#include "../../src/lib/jollymaam.h"
#include "../../src/lib/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define open_output(path) _open(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE)
#define output_stream(fd) _fdopen(fd,"wb")
#define sync_output(file) _commit(_fileno(file))
#define close_fd(fd) _close(fd)
#else
#include <unistd.h>
#define open_output(path) open(path,O_WRONLY|O_CREAT|O_EXCL,0600)
#define output_stream(fd) fdopen(fd,"wb")
#define sync_output(file) fsync(fileno(file))
#define close_fd(fd) close(fd)
#endif
struct backup_file { FILE *file;size_t written;int finished; };
static uint8_t *load(const char *path,size_t *size) {
  FILE *f=fopen(path,"rb");if(!f) return NULL;
  long n;
  if(fseek(f,0,SEEK_END) || (n=ftell(f))<=0 || n>32*1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f);return NULL; }
  uint8_t *data=malloc((size_t)n);
  if(!data) { fclose(f);return NULL; }
  int ok=fread(data,1,(size_t)n,f)==(size_t)n;
  if(fclose(f)) ok=0;
  if(!ok) { free(data);return NULL; }*size=(size_t)n;return data;
}
static int save(void *user,uint32_t at,const uint8_t *data,size_t size) {
  struct backup_file *b=user;
  if(!b->file || at!=b->written || fwrite(data,1,size,b->file)!=size) return 0;
  b->written+=size;return 1;
}
static int finish(void *user) {
  struct backup_file *b=user;
  if(!b->file) return 0;
  int ok=!fflush(b->file) && !sync_output(b->file);
  if(fclose(b->file)) ok=0;
  b->file=NULL;b->finished=ok;
  if(ok) printf("Backup saved and closed: %zu bytes. Starting restore.\n",b->written);
  return ok;
}
static void progress(void *user,size_t done,size_t total) {
  unsigned *last=user,percent=total?(unsigned)((uint64_t)done*100/total):0;
  if(percent!=*last) { *last=percent;fprintf(stderr,"Transfer/verification: %u%%\n",percent); }
}
static int check_jieli(const uint8_t *data,size_t size,const char *product) {
  const uint8_t *image;size_t length;
  enum jm_jieli_ufw_result ufw=jm_jieli_ufw_open(data,size,&image,&length);
  if(ufw!=JM_JIELI_UFW_OK && ufw!=JM_JIELI_UFW_RAW) return 0;
  uint8_t *workspace=malloc(length);struct jm_jieli_image view;int ok=0;
  if(workspace && !jm_jieli_image_open(&view,image,length,workspace,length)) {
    char actual[17];jm_jieli_name_string(actual,image+view.base+16);
    ok=!strcmp(product,actual);
    if(ok) printf("Validated Jieli image: product=%s key=0x%04x bytes=%zu (matching live key required)\n",actual,view.key,length);
  }
  free(workspace);return ok;
}
/* Explicit stock restore/check CLI. No automatic device or image selection.
 * Backup creation is exclusive, so an input or previous backup cannot be
 * overwritten. Core calls finish (including fsync/close) before its first erase.
 * Vendor binaries remain external to the repository and firmware distribution. */
int jollymaam_restore_app(int argc,char **argv,int jieli) {
  int check=!strcmp(argv[1],"check-stock");
  if((check && argc!=4) || (!check && (argc!=(jieli?9:7) || strcmp(argv[2],"--yes")))) {
    fprintf(stderr,"usage: jollymaam jieli check-stock PRODUCT IMAGE\n"
                   "       jollymaam 8bitdo check-stock 0x301b STOCK.bin|DAT\n"
                   "       jollymaam jieli restore --yes DEVICE BR23|BD19 PRODUCT LOADER IMAGE NEW_BACKUP\n"
                   "       jollymaam 8bitdo restore --yes HID_PATH 0x301b STOCK.bin|DAT NEW_BACKUP\n");return 2;
  }
  const char *image_path=argv[check?3:jieli?7:5],*product=jieli?argv[check?2:5]:NULL;
  size_t size=0;uint8_t *image=load(image_path,&size);int valid=0;
  if(image) {
    if(jieli) valid=check_jieli(image,size,product);
    else {
      struct jm_8bitdo_upgrade op;
      valid=!strcmp(argv[check?2:4],"0x301b") && jm_8bitdo_restore_init(&op,image,size,0x301b);
      if(valid) printf("Validated packed Telink image: %zu bytes; caller asserts pad model 0x301b\n",size);
    }
  }
  if(!valid) { fprintf(stderr,"Invalid image/product/model. Use a matching raw Jieli image/UFW or extracted 8BitDo pad app or vendor DAT.\n");free(image);return 1; }
  if(check) { free(image);return 0; }
  uint8_t *loader=NULL;size_t loader_size=0;enum jm_jieli_chip chip=JM_JIELI_BR23;
  if(jieli) {
    if(!strcmp(argv[4],"BD19")) chip=JM_JIELI_BD19;
    else if(strcmp(argv[4],"BR23")) { free(image);fprintf(stderr,"Unknown Jieli chip\n");return 2; }
    loader=load(argv[6],&loader_size);
    if(!loader) { free(image);fprintf(stderr,"Cannot read RAM helper\n");return 1; }
  }
  const char *backup_path=argv[jieli?8:6];int fd=open_output(backup_path);struct backup_file file={0};
  if(fd>=0) { file.file=output_stream(fd);if(!file.file) close_fd(fd); }
  if(!file.file) { perror(backup_path);if(fd>=0) remove(backup_path);free(loader);free(image);return 1; }
  struct jm_platform_connection *connection=jm_platform_open(argv[3],jieli?JM_SESSION_JIELI:JM_SESSION_8BITDO);
  struct jm_restore_backup backup={save,finish,&file};unsigned last=(unsigned)-1;
  int result=-100;
  if(connection) result=jieli?jm_restore_jieli(jm_platform_io(),connection,chip,product,loader,loader_size,image,size,&backup,progress,&last):
                               jm_restore_8bitdo(jm_platform_io(),connection,0x301b,image,size,&backup,progress,&last);
  int restarted=0;
  if(!result && jieli) restarted=jm_session_jieli_start(jm_platform_io(),connection);
  jm_platform_close(connection);
  if(file.file) fclose(file.file);
  if(!file.written && !file.finished) remove(backup_path);
  if(result==-5 && jieli) fprintf(stderr,"Image chip key or capacity does not match the device; select a matching-key stock image. Stock rekeying is disabled.\n");
  if(result) fprintf(stderr,"Restore failed (%d); %s. Rescan before retrying.\n",result,
                     file.finished?"pre-restore backup saved; device may need recovery":"backup incomplete, no erase was authorized");
  else printf("Stock firmware written and verified. %s Backup: %s\n",jieli && !restarted?"Restart unconfirmed; USB reconnect may be needed.":"Firmware restart requested.",backup_path);
  free(loader);free(image);return result?1:0;
}
