#ifndef JM_PROTO_8BITDO_H
#define JM_PROTO_8BITDO_H
#include <stddef.h>
#include <stdint.h>

#define JM_8BITDO_REPORT_SIZE 64u
#define JM_8BITDO_CHUNK 46u
#define JM_8BITDO_SECTOR 4096u

enum jm_8bitdo_result {
  JM_8BITDO_OK, JM_8BITDO_ARGUMENT, JM_8BITDO_FRAMING,
  JM_8BITDO_STATUS, JM_8BITDO_COMMAND, JM_8BITDO_STALE, JM_8BITDO_LENGTH
};
enum jm_8bitdo_result jm_8bitdo_request(uint8_t out[64], uint16_t command,
                                        uint32_t address, const uint8_t *payload,
                                        size_t length);
enum jm_8bitdo_result jm_8bitdo_reply(const uint8_t *report, size_t size,
                                      uint16_t command, uint32_t address,
                                      const uint8_t **payload, size_t *length);
uint16_t jm_8bitdo_crc16(const uint8_t *data, size_t size);
int jm_8bitdo_image_check(const uint8_t *image, size_t size);
int jm_8bitdo_stock_image(const uint8_t *input, size_t size, uint16_t model, const uint8_t **image, size_t *image_size);
int jm_8bitdo_image_product(const uint8_t *image, size_t size, char out[17]);
#endif
