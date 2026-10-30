#include "manifest.h"
#include "sha256.h"
#include "proto_jollymaam.h"
#include "proto_8bitdo.h"
#include "jieli_image/entry.h"
#include <string.h>

struct token { const uint8_t *text; size_t size; uint32_t number; unsigned end; char type; };
struct json { const uint8_t *p, *end; struct token tokens[512]; unsigned count; };
static void space(struct json *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) ++j->p;
}
static int value(struct json *j, unsigned depth) {
  space(j);
  if (depth > 16 || j->count == 512 || j->p == j->end) return 0;
  unsigned index = j->count++;
  struct token *t = &j->tokens[index]; memset(t, 0, sizeof *t);
  uint8_t c = *j->p++;
  t->type = (char)c;
  if (c == '"') {
    t->text = j->p;
    while (j->p < j->end && *j->p != '"') {
      if (*j->p < 32 || *j->p > 126 || *j->p == '\\') return 0;
      ++j->p;
    }
    if (j->p == j->end) return 0;
    t->size = (size_t)(j->p++ - t->text);
  } else if (c >= '0' && c <= '9') {
    t->type = 'n'; t->number = c - '0';
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
      if (c == '0' || t->number > (UINT32_MAX - (*j->p - '0')) / 10) return 0;
      t->number = t->number * 10 + *j->p++ - '0';
    }
  } else if (c == '{' || c == '[') {
    space(j);
    uint8_t close = c == '{' ? '}' : ']';
    if (j->p < j->end && *j->p == close) ++j->p;
    else for (;;) {
      if (c == '{') {
        unsigned key = j->count;
        if (!value(j, depth + 1) || j->tokens[key].type != '"') return 0;
        for (unsigned k = index + 1; k < key; k = j->tokens[k + 1].end)
          if (j->tokens[k].size == j->tokens[key].size &&
              !memcmp(j->tokens[k].text, j->tokens[key].text, j->tokens[k].size)) return 0;
        space(j); if (j->p == j->end || *j->p++ != ':') return 0;
      }
      if (!value(j, depth + 1)) return 0;
      space(j); if (j->p == j->end) return 0;
      if (*j->p == close) { ++j->p; break; }
      if (*j->p++ != ',') return 0;
    }
  } else return 0;
  t->end = j->count; return 1;
}
static int equals(const struct token *t, const char *s) {
  return t && t->type == '"' && t->size == strlen(s) && !memcmp(t->text, s, t->size);
}
static const struct token *field(struct json *j, const struct token *object, const char *key) {
  if (!object || object->type != '{') return NULL;
  unsigned i = (unsigned)(object - j->tokens) + 1;
  while (i < object->end) {
    if (equals(&j->tokens[i], key)) return &j->tokens[i+1];
    i = j->tokens[i+1].end;
  }
  return NULL;
}
static int string(const struct token *t, char *out, size_t size) {
  if (!t || t->type != '"' || t->size >= size) return 0;
  memcpy(out, t->text, t->size); out[t->size] = 0; return 1;
}
static int number(const struct token *t, uint32_t *out) {
  if (!t || t->type != 'n') return 0;
  *out = t->number; return 1;
}
static int artifact(struct json *j, const struct token *t, const struct jm_package *p, struct jm_package_entry *out) {
  char path[192], hex[65]; uint32_t size;
  if (!string(field(j,t,"path"),path,sizeof path) ||
      !string(field(j,t,"sha256"),hex,sizeof hex) || strlen(hex) != 64 ||
      !number(field(j,t,"size"),&size) || !jm_package_find(p,path,out) || out->size != size || !size) return 0;
  uint8_t digest[32]; jm_sha256(out->data,out->size,digest);
  const char digits[] = "0123456789abcdef";
  for (unsigned i=0;i<32;++i)
    if (hex[i*2] != digits[digest[i]>>4] || hex[i*2+1] != digits[digest[i]&15]) return 0;
  return 1;
}
/* Parse the version-1 manifest and verify every referenced artifact's size,
 * SHA-256 and image product. JSON is bounded to 512 tokens and depth 16; this
 * format accepts unsigned integers and unescaped printable ASCII strings.
 * Duplicate keys/products, unknown versions and malformed references fail.
 * Borrowed artifact views require the package buffer to remain unchanged.
 * Output is untouched on failure. No allocation or device I/O. This is not
 * signature verification. Preservation metadata is restricted to known policy. */
int jm_manifest_open(struct jm_manifest *out, const struct jm_package *package) {
  struct jm_package_entry manifest;
  if (!jm_package_find(package,"manifest.json",&manifest) || manifest.size > 32768) return 0;
  struct json j; j.p=manifest.data; j.end=manifest.data+manifest.size; j.count=0;
  if (!value(&j,0)) return 0;
  space(&j); if (j.p != j.end) return 0;
  uint32_t version;
  if (!equals(field(&j,j.tokens,"format"),"jollymaam-firmware") ||
      !number(field(&j,j.tokens,"version"),&version) || version != 1) return 0;
  const struct token *targets=field(&j,j.tokens,"targets");
  if (!targets || targets->type != '[') return 0;
  struct jm_manifest result; memset(&result,0,sizeof result);
  for (unsigned i=(unsigned)(targets-j.tokens)+1;i<targets->end;i=j.tokens[i].end) {
    if (result.count == 8) return 0;
    struct jm_firmware_target *t=&result.targets[result.count]; const struct token *object=&j.tokens[i];
    if (!string(field(&j,object,"product"),t->product,sizeof t->product) || !t->product[0] ||
        !string(field(&j,object,"board"),t->board,sizeof t->board) ||
        !string(field(&j,object,"chip"),t->chip,sizeof t->chip)) return 0;
    const char *expected_chip = !strcmp(t->product,"AC632N_TRANS") ? "BD19" :
      !strcmp(t->product,"U2C_WIRELESS") ? "TLSR8208" :
      (!strcmp(t->product,"GS_C2_ADC_DEVICE") || !strcmp(t->product,"GS_C2_Dongle") ||
       !strcmp(t->product,"AC695X_TRANS")) ? "BR23" : NULL;
    if (!expected_chip || strcmp(t->chip,expected_chip)) return 0;
    for (unsigned k=0;k<result.count;++k) if (!strcmp(result.targets[k].product,t->product)) return 0;
    const struct token *list=field(&j,object,"artifacts");
    if (!list || list->type != '[') return 0;
    unsigned roles=0;
    for (unsigned a=(unsigned)(list-j.tokens)+1;a<list->end;a=j.tokens[a].end) {
      const struct token *role=field(&j,&j.tokens[a],"role");
      unsigned bit=equals(role,"install")?1:equals(role,"update")?2:0;
      if (!bit || (roles & bit) || !artifact(&j,&j.tokens[a],package,bit==1?&t->install:&t->update)) return 0;
      roles|=bit;
    }
    if (roles != 3 || !jm_update_image_check(t->update.data,t->update.size,t->product)) return 0;
    if (!strcmp(t->chip,"TLSR8208")) {
      char product[17];
      if (strcmp(t->product,"U2C_WIRELESS") ||
          !number(field(&j,object,"model"),&t->model) || t->model != 0x301b ||
          !jm_8bitdo_image_product(t->install.data,t->install.size,product) || strcmp(product,t->product) ||
          field(&j,object,"loader")) return 0;
    } else {
      uint8_t header[32]; uint32_t base; char product[17];
      if ((strcmp(t->chip,"BR23") && strcmp(t->chip,"BD19")) ||
          !artifact(&j,field(&j,object,"loader"),package,&t->loader) ||
          !jm_jieli_find_base(t->install.data,t->install.size,header,&base)) return 0;
      jm_jieli_name_string(product,t->install.data+base+16);
      if (strcmp(product,t->product)) return 0;
    }
    list=field(&j,object,"preserve");
    if (!list || list->type != '[') return 0;
    unsigned first=(unsigned)(list-j.tokens)+1;
    if (!strcmp(t->product,"AC695X_TRANS")) {
      if (first==list->end || j.tokens[first].end != list->end ||
          !number(field(&j,&j.tokens[first],"offset"),&t->preserve_offset) ||
          !number(field(&j,&j.tokens[first],"size"),&t->preserve_size) ||
          t->preserve_offset != 0x64000 || t->preserve_size != 0x1c000) return 0;
    } else if (first != list->end) return 0;
    ++result.count;
  }
  if (!result.count) return 0;
  *out=result; return 1;
}
/* Select an exact product after successful manifest verification. Returned
 * target storage belongs to the manifest object; NULL means unsupported. */
const struct jm_firmware_target *jm_manifest_target(const struct jm_manifest *manifest, const char *product) {
  if (!manifest || !product) return NULL;
  for (unsigned i=0;i<manifest->count;++i) if (!strcmp(manifest->targets[i].product,product)) return &manifest->targets[i];
  return NULL;
}
