#ifndef JM_PROTO_JIELI_H
#define JM_PROTO_JIELI_H
#include <stddef.h>
#include <stdint.h>

/* Pure UBOOT1.00 CDB helpers. No transport or process-global state. */
int jm_jieli_cdb(uint8_t out[16], uint16_t command,
                  const uint8_t *arguments, size_t size);
/* Prepare RAM/flash read or write commands. A write includes CRC16 over data.
 * Transfers must be nonempty and fit the protocol's 16-bit length field.
 * Outputs are untouched on failure. Address ranges must not wrap. */
int jm_jieli_read_cdb(uint8_t out[16], uint16_t command,
                       uint32_t address, size_t size);
int jm_jieli_write_cdb(uint8_t out[16], uint16_t command,
                        uint32_t address, const uint8_t *data, size_t size);
/* Validate an execution reply's big-endian echo and copy its 14-byte payload. */
int jm_jieli_exec_reply(const uint8_t *reply, size_t size, uint16_t command,
                         uint8_t out[14]);
/* Symmetric CrcDecode used by the ROM loader and chip-key response.
 * Each invocation restarts the stream; loader files use separate 512-byte blocks. */
void jm_jieli_crc_cipher(uint8_t *data, size_t size, uint32_t key);
#endif
