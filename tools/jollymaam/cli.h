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

struct jm;
struct jm_firmware;
/* PACKAGE: a folder holding manifest.txt, or a zip of one (dies if not). */
/* (the name open_firmware: it fills a struct jm_firmware, the library's) */
void open_firmware(const char *path, struct jm_firmware *fw);
/* A usage error: the message, exit 2 (die: exit 1). */
void usage_error(const char *fmt, ...);
/* pads.c: list, install, update, backup, reboot; program is argv[0]. */
int cmd_pads(const char *command, int argc, char **argv, const char *program, struct jm *jm);

int cmd_pack(int argc, char **argv);      /* pack jieli|telink|join */
int cmd_unpack(int argc, char **argv);
int cmd_package(int argc, char **argv);
/* An image's description, 0 when it is none jollymaam knows. */
int inspect_image(const char *path, const uint8_t *data, size_t size);
#endif
