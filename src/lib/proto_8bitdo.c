#include "proto_8bitdo.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
uint16_t jm_8bitdo_crc16(const uint8_t *data, size_t size) {
  uint16_t crc = 0xffff;
  while (size--) {
    crc ^= *data++;
    for (unsigned i = 0; i < 8; ++i)
      crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
  }
  return crc;
}
/* Pure packet interfaces, usable with blocking or asynchronous transports.
 * The 64-byte request excludes the synthetic zero report ID hidapi needs.
 * For program, length is payload size. For read/identify it is requested size.
 * Erase/start take length zero. Program payload must remain valid only until
 * encode returns. Output is untouched on invalid arguments. */
enum jm_8bitdo_result jm_8bitdo_request(uint8_t out[64], uint16_t command,
                                        uint32_t address, const uint8_t *payload,
                                        size_t length) {
  if (length > JM_8BITDO_CHUNK ||
      (command != 3 && command != 4 && command != 5 && command != 7 && command != 8) ||
      (command == 3 && (!payload || !length)) ||
      ((command == 4 || command == 7) && length) ||
      (command != 3 && payload))
    return JM_8BITDO_ARGUMENT;
  memset(out, 0, 64);
  out[0] = 5;
  wr16(out + 1, command);
  wr16(out + 5, (uint16_t)length);
  wr16(out + 7, command == 3 ? jm_8bitdo_crc16(payload, length) : 0);
  wr32(out + 9, command == 3 || command == 4 ? JM_8BITDO_SECTOR : (uint32_t)length);
  wr32(out + 13, address);
  if (command == 3)
    memcpy(out + 17, payload, length);
  return JM_8BITDO_OK;
}
/* Replies include the leading zero byte as returned by the existing loader.
 * Return a borrowed payload view valid as long as report. On error outputs
 * are untouched. STALE means a read reply for another address; keep waiting.
 * Reply CRC is not enforced: its semantics are not established by the current
 * reference implementation. */
enum jm_8bitdo_result jm_8bitdo_reply(const uint8_t *report, size_t size,
                                      uint16_t command, uint32_t address,
                                      const uint8_t **payload, size_t *length) {
  if (size < 18 || report[0] != 0 || report[1] != 5)
    return JM_8BITDO_FRAMING;
  if (rd16(report + 2))
    return JM_8BITDO_STATUS;
  if (rd16(report + 4) != command)
    return JM_8BITDO_COMMAND;
  if (command == 5 && rd32(report + 14) != address)
    return JM_8BITDO_STALE;
  size_t n = rd16(report + 6);
  if (n > JM_8BITDO_CHUNK || n > size - 18)
    return JM_8BITDO_LENGTH;
  *payload = report + 18;
  *length = n;
  return JM_8BITDO_OK;
}
/* Validate the packed Telink image's signature, declared size and CRC32.
 * Trailing bytes are allowed for the full updater/application image. */
int jm_8bitdo_image_check(const uint8_t *image, size_t size) {
  if (size < 32 || memcmp(image + 8, "KNLT", 4))
    return 0;
  uint32_t n = rd32(image + 24);
  if (n < 8 || n > size)
    return 0;
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n - 4; ++i) {
    crc ^= image[i];
    for (unsigned j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return crc == rd32(image + n - 4);
}
/* Validate an extracted stock app or a vendor DAT container. DAT has a
 * 28-byte header: load address, payload length, model and two reserved words.
 * Check model 301b, load address 6000, reserved zeros, exact payload bounds,
 * zero padding and packed app CRC. The header check word's semantics are not
 * established; it is not treated as authentication. Raw apps have no model
 * tag, so model is the caller's independent assertion for those inputs.
 * Returned bytes borrow input; outputs are unchanged on failure. */
int jm_8bitdo_stock_image(const uint8_t *input,size_t size,uint16_t model,const uint8_t **image,size_t *image_size) {
  if(!input || !image || !image_size || model!=0x301b) return 0;
  const uint8_t *app=input;size_t available=size;int dat=0;
  if(size>=60 && memcmp(input+8,"KNLT",4) && !memcmp(input+36,"KNLT",4)) {
    if(rd32(input+4)!=0x6000 || rd32(input+8)!=size-28 || rd32(input+12)!=model ||
       rd32(input+20) || rd32(input+24)) return 0;
    app=input+28;available=size-28;dat=1;
  }
  if(!jm_8bitdo_image_check(app,available)) return 0;
  size_t n=rd32(app+24);
  if(n<32 || n>0x6f000-0x6000 || (!dat && n!=available)) return 0;
  for(size_t i=n;i<available;++i) if(app[i]) return 0;
  *image=app;*image_size=n;return 1;
}
/* Locate the product in the sector-aligned MAIN header after the updater.
 * Returns a NUL-terminated product string in out[17], untouched on failure. */
int jm_8bitdo_image_product(const uint8_t *image, size_t size, char out[17]) {
  if (!jm_8bitdo_image_check(image, size))
    return 0;
  uint64_t at = ((uint64_t)rd32(image + 24) + 4095) & ~(uint64_t)4095;
  for (; at + 40 <= size; at += 4096) {
    if (!memcmp(image + at, "MAIN", 4)) {
      memcpy(out, image + at + 24, 16);
      out[16] = 0;
      return 1;
    }
  }
  return 0;
}
#ifdef UNITTEST
#include <assert.h>
int main(void) {
  uint8_t request[64], reply[64] = {0, 5}, data[] = {1, 2, 3};
  assert(jm_8bitdo_crc16((const uint8_t *)"123456789", 9) == 0x4b37);
  assert(jm_8bitdo_request(request, 3, 0x12345678, data, 3) == JM_8BITDO_OK);
  assert(request[1] == 3 && request[5] == 3 && request[10] == 0x10);
  assert(request[13] == 0x78 && request[16] == 0x12);
  assert(!memcmp(request + 17, data, 3));
  assert(jm_8bitdo_request(request, 3, 0, data, 47) == JM_8BITDO_ARGUMENT);
  assert(jm_8bitdo_request(request, 4, 0x6000, NULL, 0) == JM_8BITDO_OK);
  const uint8_t *payload = NULL;
  size_t length = 99;
  reply[4] = 5; reply[6] = 3; reply[14] = 1;
  assert(jm_8bitdo_reply(reply, 64, 5, 0, &payload, &length) == JM_8BITDO_STALE);
  assert(payload == NULL && length == 99);
  reply[14] = 0;
  assert(jm_8bitdo_reply(reply, 20, 5, 0, &payload, &length) == JM_8BITDO_LENGTH);
  assert(jm_8bitdo_reply(reply, 64, 5, 0, &payload, &length) == JM_8BITDO_OK);
  assert(length == 3 && payload == reply + 18);
  return 0;
}
#endif
