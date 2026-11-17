#ifndef JM_PACKAGE_H
#define JM_PACKAGE_H
#include <stddef.h>
#include <stdint.h>
enum jm_package_result { JM_PACKAGE_OK, JM_PACKAGE_INVALID, JM_PACKAGE_UNSUPPORTED, JM_PACKAGE_CRC, JM_PACKAGE_DUPLICATE };
struct jm_package { const uint8_t *data; size_t size, directory; unsigned count; };
struct jm_package_entry { const uint8_t *name, *data; size_t name_size, size; uint32_t crc; };
enum jm_package_result jm_package_open(struct jm_package *out, const uint8_t *data, size_t size);
int jm_package_entry(const struct jm_package *package, unsigned index, struct jm_package_entry *out);
int jm_package_find(const struct jm_package *package, const char *name, struct jm_package_entry *out);
#endif
