#ifndef CYCLONE2_CONSOLE_H
#define CYCLONE2_CONSOLE_H

#include "generic/typedef.h"

#define CONSOLE_USAGE (-2)

struct console_command {
    const char *name;
    const char *usage;
    const char *help;
    int (*run)(int argc, char **argv);
    const char *const *words;
    u8 paths;
    /* A namespace instead of a command: its subcommands (ending with a NULL
     * name) and the text `NAME help` starts with. */
    const struct console_command *subcommands;
    const char *overview;
};

#define CONSOLE_PATHS_CONFIG 1U
#define CONSOLE_PATHS_STATUS 2U
#define CONSOLE_PATHS_ANY    3U

extern const struct console_command console_commands[];

void console_write(const char *text);
void console_printf(const char *format, ...);

void console_ok(const char *format, ...);
int console_fail(const char *format, ...);
void console_hex(const u8 *data, u32 length);

void console_input(const u8 *data, u32 length);
void console_opened(void);
void console_closed(void);
void console_poll(void);

int console_cmd_help(int argc, char **argv);
int console_cmd_complete(int argc, char **argv);
int config_cmd_get(int argc, char **argv);
int config_cmd_reset(int argc, char **argv);
int config_cmd_commit(int argc, char **argv);
int config_cmd_pending(int argc, char **argv);
int status_cmd_get(int argc, char **argv);

#endif
