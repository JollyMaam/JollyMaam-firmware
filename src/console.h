#ifndef CYCLONE2_CONSOLE_H
#define CYCLONE2_CONSOLE_H

#include "generic/typedef.h"

/*
 * The USB console: line editing, completion, the command table, the config
 * and status namespaces, and the tagged machine mode tools use.  docs/usb-
 * control.md describes it from the outside.
 */

/* A handler's return value that means "print the usage line". */
#define CONSOLE_USAGE (-2)

struct console_command {
    const char *name;
    const char *usage;          /* the arguments, e.g. "SLOT [--fresh]" */
    const char *help;           /* one line */
    int (*run)(int argc, char **argv);  /* 0, -1 (error printed), CONSOLE_USAGE */
    const char *const *words;   /* completions for the first argument */
    u8 paths;                   /* the first argument is a path: CONSOLE_PATHS_* */
    /* A namespace instead of a command: its subcommands (ending with a NULL
     * name) and the text `NAME help` starts with. */
    const struct console_command *subcommands;
    const char *overview;
};

#define CONSOLE_PATHS_CONFIG 1U     /* profile.A.type (config's own) */
#define CONSOLE_PATHS_STATUS 2U     /* radio.state (status's own) */
#define CONSOLE_PATHS_ANY    3U     /* config.x or status.x, or a command */

/* The top level, ending with a NULL name (commands.c). */
extern const struct console_command console_commands[];

/* Output of a command.  In machine mode every line is tagged with the
 * command's id, so print whole lines ending in "\r\n". */
void console_write(const char *text);
void console_printf(const char *format, ...);
/* "OK ..." / "ERR ..." (machine mode: the error goes on the closing line).
 * console_fail returns -1 for `return console_fail(...)`. */
void console_ok(const char *format, ...);
int console_fail(const char *format, ...);
void console_hex(const u8 *data, u32 length);

/* The transport (usb.c) hands input here and asks for output. */
void console_input(const u8 *data, u32 length);
void console_opened(void);
void console_closed(void);
void console_poll(void);

/* Handlers the table in commands.c points at, implemented in console.c. */
int console_cmd_help(int argc, char **argv);
int console_cmd_debug(int argc, char **argv);
int console_cmd_complete(int argc, char **argv);
int config_cmd_get(int argc, char **argv);
int config_cmd_reset(int argc, char **argv);
int config_cmd_commit(int argc, char **argv);
int config_cmd_pending(int argc, char **argv);
int status_cmd_get(int argc, char **argv);

#endif
