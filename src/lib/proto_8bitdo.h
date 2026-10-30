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
/* Pure packet interfaces, usable with blocking or asynchronous transports.
 * The 64-byte request excludes the synthetic zero report ID hidapi needs.
 * For program, length is payload size. For read/identify it is requested size.
 * Erase/start take length zero. Program payload must remain valid only until
 * encode returns. Output is untouched on invalid arguments. */
enum jm_8bitdo_result jm_8bitdo_request(uint8_t out[64], uint16_t command,
                                        uint32_t address, const uint8_t *payload,
                                        size_t length);
/* Replies include the leading zero byte as returned by the existing loader.
 * Return a borrowed payload view valid as long as report. On error outputs
 * are untouched. STALE means a read reply for another address; keep waiting.
 * Reply CRC is not enforced: its semantics are not established by the current
 * reference implementation. */
enum jm_8bitdo_result jm_8bitdo_reply(const uint8_t *report, size_t size,
                                      uint16_t command, uint32_t address,
                                      const uint8_t **payload, size_t *length);
uint16_t jm_8bitdo_crc16(const uint8_t *data, size_t size);
/* Validate the packed Telink image's signature, declared size and CRC32.
 * Trailing bytes are allowed for the full updater/application image. */
int jm_8bitdo_image_check(const uint8_t *image, size_t size);
/* Locate the product in the sector-aligned MAIN header after the updater.
 * Returns a NUL-terminated product string in out[17], untouched on failure. */
int jm_8bitdo_image_product(const uint8_t *image, size_t size, char out[17]);
#endif
