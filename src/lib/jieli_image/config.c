#include "config.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_FILES 32
struct text { char *p;size_t n,cap;int failed; };
static void emit(struct text *t,const char *format,...) {
  if(t->failed) return;
  va_list args,copy;va_start(args,format);va_copy(copy,args);
  int n=vsnprintf(NULL,0,format,copy);va_end(copy);
  if(n<0 || (size_t)n>128*1024-t->n) { t->failed=1;va_end(args);return; }
  size_t required=t->n+(size_t)n+1;
  if(required>t->cap) {
    size_t cap=required<4096?4096:required*2;
    char *p=realloc(t->p,cap);if(!p) { t->failed=1;va_end(args);return; }
    t->p=p;t->cap=cap;
  }
  vsnprintf(t->p+t->n,t->cap-t->n,format,args);t->n+=(size_t)n;va_end(args);
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int named(const struct jm_jieli_entry *e,const char *name) {
  char text[17];jm_jieli_name_string(text,e->name);return !strcmp(text,name);
}
/* Reconstruct a packable INI from an opened clear image view. No filesystem
 * access occurs. source_name is used only in a comment and must be printable.
 * The returned text is malloc-owned, NUL terminated; size excludes the NUL.
 * Output pointers are untouched on failure. Main apps are exported separately
 * as _main.bin by the file visitor and named in the reconstructed INI. */
int jm_jieli_image_config(const struct jm_jieli_image *image,const char *source_name,char **out,size_t *out_size) {
  if(!image || !out || !out_size || !image->data || image->top_count>MAX_FILES ||
      image->appbase>image->size || image->cipher_size>image->size-image->appbase || image->cipher_size<64) return 0;
  if(!source_name) source_name="image";
  for(const char *p=source_name;*p;++p) if((unsigned char)*p<32 || (unsigned char)*p>126) return 0;
  struct text text={0};uint8_t fh[32];uint32_t base=image->base;
  if(!jm_jieli_header_at(image->data,image->size,base,fh)) return 0;
  memcpy(fh+4,image->data+base+4,4);memcpy(fh+16,image->data+base+16,16);
  char pid[17],vid[5]={0};jm_jieli_name_string(pid,fh+16);memcpy(vid,fh+4,4);
  const uint8_t *sys=NULL;uint32_t sys_n=0,flash_end=0;uint16_t chipkey=image->key,mark=0xffff;
  char uboot_name[17]="";uint8_t top_uboot_flags=0;
  for(size_t i=0;i<image->top_count;++i) {
    uint64_t at=(uint64_t)base+32+i*32;struct jm_jieli_entry e;
    if(at+32>image->size || !jm_jieli_decode_entry(image->data+at,&e)) goto fail;
    if(named(&e,"uboot.boot")) { strcpy(uboot_name,"uboot.boot");top_uboot_flags=e.flags; }
    if(named(&e,"app_dir_head")) mark=e.data_crc;
    if(named(&e,"key_mac")) { if((uint64_t)e.offset+e.size>UINT32_MAX) goto fail;flash_end=e.offset+e.size; }
    if(named(&e,"isd_config.ini")) {
      if((uint64_t)base+e.offset+e.size>image->size || e.size<34) goto fail;
      sys=image->data+base+e.offset;sys_n=e.size;
    }
  }
  const uint8_t *plain=image->data+image->appbase;struct jm_jieli_entry outer;
  if(!jm_jieli_decode_entry(plain,&outer) || outer.size<64 || outer.size>image->cipher_size) goto fail;
  char files[MAX_FILES][17],res[MAX_FILES][17];uint8_t res_flags[MAX_FILES];
  size_t nfiles=0,nres=0,nareas=0;struct jm_jieli_entry areas[MAX_FILES];
  uint32_t code=0,vm_len=0,btif_len=0,vm_at=0;int last=0;
  for(uint64_t off=32;off+32<=outer.size;off+=32) {
    struct jm_jieli_entry e;if(!jm_jieli_decode_entry(plain+off,&e)) goto fail;
    if(named(&e,"PRCT")) code=e.size;
    if(named(&e,"VM")) { vm_len=rd32(e.name+12);vm_at=e.offset; }
    if(named(&e,"BTIF")) btif_len=e.size;
    if(e.flags==0x82) {
      if(nfiles==MAX_FILES) goto fail;
      jm_jieli_name_string(files[nfiles++],e.name);
    } else { if(nareas==MAX_FILES) goto fail;areas[nareas++]=e; }
    if(e.index) { last=1;break; }
  }
  if(!last) goto fail;
  if(!outer.index) {
    uint64_t off=outer.size;
    for(;;) {
      struct jm_jieli_entry e;
      if(off+32>image->cipher_size || !jm_jieli_decode_entry(plain+off,&e) ||
          e.offset<32 || e.size<e.offset || e.size>image->cipher_size-off || nres==MAX_FILES) goto fail;
      res_flags[nres]=e.flags;jm_jieli_name_string(res[nres++],e.name);
      if(e.index) break;
      off+=e.size;
    }
  }
    emit(&text, "// From %s by jollymaam jieli unpack: jollymaam jieli pack -c this_file -o OUT\n"
               "// rebuilds it ([JLNEWFW] holds what the SDK INI cannot say).\n",
            source_name);
    emit(&text, "[EXTRA_CFG_PARAM]\nNEW_FLASH_FS = YES;\nFORCE_4K_ALIGN = YES;\n"
               "SPECIAL_OPT = 0;\nPID = %s;\nVID = %s;\nENTRY = 0x%x;\n"
               "RESERVED_OPT = 0;\n[SYS_CFG_PARAM]\n",
            pid, vid, outer.offset);
    /* After the 34-byte chip-key record: a length (4 for a number), the
     * name, the value.  A 4-character string and a number look the same;
     * printable is taken for a string. */
    for (uint32_t i = 34; sys && i < sys_n;) {
      uint8_t len = sys[i++];
      const char *key = (const char *)sys + i;
      size_t kn=0;while(kn<sys_n-i && key[kn]) ++kn;
      if(kn==sys_n-i) goto fail;
      i += (uint32_t)kn + 1;
      if (i + len > sys_n)
        goto fail;
      bool printable = true;
      for (unsigned j = 0; j < len; j++)
        if (!isalnum(sys[i + j]) && sys[i + j] != '_')
          printable = false;
      if (len == 4 && !printable)
        emit(&text, "%.*s = %u;\n", (int)kn, key, rd32(sys + i));
      else
        emit(&text, "%.*s = %.*s;\n", (int)kn, key, len, sys + i);
      i += len;
    }
    emit(&text, "[RESERVED_CONFIG]\nPRCT_ADR = 0;\nPRCT_LEN = CODE_LEN;\n"
               "PRCT_OPT = 2;\nVM_ADR = 0;\nVM_LEN = 0x%x;\nVM_OPT = 1;\n"
               "BTIF_ADR = AUTO;\nBTIF_LEN = 0x%x;\nBTIF_OPT = 1;\n[JLNEWFW]\n",
            vm_len, btif_len);
    emit(&text, "UBOOT = %s;\nUBOOT_FLAGS = 0x%02x;\nHEADER_FS = %u;\nAREAS = ", uboot_name,
            top_uboot_flags, fh[12]);
    for (size_t i = 0; i < nareas; i++) {
      emit(&text, "%s0x%x:0x%x:0x%02x:0x%02x:0x%04x:", i ? ", " : "", areas[i].offset,
              areas[i].size, areas[i].flags, areas[i].reserved, areas[i].data_crc);
      for (unsigned k = 0; k < 16; k++)
        emit(&text, "%02x", areas[i].name[k]);
    }
    emit(&text, ";\nFILES = ");
    for (size_t i = 0; i < nfiles; i++)
      emit(&text, "%s%s", i ? ", " : "", files[i]);
    emit(&text, ";\n");
    if (nres) {
      emit(&text, "RES = ");
      for (size_t i = 0; i < nres; i++)
        emit(&text, "%s%s%s", i ? ", " : "", res[i], res_flags[i] == 0x82 ? ":82" : "");
      emit(&text, ";\n");
    }
    if (!code && vm_at > base) /* no PRCT entry (BD19): the code ends where VM starts */
      code = vm_at - base;
    emit(&text, "FLASH_SIZE = 0x%x;\nCODE_LIMIT = 0x%x;\nHEADER_SPECIAL = 0x%02x;\n"
               "CHIP_KEY = 0x%04x;\nCHIP_KEY_RECORD = ",
            flash_end, code, fh[15], chipkey);
    for (unsigned i = 0; sys && i < 34; i++)
      emit(&text, "%02x", sys[i]);
    emit(&text, ";\n");
    if (mark != 0xffff)
      emit(&text, "MARK = 0x%04x;\n", mark);
  struct jm_jieli_file main;uint32_t main_at;
  if(jm_jieli_image_main(image,&main,&main_at)) emit(&text,"MAIN_FILE = _main.bin;\nMAIN_AT = 0x%x;\n",main_at);
  if(text.failed) goto fail;
  *out=text.p;*out_size=text.n;return 1;
fail:
  free(text.p);return 0;
}
