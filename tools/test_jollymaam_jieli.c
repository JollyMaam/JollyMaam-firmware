#include "../src/lib/jollymaam.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void simulate(const uint8_t *image, size_t size, int mode) {
  size_t capacity = (size + 8191) & ~(size_t)4095;
  uint8_t *workspace = malloc(capacity), *old = malloc(capacity + 4096), *scratch = malloc(size);
  assert(workspace && old && scratch);
  memset(old, 0x55, capacity + 4096);
  struct jm_jieli_image view;
  assert(jm_jieli_image_open(&view, image, size, scratch, size) == JM_JIELI_IMAGE_OK);
  char product[17]; jm_jieli_name_string(product, image + view.base + 16);
  struct jm_jieli_device device = {0, 0x400000, 512, view.key};
  struct jm_jieli_keep keep = {(uint32_t)size + 128, 64};
  struct jm_jieli_flash op;
  if (mode == 4) {
    assert(!jm_jieli_flash_init(&op, &device, "wrong", image, size, workspace, capacity, &keep, 1));
    device.key ^= 1;
    assert(!jm_jieli_flash_init(&op, &device, product, image, size, workspace, capacity, &keep, 1));
    goto done;
  }
  assert(jm_jieli_flash_init(&op, &device, product, image, size, workspace, capacity, &keep, 1));
  struct jm_jieli_request request;
  unsigned erases = 0, writes = 0, reads = 0, requests = 0;
  int preserved = 0;
  enum jm_jieli_step step;
  uint8_t reply[32768];
  while ((step = jm_jieli_flash_next(&op, &request)) == JM_JIELI_TRANSFER) {
    assert(++requests < 30000);
    unsigned command = (unsigned)request.cdb[0] << 8 | request.cdb[1];
    uint32_t at = rd32be(request.cdb + 2);
    assert(at < capacity);
    if (command == 0xfd05) {
      assert(at + request.size <= capacity);
      ++reads;
      memcpy(reply, old + at, request.size);
      if (!erases) {
        assert(at == keep.offset); preserved = 1;
        if (mode == 1) { jm_jieli_flash_complete(&op, 0, NULL, 0); continue; }
      } else if (mode == 3) reply[0] ^= 1;
      jm_jieli_flash_complete(&op, 1, reply, request.size);
    } else if (command == 0xfb00 || command == 0xfb01) {
      assert(preserved && reads && !writes);
      unsigned n = command == 0xfb00 ? 65536 : 4096;
      assert(!(at & (n - 1)) && at + n <= capacity);
      memset(old + at, 0xff, n); ++erases;
      memset(reply, 0, 16); reply[0] = request.cdb[0]; reply[1] = request.cdb[1];
      if (mode == 2) reply[1] ^= 1;
      jm_jieli_flash_complete(&op, 1, reply, 16);
    } else {
      assert(command == 0xfb04 && erases);
      ++writes;
      unsigned n = (unsigned)request.cdb[6] << 8 | request.cdb[7];
      assert(n == request.size && at + n <= capacity);
      assert(jm_crc16(request.data, n) == (unsigned)(request.cdb[9] | request.cdb[10] << 8));
      memcpy(old + at, request.data, n);
      jm_jieli_flash_complete(&op, 1, NULL, n);
    }
  }
  if (!mode) {
    assert(step == JM_JIELI_DONE && !memcmp(old, workspace, op.size));
    for (size_t i = keep.offset; i < keep.offset + keep.size; ++i) assert(old[i] == 0x55);
    for (size_t i = op.size; i < capacity + 4096; ++i) assert(old[i] == 0x55);
  } else {
    assert(step == JM_JIELI_FAILED);
    if (mode == 1) assert(!erases && !writes);
    if (mode == 2) assert(erases == 1 && !writes);
    if (mode == 3) assert(op.error == -4);
  }
done:
  free(workspace); free(old); free(scratch);
}
int main(int argc, char **argv) {
  size_t size; uint8_t *image;
  if (argc == 2) {
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n > 0); rewind(f);
    size = (size_t)n; image = malloc(size); assert(image);
    assert(fread(image, 1, size, f) == size); fclose(f);
  } else image = fixture(&size);
  for (int mode = 0; mode < 5; ++mode) simulate(image, size, mode);
  free(image); puts("Jieli flash simulation passed");
  return 0;
}
