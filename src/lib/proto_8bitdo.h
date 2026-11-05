#ifndef JM_PROTO_8BITDO_H
#define JM_PROTO_8BITDO_H
#include <stddef.h>
#include <stdint.h>

#define JM_8BITDO_REPORT_SIZE 64u
#define JM_8BITDO_CHUNK 46u
#define JM_8BITDO_SECTOR 4096u

/* The boot loader's models (IDENTIFY, command 8). */
#define JM_8BITDO_MODEL_U2C_BLUETOOTH 0x301au   /* Yichip YC3121 */
#define JM_8BITDO_MODEL_U2C_WIRELESS 0x301bu    /* Telink TLSR8208 */
#define JM_8BITDO_MODEL_U2C_RECEIVER 0x301cu

/* Telink pads: the loader starts the packed app at 0x6000; settings from
 * 0x6f000 stay. */
#define JM_8BITDO_TELINK_START 0x6000u
#define JM_8BITDO_TELINK_END 0x6f000u

/* Yichip pads: addresses are the CPU's (flash at 0x01000000).  An app goes
 * into the slot from BASE as one stream: an erase at BASE with the stream's
 * length as its total, then programs of 46 bytes with that total, encrypted
 * (jm_8bitdo_yichip_encrypt); reads come back in the clear.  The loader
 * programs only whole 512-byte blocks, and erases in 32 KiB blocks only for
 * a total of 32 KiB or more, so the stream is the app padded with 0xff to
 * whole BLOCKs.  Without that erase it acknowledges the programs but
 * leaves the flash erased.  The slot ends where its ciphered flash (36
 * bytes a 32-byte block) would reach our settings banks at flash 0x74000. */
#define JM_8BITDO_YICHIP_BASE 0x01018000u
#define JM_8BITDO_YICHIP_END 0x01060000u
#define JM_8BITDO_YICHIP_BLOCK 0x8000u
#define JM_8BITDO_YICHIP_CIPHER_BLOCK 512u

enum jm_8bitdo_result {
  JM_8BITDO_OK, JM_8BITDO_ARGUMENT, JM_8BITDO_FRAMING,
  JM_8BITDO_STATUS, JM_8BITDO_COMMAND, JM_8BITDO_STALE, JM_8BITDO_LENGTH
};
enum jm_8bitdo_result jm_8bitdo_request(uint8_t out[64], uint16_t command,
                                        uint32_t address, const uint8_t *payload,
                                        size_t length);
enum jm_8bitdo_result jm_8bitdo_request_total(uint8_t out[64], uint16_t command,
                                              uint32_t address, const uint8_t *payload,
                                              size_t length, uint32_t total);
void jm_8bitdo_yichip_encrypt(uint32_t block, const uint8_t plain[512], uint8_t out[512]);
int jm_8bitdo_telink_pack(const uint8_t *app, size_t size, const uint8_t stamp[6],
                          uint8_t *out, size_t room, size_t *out_size);
enum jm_8bitdo_result jm_8bitdo_reply(const uint8_t *report, size_t size,
                                      uint16_t command, uint32_t address,
                                      const uint8_t **payload, size_t *length);
uint16_t jm_8bitdo_crc16(const uint8_t *data, size_t size);
int jm_8bitdo_image_check(const uint8_t *image, size_t size);
int jm_8bitdo_stock_image(const uint8_t *input, size_t size, uint16_t model, const uint8_t **image, size_t *image_size);
int jm_8bitdo_image_product(const uint8_t *image, size_t size, char out[17]);
#endif
