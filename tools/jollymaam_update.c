#include "jollymaam_update.h"
#include "../src/lib/platform.h"
#include "../src/lib/manifest.h"
#include "../src/lib/transport_custom.h"
#include "../src/lib/transport_hid.h"
#include "../src/lib/transport_jieli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CUSTOM, BOOT8, JIELI };
struct device { char path[1024]; unsigned kind; enum jm_jieli_chip chip; struct jm_identity id; };
struct devices { struct device entries[64]; unsigned count; };
static struct device *add(struct devices *list,const char *path,unsigned kind) {
  if (list->count==64 || strlen(path)>=sizeof list->entries[0].path) return NULL;
  struct device *d=&list->entries[list->count++]; memset(d,0,sizeof *d);
  strcpy(d->path,path); d->kind=kind; return d;
}
static int custom(void *user,const char *path,const struct jm_identity *id) {
  struct device *d=add(user,path,CUSTOM); if(d) d->id=*id; return 0;
}
static int boot8(void *user,const char *path,unsigned vid,unsigned pid) {
  (void)vid; (void)pid;
  uint16_t model;
  if (!jm_hid_8bitdo_model(path,&model)) return 0;
  if (model==0x301b) { struct device *d=add(user,path,BOOT8); if(d) strcpy(d->id.product,"U2C_WIRELESS"); }
  else printf("Unsupported 8BitDo bootloader model %04x at %s\n",model,path);
  return 0;
}
static int jieli(void *user,const char *path,enum jm_jieli_chip chip) {
  struct device *d=add(user,path,JIELI); if(d) d->chip=chip; return 0;
}
static int line(char *buffer,size_t size) {
  if (!fgets(buffer,(int)size,stdin)) return 0;
  char *newline=strchr(buffer,'\n');
  if (!newline) {
    int c; while((c=getchar())!='\n' && c!=EOF) {}
    buffer[0]=0; return 1;
  }
  *newline=0; return 1;
}
static void progress(void *user,size_t done,size_t total) {
  unsigned *last=user; unsigned percent=total?(unsigned)(done*100/total):0;
  if(percent!=*last) { fprintf(stderr,"\rUpdating: %u%%",percent); *last=percent; }
}
/* Interactive terminal frontend. All discovery/probing is read-only. Jieli
 * candidates remain unidentified until the user chooses a target and confirms;
 * the driver uploads RAM code and verifies the installed product before erase.
 * Every operation requires a fresh, explicit yes. EOF/blank input never grants
 * approval. Re-scan after failures or mode changes; never auto-select a device. */
int jollymaam_update_app(const char *package_path) {
  FILE *file=fopen(package_path,"rb");
  if(!file) { perror(package_path); return 1; }
  long n;
  if(fseek(file,0,SEEK_END) || (n=ftell(file))<0 || n>32*1024*1024 || fseek(file,0,SEEK_SET)) { fclose(file); return 1; }
  uint8_t *data=malloc(n?(size_t)n:1);
  if(!data) { fclose(file); return 1; }
  int read_ok=fread(data,1,(size_t)n,file)==(size_t)n; fclose(file);
  struct jm_package package; struct jm_manifest manifest;
  if(!read_ok || jm_package_open(&package,data,(size_t)n)!=JM_PACKAGE_OK || !jm_manifest_open(&manifest,&package)) {
    fputs("Firmware package validation failed.\n",stderr); free(data); return 1;
  }
  printf("Verified firmware package: %u targets.\n",manifest.count);
  puts("Shows our HID tool devices and supported stock bootloaders. Stock gamepad modes are not yet discovered.");
  char input[128];
  for (;;) {
    struct devices list; memset(&list,0,sizeof list);
    if(jm_hid_custom_devices(custom,&list)<0) puts("Custom HID discovery unavailable.");
    if(jm_hid_enumerate_8bitdo(boot8,&list)<0) puts("8BitDo discovery unavailable.");
    if(jm_jieli_candidates(jieli,&list)<0) puts("Jieli discovery unavailable.");
    for(unsigned i=0;i<list.count;++i) {
      struct device *d=&list.entries[i];
      const struct jm_firmware_target *target=jm_manifest_target(&manifest,d->id.product);
      printf("%u. %s [%s]\n   %s\n",i+1,d->kind==JIELI?"Jieli stock bootloader (product not yet identified)":
        target?target->board:d->id.product,d->kind==CUSTOM?d->id.updater?"our updater":"our firmware":
        d->kind==BOOT8?"8BitDo bootloader":d->chip==JM_JIELI_BR23?"BR23":"BD19",d->path);
      if(d->kind==CUSTOM) printf("   Current build: %s\n",d->id.build);
    }
    if(!list.count) puts("No supported devices found. Check USB mode and access permissions.");
    printf("Select a device number, r to rescan, or q to quit: "); fflush(stdout);
    if(!line(input,sizeof input) || !strcmp(input,"q")) break;
    if(!strcmp(input,"r")) continue;
    char *end; unsigned long index=strtoul(input,&end,10);
    if(!*input || *end || index<1 || index>list.count) continue;
    struct device *d=&list.entries[index-1];
    const struct jm_firmware_target *target=jm_manifest_target(&manifest,d->id.product);
    if(d->kind==JIELI) {
      puts("Choose the intended product; it will be verified against device flash before erasing:");
      for(unsigned i=0;i<manifest.count;++i)
        if(!strcmp(manifest.targets[i].chip,d->chip==JM_JIELI_BR23?"BR23":"BD19"))
          printf("%u. %s\n",i+1,manifest.targets[i].board);
      printf("Target number (blank cancels): "); fflush(stdout);
      if(!line(input,sizeof input)) break;
      index=strtoul(input,&end,10);
      if(!*input || *end || index<1 || index>manifest.count) continue;
      target=&manifest.targets[index-1];
      if(strcmp(target->chip,d->chip==JM_JIELI_BR23?"BR23":"BD19")) continue;
    }
    if(!target) { puts("No firmware for this product in the package."); continue; }
    printf("Install package firmware for %s on %s?\nThis rewrites firmware%s. Type yes to continue: ",target->board,d->path,
           d->kind==CUSTOM && !d->id.updater?" and restarts the pad into its updater":""); fflush(stdout);
    if(!line(input,sizeof input)) break;
    if(strcmp(input,"yes")) { puts("Cancelled; no update requested."); continue; }
    unsigned last=(unsigned)-1; int result;
    struct jm_platform_connection *connection=jm_platform_open(d->path,(enum jm_session_kind)d->kind);
    result=connection?jm_session_run(target,(enum jm_session_kind)d->kind,d->id.uid,
                                      jm_platform_io(),connection,progress,&last):-2;
    jm_platform_close(connection);
    if(result) printf("\nUpdate failed (%d). Device may remain in its loader; rescan before retrying.\n",result);
    else puts(d->kind==JIELI?"\nWritten and verified. Device remains in its loader.":"\nWritten and verified. Firmware restart requested.");
  }
  free(data); return 0;
}
