#include "image.h"
#include "crypto.h"
#include "key.h"
#include "bankcb.h"
#include "../proto_jollymaam.h"
#include <string.h>

static int range(size_t size, uint64_t offset, uint64_t length) {
  return offset <= size && length <= size - offset;
}
static int named(const struct jm_jieli_entry *entry, const char *name) {
  char text[17]; jm_jieli_name_string(text, entry->name);
  return !strcmp(text, name);
}
/* Open a raw flash image into caller-owned workspace of at least size bytes.
 * The source remains unchanged; workspace must be distinct from source. All
 * top headers, BANKCB data and app encryption become clear in workspace.
 * Validate file CRCs, reserved-area bounds and resource chains before exposing
 * a view. On failure workspace is scratch and out is untouched. No allocation.
 * The view and its data remain valid until workspace is released or changed. */
enum jm_jieli_image_result jm_jieli_image_open(struct jm_jieli_image *out,
                                               const uint8_t *data, size_t size,
                                               uint8_t *workspace, size_t capacity) {
  if (!out || !data || !workspace || workspace == data || capacity < size || size > UINT32_MAX)
    return JM_JIELI_IMAGE_ARGUMENT;
  struct jm_jieli_image view;
  memset(&view, 0, sizeof view);
  view.data = workspace; view.size = size; view.key = 0xffff;
  uint8_t header[32];
  if (!jm_jieli_find_base(data, size, header, &view.base)) return JM_JIELI_IMAGE_HEADER;
  memcpy(workspace, data, size);
  int last = 0;
  for (size_t i = 0; i < 32; ++i) {
    uint64_t at = (uint64_t)view.base + 32 + i * 32;
    if (!range(size, at, 32)) return JM_JIELI_IMAGE_BOUNDS;
    uint8_t *h = workspace + at;
    jm_jieli_enc(h, 32, 0xffff);
    struct jm_jieli_entry e;
    if (!jm_jieli_decode_entry(h, &e)) return JM_JIELI_IMAGE_CRC;
    ++view.top_count;
    if (named(&e, "app_dir_head")) {
      uint64_t app = (uint64_t)view.base + e.offset;
      if (!range(size, app, 32)) return JM_JIELI_IMAGE_BOUNDS;
      view.appbase = (uint32_t)app;
    }
    if (!(e.flags & 0x10) && e.flags != 0x81) {
      uint64_t off = (uint64_t)view.base + e.offset;
      if (!range(size, off, e.size)) return JM_JIELI_IMAGE_BOUNDS;
      uint8_t *p = workspace + off;
      if (named(&e, "uboot.boot") && !jm_jieli_descramble_bankcb(p, e.size))
        return JM_JIELI_IMAGE_BANKCB;
      if (jm_crc16(p, e.size) != e.data_crc) return JM_JIELI_IMAGE_CRC;
      if (named(&e, "isd_config.ini")) {
        if (e.size < 34 || !jm_jieli_record_key(p, &view.key)) return JM_JIELI_IMAGE_CRC;
      }
    }
    if (e.index) { last = 1; break; }
  }
  if (!last || !view.appbase) return JM_JIELI_IMAGE_HEADER;
  uint8_t *plain = workspace + view.appbase;
  size_t n = size - view.appbase;
  view.cipher_size = (uint32_t)n;
  jm_jieli_sfc(plain, 0, (uint32_t)n, 0, view.key);
  struct jm_jieli_entry outer;
  if (!jm_jieli_decode_entry(plain, &outer)) return JM_JIELI_IMAGE_CRC;
  if (outer.size < 64 || outer.size > n) return JM_JIELI_IMAGE_BOUNDS;
  if (jm_crc16(plain + 32, outer.size - 32) != outer.data_crc) return JM_JIELI_IMAGE_CRC;
  last = 0;
  for (uint64_t at = 32; at + 32 <= outer.size; at += 32) {
    struct jm_jieli_entry e;
    if (!jm_jieli_decode_entry(plain + at, &e)) return JM_JIELI_IMAGE_CRC;
    if (e.flags != 0x82 && e.offset >= view.appbase &&
        e.offset - view.appbase < view.cipher_size)
      view.cipher_size = e.offset - view.appbase;
    if (e.flags == 0x82) {
      if (!range(outer.size, e.offset, e.size)) return JM_JIELI_IMAGE_BOUNDS;
      if (jm_crc16(plain + e.offset, e.size) != e.data_crc) return JM_JIELI_IMAGE_CRC;
    }
    if (e.index) { last = 1; break; }
  }
  if (!last) return JM_JIELI_IMAGE_HEADER;
  uint64_t resource_end = outer.size;
  if (!outer.index) {
    uint64_t at = outer.size;
    for (;;) {
      struct jm_jieli_entry e;
      if (!range(n, at, 32)) return JM_JIELI_IMAGE_BOUNDS;
      if (!jm_jieli_decode_entry(plain + at, &e) || (e.flags != 0x82 && e.flags != 0x83))
        return JM_JIELI_IMAGE_HEADER;
      if (e.offset < 32 || e.size < e.offset || !range(n, at, e.size)) return JM_JIELI_IMAGE_BOUNDS;
      if (jm_crc16(plain + at + e.offset, e.size - e.offset) != e.data_crc) return JM_JIELI_IMAGE_CRC;
      resource_end = at + e.size;
      if (e.index) break;
      at += e.size;
    }
  }
  if (view.cipher_size < resource_end || (view.cipher_size < n && (view.cipher_size & 31)))
    return JM_JIELI_IMAGE_BOUNDS;
  if (view.cipher_size < n)
    jm_jieli_sfc(plain, view.cipher_size, (uint32_t)n - view.cipher_size, 0, view.key);
  *out = view;
  return JM_JIELI_IMAGE_OK;
}
/* Repack an opened, unmodified view at the same layout with a chosen chip key.
 * This preserves every byte, including padding and reserved regions, when the
 * key is unchanged. Output must be distinct from the workspace. The source view
 * is not changed. On error output is scratch. This is not a layout builder:
 * arbitrary edits to the view or resizing files are not supported. */
enum jm_jieli_image_result jm_jieli_image_pack(const struct jm_jieli_image *image,
                                               uint16_t key, uint8_t *out,
                                               size_t capacity) {
  if (!image || !out || out == image->data || capacity < image->size ||
      image->size > UINT32_MAX || image->top_count > 32 ||
      !range(image->size, image->appbase, 32) ||
      !range(image->size, image->appbase, image->cipher_size)) return JM_JIELI_IMAGE_ARGUMENT;
  memcpy(out, image->data, image->size);
  int found_key = 0;
  for (size_t i = 0; i < image->top_count; ++i) {
    uint64_t at = (uint64_t)image->base + 32 + i * 32;
    if (!range(image->size, at, 32)) return JM_JIELI_IMAGE_BOUNDS;
    uint8_t *h = out + at;
    struct jm_jieli_entry e;
    if (!jm_jieli_decode_entry(h, &e)) return JM_JIELI_IMAGE_CRC;
    if (!(e.flags & 0x10) && e.flags != 0x81) {
      uint64_t off = (uint64_t)image->base + e.offset;
      if (!range(image->size, off, e.size)) return JM_JIELI_IMAGE_BOUNDS;
      uint8_t *p = out + off;
      if (named(&e, "isd_config.ini")) {
        if (e.size < 34) return JM_JIELI_IMAGE_BOUNDS;
        jm_jieli_set_record_key(p, key);
        e.data_crc = jm_crc16(p, e.size);
        jm_jieli_encode_entry(h, &e);
        found_key = 1;
      }
      if (named(&e, "uboot.boot") && !jm_jieli_scramble_bankcb(p, e.size))
        return JM_JIELI_IMAGE_BANKCB;
    }
    jm_jieli_enc(h, 32, 0xffff);
  }
  if (!found_key && key != image->key) return JM_JIELI_IMAGE_HEADER;
  jm_jieli_sfc(out, image->appbase, image->cipher_size, image->appbase, key);
  return JM_JIELI_IMAGE_OK;
}

/* Locate a valid sector-aligned MAIN app in the clear code region. Returned
 * bytes are the raw app, without its MAIN header. Output is unchanged if absent. */
int jm_jieli_image_main(const struct jm_jieli_image *image,struct jm_jieli_file *file,uint32_t *offset) {
  if(!image || !file || !offset || image->appbase>image->size || image->cipher_size>image->size-image->appbase) return 0;
  char product[17];if((uint64_t)image->base+32>image->size) return 0;
  jm_jieli_name_string(product,image->data+image->base+16);
  const uint8_t *plain=image->data+image->appbase;
  for(uint64_t at=0x1000;at+0x120<=image->cipher_size;at+=0x1000) {
    const uint8_t *h=plain+at;
    if(memcmp(h,"MAIN",4)) continue;
    uint32_t n=(uint32_t)h[4]|(uint32_t)h[5]<<8|(uint32_t)h[6]<<16|(uint32_t)h[7]<<24;
    uint32_t declared=(uint32_t)h[20]|(uint32_t)h[21]<<8|(uint32_t)h[22]<<16|(uint32_t)h[23]<<24;
    if(declared!=at || at+0x120+n>image->cipher_size || !jm_update_image_check(h,0x120+(size_t)n,product)) continue;
    struct jm_jieli_file result;memset(&result,0,sizeof result);strcpy(result.name,"_main.bin");
    result.data=h+0x120;result.size=n;result.kind=JM_JIELI_FILE_MAIN;*file=result;*offset=(uint32_t)at;return 1;
  }
  return 0;
}
static int emit_file(jm_jieli_file_visitor visit, void *user,
                     const struct jm_jieli_entry *entry, const uint8_t *data,
                     size_t size, enum jm_jieli_file_kind kind) {
  struct jm_jieli_file file;
  jm_jieli_name_string(file.name, entry->name);
  file.data = data; file.size = size; file.kind = kind;
  return visit(user, &file);
}
/* Visit decoded top-level files, application files and resources in a view
 * returned by open. Reserved flash areas and directory records are omitted.
 * File data borrows workspace; each descriptor lives only during the callback.
 * Return zero from the callback to continue, nonzero to stop. isd_config.ini is
 * the binary configuration stream, not the desktop tool's reconstructed INI.
 * The callback must not modify the view or its workspace. No filesystem I/O. */
enum jm_jieli_image_result jm_jieli_image_visit(const struct jm_jieli_image *image,
                                                jm_jieli_file_visitor visit, void *user) {
  if (!image || !visit || !image->data || image->top_count > 32 ||
      !range(image->size, image->appbase, image->cipher_size) || image->cipher_size < 32)
    return JM_JIELI_IMAGE_ARGUMENT;
  for (size_t i = 0; i < image->top_count; ++i) {
    uint64_t at = (uint64_t)image->base + 32 + i * 32;
    struct jm_jieli_entry e;
    if (!range(image->size, at, 32)) return JM_JIELI_IMAGE_BOUNDS;
    if (!jm_jieli_decode_entry(image->data + at, &e)) return JM_JIELI_IMAGE_CRC;
    if (!(e.flags & 0x10) && e.flags != 0x81) {
      uint64_t off = (uint64_t)image->base + e.offset;
      if (!range(image->size, off, e.size)) return JM_JIELI_IMAGE_BOUNDS;
      if (emit_file(visit, user, &e, image->data + off, e.size, JM_JIELI_FILE_TOP))
        return JM_JIELI_IMAGE_VISITOR;
    }
  }
  const uint8_t *plain = image->data + image->appbase;
  struct jm_jieli_entry outer;
  if (!jm_jieli_decode_entry(plain, &outer)) return JM_JIELI_IMAGE_CRC;
  if (outer.size < 64 || outer.size > image->cipher_size) return JM_JIELI_IMAGE_BOUNDS;
  int last = 0;
  for (uint64_t at = 32; at + 32 <= outer.size; at += 32) {
    struct jm_jieli_entry e;
    if (!jm_jieli_decode_entry(plain + at, &e)) return JM_JIELI_IMAGE_CRC;
    if (e.flags == 0x82) {
      if (!range(outer.size, e.offset, e.size)) return JM_JIELI_IMAGE_BOUNDS;
      if (emit_file(visit, user, &e, plain + e.offset, e.size, JM_JIELI_FILE_APP))
        return JM_JIELI_IMAGE_VISITOR;
    }
    if (e.index) { last = 1; break; }
  }
  if (!last) return JM_JIELI_IMAGE_HEADER;
  if (!outer.index) {
    uint64_t at = outer.size;
    for (;;) {
      struct jm_jieli_entry e;
      if (!range(image->cipher_size, at, 32)) return JM_JIELI_IMAGE_BOUNDS;
      if (!jm_jieli_decode_entry(plain + at, &e)) return JM_JIELI_IMAGE_CRC;
      if (e.offset < 32 || e.size < e.offset || !range(image->cipher_size, at, e.size))
        return JM_JIELI_IMAGE_BOUNDS;
      if (emit_file(visit, user, &e, plain + at + e.offset, e.size - e.offset, JM_JIELI_FILE_RESOURCE))
        return JM_JIELI_IMAGE_VISITOR;
      if (e.index) break;
      at += e.size;
    }
  }
  struct jm_jieli_file main;uint32_t main_at;
  if(jm_jieli_image_main(image,&main,&main_at) && visit(user,&main)) return JM_JIELI_IMAGE_VISITOR;
  return JM_JIELI_IMAGE_OK;
}

#ifdef UNITTEST
#include <assert.h>
int main(void) {
  uint8_t data[256] = {0}, workspace[256];
  struct jm_jieli_image view;
  assert(jm_jieli_image_open(&view, data, sizeof data, data, sizeof data) == JM_JIELI_IMAGE_ARGUMENT);
  assert(jm_jieli_image_open(&view, data, sizeof data, workspace, 255) == JM_JIELI_IMAGE_ARGUMENT);
  assert(jm_jieli_image_open(&view, data, sizeof data, workspace, sizeof workspace) == JM_JIELI_IMAGE_HEADER);
  data[2] = 1;
  uint16_t crc = jm_crc16(data + 2, 30);
  data[0] = crc; data[1] = crc >> 8;
  jm_jieli_enc(data, 32, 0xffff);
  struct jm_jieli_entry top = {0};
  top.offset = UINT32_MAX;
  top.flags = 0x81; top.index = 1;
  memcpy(top.name, "app_dir_head", 13);
  jm_jieli_encode_entry(data + 32, &top);
  jm_jieli_enc(data + 32, 32, 0xffff);
  assert(jm_jieli_image_open(&view, data, sizeof data, workspace, sizeof workspace) == JM_JIELI_IMAGE_BOUNDS);
  return 0;
}
#endif
