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
    const char *usage;          /* the arguments, e.g. "N [--fresh]" */
    const char *help;           /* one line */
    int (*run)(int argc, char **argv);  /* 0, -1 (error printed), CONSOLE_USAGE */
    const char *const *words;   /* completions for the first argument */
    u8 paths;                   /* the arguments are config paths */
};

extern const struct console_command console_commands[];
extern const u16 console_command_count;

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
int console_cmd_unset(int argc, char **argv);
int console_cmd_export(int argc, char **argv);
int console_cmd_import(int argc, char **argv);
int console_cmd_commit(int argc, char **argv);
int console_cmd_pending(int argc, char **argv);
int console_cmd_factory_reset(int argc, char **argv);
int console_cmd_complete(int argc, char **argv);
int console_cmd_debug(int argc, char **argv);

#endif
