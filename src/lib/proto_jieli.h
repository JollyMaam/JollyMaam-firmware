#ifndef JM_PROTO_JIELI_H
#define JM_PROTO_JIELI_H
#include <stddef.h>
#include <stdint.h>

int jm_jieli_cdb(uint8_t out[16], uint16_t command,
                  const uint8_t *arguments, size_t size);
int jm_jieli_read_cdb(uint8_t out[16], uint16_t command,
                       uint32_t address, size_t size);
int jm_jieli_write_cdb(uint8_t out[16], uint16_t command,
                        uint32_t address, const uint8_t *data, size_t size);
int jm_jieli_exec_reply(const uint8_t *reply, size_t size, uint16_t command,
                         uint8_t out[14]);
void jm_jieli_crc_cipher(uint8_t *data, size_t size, uint32_t key);
#endif
