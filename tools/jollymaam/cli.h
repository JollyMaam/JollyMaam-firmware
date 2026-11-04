#ifndef JOLLYMAAM_CLI_H
#define JOLLYMAAM_CLI_H
/* The jollymaam command's parts: jollymaam.c (devices, firmware) and
 * image.c (images, packages). */
#include <stddef.h>
#include <stdint.h>

void die(const char *fmt, ...);
uint8_t *read_file(const char *path, size_t *size);
void write_file(const char *path, const void *data, size_t size);
uint32_t number(const char *text);
void make_dir(const char *path);
int is_dir(const char *path);

int cmd_pack(int argc, char **argv);      /* pack jieli|telink|join */
int cmd_unpack(int argc, char **argv);
int cmd_package(int argc, char **argv);
/* An image's description, 0 when it is none jollymaam knows. */
int inspect_image(const char *path, const uint8_t *data, size_t size);
#endif
