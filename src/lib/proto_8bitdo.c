#include "proto_8bitdo.h"
#include "sha256.h"
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
  return jm_8bitdo_request_total(out, command, address, payload, length,
                                 command == 3 || command == 4 ? JM_8BITDO_SECTOR : (uint32_t)length);
}
/* As jm_8bitdo_request with the header's total given: the Telink loader
 * takes a sector for program and erase, the Yichip one the whole stream's
 * length. */
enum jm_8bitdo_result jm_8bitdo_request_total(uint8_t out[64], uint16_t command,
                                              uint32_t address, const uint8_t *payload,
                                              size_t length, uint32_t total) {
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
  wr32(out + 9, total);
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
/* The Yichip loader's transfer cipher: what program data must be, the
 * loader decrypts it before it programs the flash.  Words of 32 bits in
 * blocks of 512 bytes, the block counted from the stream's start; each word
 * XOR a key from the block and its position, chained to the previous
 * ciphertext word of the block.
 *
 * The seed table and the schedule are from 8CryptDo (AeroModes,
 * https://github.com/aeromodes/8cryptdo, commit aaffcebafa48), as
 * alphaorderly's ultimate-bluetooth-updater applies them; its licence:
 *
 *   MIT License
 *
 *   Copyright (c) 2026 AeroModes
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE. */
static const uint16_t yichip_seeds[256] = {
  0xf428, 0x50c3, 0x3071, 0x722a, 0x848d, 0x0dd7, 0x5b3e, 0x56ef,
  0x3ad3, 0x510a, 0x0147, 0x417a, 0x886c, 0x12ae, 0x3c9c, 0x5dd3,
  0x2d6e, 0x615b, 0xd68b, 0x5bcc, 0xd3e3, 0x2d48, 0xc15b, 0xa342,
  0x5e54, 0x4d90, 0xaf30, 0x7a74, 0x05a8, 0x5e0b, 0x53b4, 0xc56a,
  0xdb56, 0x6f88, 0x367e, 0x3814, 0x4e53, 0x184e, 0x2227, 0x0024,
  0x7705, 0x4fd0, 0x42ae, 0x71c5, 0x5004, 0xbff0, 0x1e71, 0x3330,
  0xf02a, 0x470b, 0x4142, 0x2ea0, 0x56ec, 0x424f, 0x7982, 0x1361,
  0xd7c3, 0x0dc8, 0x1552, 0x5537, 0xa2b9, 0x4e96, 0x7243, 0xa00a,
  0xc620, 0xed93, 0xe24f, 0x82dc, 0x0be4, 0x240b, 0x7118, 0xdd34,
  0xf0d2, 0xe406, 0x7f07, 0xf800, 0x327b, 0xdb9c, 0x547a, 0xfcc2,
  0x8835, 0x539a, 0x320d, 0xf492, 0xe6ec, 0x26bc, 0x6fc3, 0xa9c8,
  0x00e9, 0xc11b, 0xd224, 0x199a, 0x2484, 0x1f47, 0xce36, 0x2f14,
  0x9972, 0x6801, 0x6c86, 0x5c33, 0x8274, 0x0e1f, 0xf5f6, 0x885f,
  0x02b3, 0x9605, 0x39bc, 0xdbbd, 0x9e2d, 0xed7d, 0x5968, 0xe436,
  0xa8dd, 0x1981, 0x5b9a, 0x93f4, 0x91d5, 0x4cbe, 0x5cfc, 0x8fc3,
  0x1484, 0x101c, 0x4251, 0x17bf, 0xfb91, 0xf156, 0x0b4a, 0x2205,
  0x949f, 0x1e4e, 0xc9f0, 0xb66c, 0xaccf, 0x5c44, 0xe3bb, 0x5bfd,
  0xf3ec, 0xa47c, 0x6bc8, 0x46b3, 0xb5cd, 0xd223, 0x7022, 0x046f,
  0xb7c8, 0x0b51, 0xd416, 0x5041, 0x95d1, 0xb7f0, 0x456f, 0xec16,
  0xe119, 0x8bd4, 0x43ad, 0xc04f, 0x16b2, 0x802d, 0xd7e2, 0x16ac,
  0x833f, 0x1660, 0x993e, 0xd4f4, 0x9ac7, 0xe9a1, 0x12a5, 0x0504,
  0x931e, 0xecc2, 0x157d, 0xe013, 0x8562, 0x8693, 0x9237, 0xd7f7,
  0xc59d, 0xeac2, 0x7cd0, 0xba88, 0x5bd1, 0x09b5, 0x9c15, 0xacc0,
  0xaa02, 0x0fde, 0x434d, 0xe9e5, 0x8be7, 0x601e, 0x8b55, 0x2e39,
  0x1c1d, 0x0cdc, 0x1e81, 0xe405, 0x82af, 0xf6c2, 0x53e9, 0x3e0a,
  0x09dd, 0x173e, 0xa207, 0xd23d, 0x5ddf, 0x182e, 0x1a62, 0xc02e,
  0xedb9, 0x0d6a, 0x6687, 0x37b4, 0x1edc, 0xeacd, 0xc515, 0x495b,
  0x39b2, 0x9972, 0x9e49, 0xac4e, 0xd364, 0x96b7, 0xbbb5, 0x412e,
  0x8bb7, 0xa647, 0x3a9c, 0x93cf, 0x0ad3, 0x388c, 0x9c1c, 0x8268,
  0x0bcd, 0xc814, 0x6521, 0x608a, 0x7120, 0xdbca, 0xd2d0, 0xfa37,
  0xc9a5, 0x56c8, 0x2066, 0xf8c9, 0x1872, 0x352b, 0x1908, 0x991e,
  0xa478, 0x45a3, 0x0e8c, 0x2486, 0xe293, 0x8d55, 0x2912, 0x2129,
};
static const uint8_t yichip_index[2][8] = {
  {0, 0x82, 0x83, 0x81, 3, 0x81, 0x83, 0x81},
  {0x86, 0x83, 0x81, 0x82, 3, 5, 0x82, 0x82},
};
static uint32_t ror32(uint32_t v, unsigned n) {
  n &= 31;
  return n ? v >> n | v << (32 - n) : v;
}
/* Block block of the stream (512 bytes) encrypted: plain and out may be
 * the same. */
void jm_8bitdo_yichip_encrypt(uint32_t block, const uint8_t plain[512], uint8_t out[512]) {
  uint32_t e = (block + (block >> 8)) & 0xff, other = e ^ 0xf9, previous = 0;
  unsigned hi = e ^ yichip_index[e >> 7][e & 7], lo = other ^ yichip_index[other >> 7][other & 7];
  uint32_t seed = (uint32_t)yichip_seeds[hi] << 16 | yichip_seeds[lo];
  for (uint32_t position = 0; position < 128; ++position) {
    uint32_t counter = block * 0x01200210u + (block >> 8) + position * 0xee97fafau;
    uint32_t key = counter ^ ror32(seed, 16 * position);
    uint32_t word = rd32(plain + 4 * position) ^ key ^ (position ? ror32(previous, 18) : 0);
    wr32(out + 4 * position, word);
    previous = word;
  }
}
/* An app from objcopy (a Telink image, "KNLT" at 8) as the boot loader
 * starts it: stamp in header bytes 2..7, padded with 0xff to 16 bytes, its
 * size (with the CRC) at 0x18, then the CRC-32 without its final inversion
 * of it all.  Into out (room bytes); 0 when it is no Telink app or does not
 * fit. */
int jm_8bitdo_telink_pack(const uint8_t *app, size_t size, const uint8_t stamp[6],
                          uint8_t *out, size_t room, size_t *out_size) {
  if (!app || !out || !out_size || size < 0x20 || memcmp(app + 8, "KNLT", 4))
    return 0;
  size_t n = (size + 15) & ~(size_t)15;
  if (n + 4 > room || n + 4 > 0xffffffffu)
    return 0;
  memmove(out, app, size);
  memset(out + size, 0xff, n - size);
  memcpy(out + 2, stamp, 6);
  wr32(out + 0x18, (uint32_t)(n + 4));
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < n; ++i) {
    crc ^= out[i];
    for (unsigned j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  wr32(out + n, crc);
  *out_size = n + 4;
  return 1;
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
/* The U2CB's stock app, v1.01 (the only one known): its 0x18000 bytes from
 * 0x01018000, alone or inside a loader dump from 0x01017000 (jollymaam backup,
 * or the stock-firmware repository's).  Raw apps carry no tag or CRC, so the
 * bytes must match v1.01's SHA-256.  Returned bytes borrow input. */
#define YICHIP_STOCK_SIZE 0x18000u
static const uint8_t yichip_stock_v101[32] = {
  0x95, 0x5a, 0x74, 0xa2, 0xaa, 0x5d, 0x73, 0x1e, 0xff, 0x85, 0xbb, 0x0f, 0x1b, 0x7a, 0xca, 0xf6,
  0xf6, 0xb9, 0x71, 0x9e, 0x52, 0x0c, 0xc3, 0xf6, 0x8a, 0x41, 0x91, 0x8a, 0xf0, 0x9b, 0x04, 0xae};
int jm_8bitdo_yichip_stock_app(const uint8_t *input, size_t size, const uint8_t **app, size_t *app_size) {
  uint8_t digest[32];
  size_t at = size == YICHIP_STOCK_SIZE ? 0 : size == 0x01080000u - 0x01017000u ? 0x1000 : (size_t)-1;
  if (!input || !app || !app_size || at == (size_t)-1) return 0;
  jm_sha256(input + at, YICHIP_STOCK_SIZE, digest);
  if (memcmp(digest, yichip_stock_v101, 32)) return 0;
  *app = input + at;
  *app_size = YICHIP_STOCK_SIZE;
  return 1;
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
  {
    static uint8_t app[0x18000];
    const uint8_t *got = NULL;
    size_t n = 0;
    assert(!jm_8bitdo_yichip_stock_app(app, sizeof app, &got, &n) && !got && !n);   /* not v1.01 */
    assert(!jm_8bitdo_yichip_stock_app(app, sizeof app - 1, &got, &n));
  }
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
  assert(jm_8bitdo_request_total(request, 4, 0x01018000, NULL, 0, 0x18000) == JM_8BITDO_OK);
  assert(rd32(request + 9) == 0x18000 && rd32(request + 13) == 0x01018000);
  /* The cipher, against pad8cryptdo.transform (proven on the pad): bytes
   * (i * 7 + 3) & 255 of the stream. */
  {
    static const struct { uint32_t block; uint8_t head[16]; } want[] = {
      {0, {0x52,0x48,0x39,0xec,0xc3,0x93,0xff,0xca,0x21,0x07,0xbe,0x9d,0xfe,0x3d,0x3b,0x64}},
      {1, {0x59,0x03,0x5d,0xaf,0xae,0x06,0x06,0x90,0x74,0x15,0xc1,0xb9,0xb5,0x34,0x95,0xee}},
      {2, {0x3f,0x1e,0x1f,0x04,0x4c,0x06,0x69,0x13,0xe9,0xac,0xfb,0xd0,0x29,0xc1,0x44,0x59}},
      {300, {0x99,0x29,0xf1,0xcc,0xfa,0xb5,0x0a,0x77,0x17,0xf6,0xe7,0x56,0x23,0x55,0xf1,0xc5}},
    };
    static const uint8_t tail0[16] = {0xca,0xb1,0xfd,0x80,0xaa,0x6e,0xab,0x6a,0x3c,0x8d,0x96,0xa1,0xac,0xb0,0x8e,0x40};
    uint8_t plain[512], out[512];
    for (unsigned k = 0; k < sizeof want / sizeof want[0]; ++k) {
      for (unsigned i = 0; i < 512; ++i) plain[i] = (uint8_t)((want[k].block * 512 + i) * 7 + 3);
      jm_8bitdo_yichip_encrypt(want[k].block, plain, out);
      assert(!memcmp(out, want[k].head, 16));
      if (!want[k].block) assert(!memcmp(out + 496, tail0, 16));
    }
  }
  /* Telink pack: stamp, padding, size and a CRC the image check accepts. */
  {
    uint8_t app[0x25], image[0x40];
    size_t n = 0;
    const uint8_t stamp[6] = {1, 2, 3, 4, 5, 6};
    memset(app, 0x11, sizeof app);
    memcpy(app + 8, "KNLT", 4);
    assert(!jm_8bitdo_telink_pack(app, sizeof app, stamp, image, 0x30, &n));
    assert(jm_8bitdo_telink_pack(app, sizeof app, stamp, image, sizeof image, &n) && n == 0x34);
    assert(image[2] == 1 && image[7] == 6 && rd32(image + 0x18) == 0x34 && image[0x2f] == 0xff);
    assert(jm_8bitdo_image_check(image, n));
    image[0x20] ^= 1;
    assert(!jm_8bitdo_image_check(image, n));
  }
  return 0;
}
#endif
