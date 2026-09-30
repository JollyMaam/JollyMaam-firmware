#include <system/includes.h>
#include <stdarg.h>

#include "config.h"
#include "console.h"
#include "controller.h"
#include "usb.h"

#define LINE_SIZE       160U
#define HISTORY         6U
#define MAX_ARGS        8U
#define LOG_LINES       48U
#define LOG_LINE_SIZE   96U
#define CANDIDATES      24U
#define CANDIDATE_SIZE  40U
#define ID_SIZE         12U
#define NAMESPACE_CONFIG "config"
#define NAMESPACE_STATUS "status"

struct log_line {
    u32 timestamp_ms;
    u8 length;
    char text[LOG_LINE_SIZE];
};

static char line[LINE_SIZE];
static u8 line_length;
static char history[HISTORY][LINE_SIZE];
static u32 history_count;
static u8 history_browse;
static u8 escape_state;
static u8 last_was_cr;
static u8 machine;
static u8 executing;
static u8 at_line_start = 1U;
static char command_id[ID_SIZE];
static char failure[160];
static u8 failed;
static u8 last_key_tab;

static u8 log_read;
static u8 log_write;
static u16 log_dropped;
static struct log_line log_lines[LOG_LINES];

static char candidates[CANDIDATES][CANDIDATE_SIZE];
static u8 candidate_count;
static char scratch[192];
static char value_text[112];

extern void cyclone2_usb_write(const void *data, u32 length);

static void raw(const char *data, u32 length)
{
    cyclone2_usb_write(data, length);
}

static void raw_text(const char *text)
{
    raw(text, strlen(text));
}

static void out(const char *data, u32 length)
{
    while (length) {
        const char *end = memchr(data, '\n', length);
        u32 chunk = end ? (u32)(end - data) + 1U : length;

        if (at_line_start && machine && executing) {
            raw("=", 1U);
            raw_text(command_id);
            raw(" ", 1U);
        }
        raw(data, chunk);
        at_line_start = end != NULL;
        data += chunk;
        length -= chunk;
    }
}

void console_write(const char *text)
{
    out(text, strlen(text));
}

static void vprint(const char *format, va_list arguments)
{
    int length = vsnprintf(scratch, sizeof(scratch), format, arguments);

    if (length <= 0) return;
    if ((u32)length >= sizeof(scratch)) length = sizeof(scratch) - 1U;
    out(scratch, (u32)length);
}

void console_printf(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprint(format, arguments);
    va_end(arguments);
}

void console_ok(const char *format, ...)
{
    va_list arguments;

    console_write("OK ");
    va_start(arguments, format);
    vprint(format, arguments);
    va_end(arguments);
    console_write("\r\n");
}

int console_fail(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(failure, sizeof(failure), format, arguments);
    va_end(arguments);
    failed = 1U;
    if (!machine) {
        console_write("ERR ");
        console_write(failure);
        console_write("\r\n");
    }
    return -1;
}

void console_hex(const u8 *data, u32 length)
{
    static const char digits[] = "0123456789abcdef";
    char pair[2];

    while (length--) {
        pair[0] = digits[*data >> 4];
        pair[1] = digits[*data++ & 15U];
        out(pair, 2U);
    }
}

static void prompt(void)
{
    if (machine || cyclone2_usb_uboot_pending()) return;
    raw_text("c2> ");
    raw(line, line_length);
}

void cyclone2_usb_debug(const char *format, ...)
{
    struct log_line *entry;
    u8 next;
    int length;
    va_list arguments;

    if (!config.log.console) return;
    next = (u8)((log_write + 1U) % LOG_LINES);
    if (next == log_read) {
        log_read = (u8)((log_read + 1U) % LOG_LINES);
        log_dropped++;
    }
    entry = &log_lines[log_write];
    entry->timestamp_ms = sys_timer_get_ms();
    va_start(arguments, format);
    length = vsnprintf(entry->text, sizeof(entry->text), format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((u32)length >= sizeof(entry->text)) length = sizeof(entry->text) - 1U;
    while (length > 0 && (entry->text[length - 1] == '\r' ||
                          entry->text[length - 1] == '\n'))
        length--;
    if (length == 0) return;
    entry->length = (u8)length;
    log_write = next;
}

static void flush_log_line(void)
{
    struct log_line *entry = &log_lines[log_read];
    char prefix[40];
    int length;

    if (log_dropped) {
        length = snprintf(prefix, sizeof(prefix), "%s[%u log lines dropped]\r\n",
                          machine ? "! " : "\r\x1b[K", log_dropped);
        log_dropped = 0U;
        if (length > 0) raw(prefix, (u32)length);
        if (!machine) prompt();
    }
    length = snprintf(prefix, sizeof(prefix), "%s[%u.%03u] ",
                      machine ? "! " : "\r\x1b[K",
                      entry->timestamp_ms / 1000U, entry->timestamp_ms % 1000U);
    if (length > 0) raw(prefix, (u32)length);
    raw(entry->text, entry->length);
    raw("\r\n", 2U);
    log_read = (u8)((log_read + 1U) % LOG_LINES);
    prompt();
}

static int starts_with(const char *text, const char *prefix)
{
    return !strncmp(text, prefix, strlen(prefix));
}

static int same_letters(const char *first, const char *second, u32 length)
{
    while (length--) {
        char one = *first++;
        char two = *second++;

        if (one >= 'A' && one <= 'Z') one = (char)(one - 'A' + 'a');
        if (two >= 'A' && two <= 'Z') two = (char)(two - 'A' + 'a');
        if (one != two) return 0;
        if (!one) return 1;
    }
    return 1;
}

static const struct cfg_item *table_of(int is_config, u16 *count)
{
    *count = is_config ? config_item_count : status_item_count;
    return is_config ? config_items : status_items;
}

static const char *space_name(int is_config)
{
    return is_config ? NAMESPACE_CONFIG : NAMESPACE_STATUS;
}

static void print_leaf(const struct cfg_leaf *leaf, int is_config, int annotate)
{
    char path[48];

    cfg_leaf_path(leaf, path, sizeof(path));
    if (!is_config) {
        cfg_status_format(leaf, value_text, sizeof(value_text));
        console_printf("status %s=%s\r\n", path, value_text);
        return;
    }
    {
        int derived;

        cfg_leaf_format(&config, leaf, value_text, sizeof(value_text), &derived);

        console_printf("config %s=%s", path, derived && !annotate ? "none" : value_text);
        if (derived && annotate) {
            console_write(cfg_leaf_is_pending(leaf) ? "  # derived, not written" :
                                                      "  # derived");
            annotate = 0;
        }
    }
    if (annotate) {
        int changed = !cfg_leaf_is_default(&config, leaf);
        int pending = cfg_leaf_is_pending(leaf);

        if (changed) {
            u8 value[64];

            cfg_leaf_default(leaf, value);
            cfg_format(leaf->item, value, value_text, sizeof(value_text));
            console_printf("  # default %s%s",
                           leaf->item->derived ? "derived" : value_text,
                           pending ? ", not written" : "");
        } else if (pending) {
            console_write("  # not written");
        }
    }
    console_write("\r\n");
}

static void print_leaf_help(const struct cfg_leaf *leaf, int is_config)
{
    const struct cfg_item *item = leaf->item;
    const struct cfg_choice *choice;

    print_leaf(leaf, is_config, 0);
    if (item->help) console_printf("  %s\r\n", item->help);
    console_printf("  %s", cfg_type_name(item));
    if (item->type == CFG_ENUM) {
        for (choice = item->choices; choice->name; choice++)
            console_printf(" %s", choice->name);
    } else if (item->type == CFG_UINT && item->max) {
        console_printf(" %u..%u", (unsigned)item->min, (unsigned)item->max);
    } else if (item->type == CFG_PERCENT) {
        console_printf(" 0..%u", (unsigned)item->max);
    }
    if (is_config) {
        u8 value[64];

        cfg_leaf_default(leaf, value);
        cfg_format(item, value, value_text, sizeof(value_text));
        console_printf("; default %s; %s", item->derived ? "derived" : value_text,
                       cfg_apply_name(item->apply));
    } else {
        console_write("; read-only");
    }
    console_write("\r\n");
}

static int path_statement(char *text, int is_config)
{
    u16 count;
    const struct cfg_item *items = table_of(is_config, &count);
    struct cfg_leaf leaf;
    char *equals = strchr(text, '=');
    char *value = NULL;
    const char *error;
    int help = 0;
    u32 position = 0U;
    u32 shown = 0U;

    if (equals) {
        char *end = equals;
        char *comment = strchr(equals, '#');

        if (comment) *comment = '\0';
        value = equals + 1;
        while (end > text && end[-1] == ' ') end--;
        *end = '\0';
        while (*value == ' ') value++;
        end = value + strlen(value);
        while (end > value && end[-1] == ' ') end--;
        *end = '\0';
        if (*value == '"' && end > value + 1 && end[-1] == '"') {
            value++;
            end[-1] = '\0';
        }
    } else {
        char *space = strchr(text, ' ');

        if (space) {
            *space++ = '\0';
            while (*space == ' ') space++;
            if (!strcmp(space, "-h") || !strcmp(space, "--help") ||
                !strcmp(space, "help")) help = 1;
            else if (*space)
                return console_fail("use %s PATH, %s PATH=VALUE or %s PATH help",
                                    space_name(is_config), space_name(is_config),
                                    space_name(is_config));
        }
    }

    if (value) {
        u8 before[64];

        if (!is_config) return console_fail("status is read-only");
        if (cfg_leaf_find(items, count, text, &leaf) != 0)
            return console_fail("no such setting: %s", text);
        memcpy(before, cfg_leaf_data(&config, &leaf), leaf.item->size);
        error = cfg_set(&config, &leaf, value);
        if (!error) error = cfg_check(&config, &leaf);
        if (error) {
            memcpy(cfg_leaf_data(&config, &leaf), before, leaf.item->size);
            return console_fail("%s: %s", text, error);
        }
        cfg_leaf_path(&leaf, value_text, sizeof(value_text));
        console_printf("OK config %s=", value_text);
        cfg_format(leaf.item, cfg_leaf_data(&config, &leaf), value_text,
                   sizeof(value_text));
        console_printf("%s (%s%s%s%s)\r\n", value_text,
                       cfg_apply_name(leaf.item->apply),
                       cfg_set_note() ? "; " : "",
                       cfg_set_note() ? cfg_set_note() : "",
                       config.autocommit ? "" : "; not written: autocommit is off");
        return 0;
    }
    while (cfg_leaf_next(items, count, &position, &leaf)) {
        if (!cfg_leaf_under(&leaf, text)) continue;
        if (help) print_leaf_help(&leaf, is_config);
        else print_leaf(&leaf, is_config, 1);
        shown++;
    }
    return shown ? 0 : console_fail("no such %s: %s", is_config ? "setting" : "value",
                                    text);
}

/* `get [PATH] [--changed]`: every value under PATH ("" is everything);
 * --changed only the settings that differ from their defaults, as lines
 * that set them again when pasted back. */
static int get(int argc, char **argv, int is_config)
{
    u16 count;
    const struct cfg_item *items = table_of(is_config, &count);
    struct cfg_leaf leaf;
    const char *path = "";
    u32 position = 0U;
    u32 shown = 0U;
    int changed = 0;
    int index;

    for (index = 1; index < argc; index++) {
        if (!strcmp(argv[index], "--changed")) changed = 1;
        else if (*path || argv[index][0] == '-') return CONSOLE_USAGE;
        else path = argv[index];
    }
    if (changed && !is_config)
        return console_fail("status values have no defaults; status get [PATH]");
    while (cfg_leaf_next(items, count, &position, &leaf)) {
        if (!cfg_leaf_under(&leaf, path)) continue;
        shown++;
        if (changed && cfg_leaf_is_default(&config, &leaf)) continue;
        print_leaf(&leaf, is_config, !changed);
    }
    return shown ? 0 : console_fail("nothing under %s", path);
}

int config_cmd_get(int argc, char **argv)
{
    return get(argc, argv, 1);
}

int status_cmd_get(int argc, char **argv)
{
    return get(argc, argv, 0);
}

/* `config reset PATH`: back to the default.  `config reset --all`: every
 * setting, autocommit included, written at once - or with --no-commit,
 * autocommit off and nothing written (commit keeps it, a reboot undoes
 * it).  The whole thing needs its own word, so a stray Enter cannot. */
int config_cmd_reset(int argc, char **argv)
{
    struct cfg_leaf leaf;
    u32 position = 0U;
    u32 reset = 0U;

    if (argc >= 2 && !strcmp(argv[1], "--all")) {
        if (argc == 3 && !strcmp(argv[2], "--no-commit")) {
            config_set_autocommit(0U);
            config_reset_all(1);
            console_ok("every setting is back to its default; nothing written "
                       "(config commit keeps it, a reboot undoes it)");
        } else if (argc == 2) {
            config_reset_all(0);
            console_ok(config_commit(cyclone2_radio_quiet()) ?
                       "every setting is back to its default, and written" :
                       "every setting is back to its default; written once the "
                       "radio settles");
        } else {
            return CONSOLE_USAGE;
        }
        return 0;
    }
    if (argc != 2 || argv[1][0] == '-') return CONSOLE_USAGE;
    while (cfg_leaf_next(config_items, config_item_count, &position, &leaf)) {
        if (!cfg_leaf_under(&leaf, argv[1])) continue;
        cfg_leaf_default(&leaf, cfg_leaf_data(&config, &leaf));
        reset++;
    }
    if (!reset) return console_fail("no such setting: %s", argv[1]);
    console_ok("%u setting%s back to the default", reset, reset == 1U ? "" : "s");
    return 0;
}

int config_cmd_commit(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    if (!config_pending()) {
        console_ok("nothing to write");
        return 0;
    }
    if (config_commit(cyclone2_radio_quiet())) console_ok("written");
    else console_ok("queued: written once the radio settles");
    return 0;
}

int config_cmd_pending(int argc, char **argv)
{
    struct cfg_leaf leaf;
    u32 position = 0U;
    u32 count = 0U;

    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    while (cfg_leaf_next(config_items, config_item_count, &position, &leaf)) {
        if (!cfg_leaf_is_pending(&leaf)) continue;
        print_leaf(&leaf, 1, 0);
        count++;
    }
    if (!count) console_ok("nothing pending");
    return 0;
}

static int is_help(const char *word)
{
    return !strcmp(word, "help") || !strcmp(word, "-h") || !strcmp(word, "--help");
}

static const struct console_command *find_in(const struct console_command *table,
                                             const char *name)
{
    for (; table && table->name; table++)
        if (!strcmp(table->name, name)) return table;
    return NULL;
}

static void print_entry(const char *prefix, const struct console_command *command)
{
    int length = snprintf(value_text, sizeof(value_text), "  %s%s%s%s", prefix,
                          command->name, command->usage ? " " : "",
                          command->usage ? command->usage : "");

    if (length < 0) return;
    if (length > 34) {

        console_printf("%s\r\n", value_text);
        length = 0;
    }
    while (length < 34 && length + 1 < (int)sizeof(value_text))
        value_text[length++] = ' ';
    value_text[length] = '\0';
    console_printf("%s %s\r\n", value_text, command->help);
}

static void print_usage(const struct console_command *command,
                        const struct console_command *parent)
{
    console_printf("usage: %s%s%s%s%s\r\n  %s\r\n",
                   parent ? parent->name : "", parent ? " " : "", command->name,
                   command->usage ? " " : "", command->usage ? command->usage : "",
                   command->help);
}

static void print_groups(const char *label, int is_config)
{
    u16 count;
    const struct cfg_item *items = table_of(is_config, &count);
    struct cfg_leaf leaf;
    u32 position = 0U;
    char name[48];
    char previous[48] = "";

    console_printf("\r\n%s:", label);
    while (cfg_leaf_next(items, count, &position, &leaf)) {
        char *dot;

        snprintf(name, sizeof(name), "%s", leaf.item->path);
        dot = strchr(name, '.');
        if (dot) *dot = '\0';
        if (!strcmp(name, previous)) continue;
        memcpy(previous, name, sizeof(previous));
        console_printf(" %s", name);
    }
    console_write("\r\n");
}

static void print_overview(const struct console_command *space)
{
    const struct console_command *command;
    char prefix[24];

    if (space->overview) console_write(space->overview);
    snprintf(prefix, sizeof(prefix), "%s ", space->name);
    for (command = space->subcommands; command->name; command++)
        print_entry(prefix, command);
    if (!strcmp(space->name, NAMESPACE_CONFIG)) print_groups("Groups", 1);
    else if (!strcmp(space->name, NAMESPACE_STATUS)) print_groups("Groups", 0);
}

static int dispatch(const struct console_command *table,
                    const struct console_command *parent, int argc, char **argv)
{
    const struct console_command *command = find_in(table, argv[0]);
    int result;

    if (!command) {
        if (parent)
            return console_fail("%s has no %s; %s help", parent->name, argv[0],
                                parent->name);
        if (strchr(argv[0], '.'))
            return console_fail("unknown command %s; a path goes after its "
                                "namespace, with a space: config profile.A.type",
                                argv[0]);
        return console_fail("unknown command %s; type help", argv[0]);
    }
    if (command->subcommands) {
        if (argc == 1 || (argc == 2 && is_help(argv[1]))) {
            print_overview(command);
            return 0;
        }
        return dispatch(command->subcommands, command, argc - 1, argv + 1);
    }
    if (argc == 2 && is_help(argv[1])) {
        print_usage(command, parent);
        return 0;
    }
    result = command->run(argc, argv);
    if (result == CONSOLE_USAGE)
        return console_fail("usage: %s%s%s%s%s", parent ? parent->name : "",
                            parent ? " " : "", command->name,
                            command->usage ? " " : "",
                            command->usage ? command->usage : "");
    return result;
}

static int split(char *text, char **argv)
{
    int argc = 0;

    while (*text) {
        while (*text == ' ') text++;
        if (!*text) break;
        if (argc == (int)MAX_ARGS - 1) return -1;
        if (*text == '"') {
            argv[argc++] = ++text;
            while (*text && *text != '"') text++;
        } else {
            argv[argc++] = text;
            while (*text && *text != ' ') text++;
        }
        if (*text) *text++ = '\0';
    }
    return argc;
}

static int path_line(const char *text, const char **path)
{
    const struct console_command *space;
    const struct console_command *verb;
    const char *rest;
    char word[24];
    u32 length = 0U;
    int is_config;

    if (starts_with(text, NAMESPACE_CONFIG " ")) {
        is_config = 1;
        rest = text + strlen(NAMESPACE_CONFIG);
    } else if (starts_with(text, NAMESPACE_STATUS " ")) {
        is_config = 0;
        rest = text + strlen(NAMESPACE_STATUS);
    } else {
        return -1;
    }
    while (*rest == ' ') rest++;
    if (!*rest) return -1;
    while (rest[length] && rest[length] != ' ' && rest[length] != '=' &&
           length + 1U < sizeof(word)) {
        word[length] = rest[length];
        length++;
    }
    word[length] = '\0';
    if (is_help(word)) return -1;
    space = find_in(console_commands, space_name(is_config));
    for (verb = space ? space->subcommands : NULL; verb && verb->name; verb++)
        if (!strcmp(verb->name, word)) return -1;
    *path = rest;
    return is_config;
}

int console_cmd_help(int argc, char **argv)
{
    const struct console_command *command;
    char *words[MAX_ARGS];
    int index;

    if (argc == 1) {
        console_write("Cyclone 2 console.  Tab completes; NAME help (or -h, "
                      "--help) explains anything.\r\n\r\n");
        for (command = console_commands; command->name; command++)
            print_entry("", command);
        console_write("\r\nSlots are A B X Y: the Home+button chord that "
                      "selects them.\r\n");
        return 0;
    }
    if (argc == 3 && strchr(argv[2], '.') &&
        (!strcmp(argv[1], NAMESPACE_CONFIG) || !strcmp(argv[1], NAMESPACE_STATUS))) {
        char text[LINE_SIZE];

        snprintf(text, sizeof(text), "%s help", argv[2]);
        return path_statement(text, !strcmp(argv[1], NAMESPACE_CONFIG));
    }
    /* `help config reset` is `config reset help`. */
    for (index = 1; index < argc; index++) words[index - 1] = argv[index];
    words[argc - 1] = "help";
    return dispatch(console_commands, NULL, argc, words);
}

static int complete_line(const char *text);

static int run_line(char *text)
{
    char *argv[MAX_ARGS];
    const char *path;
    int is_config;
    int argc;

    while (*text == ' ') text++;
    if (!*text || *text == '#') return 0;
    is_config = path_line(text, &path);
    if (is_config >= 0) return path_statement((char *)path, is_config);
    if (starts_with(text, "debug complete "))
        return complete_line(text + strlen("debug complete "));
    argc = split(text, argv);
    if (argc < 0) return console_fail("too many arguments");
    return dispatch(console_commands, NULL, argc, argv);
}

static void execute(void)
{
    char *text = line;
    int result;

    line[line_length] = '\0';
    command_id[0] = '\0';
    if (machine && *text == '@') {
        u32 length = 0U;

        text++;
        while (*text && *text != ' ' && length + 1U < sizeof(command_id))
            command_id[length++] = *text++;
        command_id[length] = '\0';
        while (*text == ' ') text++;
    }
    if (!machine && line_length) {
        if (!history_count ||
            strcmp(history[(history_count - 1U) % HISTORY], line)) {
            memcpy(history[history_count % HISTORY], line, line_length + 1U);
            history_count++;
        }
    }
    history_browse = 0U;
    executing = 1U;
    at_line_start = 1U;
    failed = 0U;
    result = run_line(text);
    executing = 0U;
    if (!at_line_start) raw("\r\n", 2U);
    at_line_start = 1U;
    if (machine) {
        raw(".", 1U);
        raw_text(command_id);
        if (result != 0 || failed) {
            raw_text(" ERR ");
            raw_text(failed ? failure : "failed");
        } else {
            raw_text(" OK");
        }
        raw("\r\n", 2U);
    }
    line_length = 0U;
    prompt();
}

static void add_candidate(const char *text, u32 length)
{
    u8 index;

    if (length >= CANDIDATE_SIZE) length = CANDIDATE_SIZE - 1U;
    for (index = 0U; index < candidate_count; index++)
        if (strlen(candidates[index]) == length &&
            !strncmp(candidates[index], text, length)) return;
    if (candidate_count == CANDIDATES) return;
    memcpy(candidates[candidate_count], text, length);
    candidates[candidate_count][length] = '\0';
    candidate_count++;
}

static void complete_paths(const char *word, int is_config, int assign)
{
    u16 count;
    const struct cfg_item *items = table_of(is_config, &count);
    u32 length = strlen(word);
    struct cfg_leaf leaf;
    u32 position = 0U;
    char full[CANDIDATE_SIZE];

    while (cfg_leaf_next(items, count, &position, &leaf)) {
        char *dot;

        cfg_leaf_path(&leaf, full, sizeof(full));
        if (!same_letters(full, word, length)) continue;
        dot = strchr(full + length, '.');
        if (dot) {
            add_candidate(full, (u32)(dot - full) + 1U);
        } else if (assign && strlen(full) == length &&
                   strlen(full) + 1U < sizeof(full)) {
            strcat(full, "=");
            add_candidate(full, strlen(full));
        } else {
            add_candidate(full, strlen(full));
        }
    }
}

static void complete_names(const char *word, const struct console_command *table)
{
    u32 length = strlen(word);

    for (; table->name; table++)
        if (same_letters(table->name, word, length))
            add_candidate(table->name, strlen(table->name));
}

static void complete_words(const char *word, const char *const *words)
{
    u32 length = strlen(word);

    for (; words && *words; words++)
        if (same_letters(*words, word, length)) add_candidate(*words, strlen(*words));
}

static u32 complete(const char *text, u32 length)
{
    char buffer[LINE_SIZE];
    char *argv[MAX_ARGS];
    const struct console_command *command;
    const struct console_command *table = console_commands;
    const char *path;
    u32 start = length;
    int is_config;
    int argc;
    int position;
    int index;

    candidate_count = 0U;
    while (start && text[start - 1] != ' ' && text[start - 1] != '=') start--;
    memcpy(buffer, text, length);
    buffer[length] = '\0';

    is_config = path_line(buffer, &path);
    if (is_config == 1 && !strchr(path, '=') && length && text[length - 1] == ' ') {

        char name[48];
        u32 size = 0U;
        struct cfg_leaf leaf;

        while (path[size] && path[size] != ' ' && size + 1U < sizeof(name)) {
            name[size] = path[size];
            size++;
        }
        name[size] = '\0';
        while (path[size] == ' ') size++;
        if (!path[size] &&
            cfg_leaf_find(config_items, config_item_count, name, &leaf) == 0)
            add_candidate("=", 1U);
        return start;
    }
    if (is_config >= 0 && strchr(path, '=')) {

        static const char *const booleans[] = { "true", "false", NULL };
        struct cfg_leaf leaf;
        char *end = strchr(path, '=');

        while (end > path && end[-1] == ' ') end--;
        *end = '\0';
        if (is_config &&
            cfg_leaf_find(config_items, config_item_count, path, &leaf) == 0) {
            if (leaf.item->type == CFG_BOOL) {
                complete_words(text + start, booleans);
            } else if (leaf.item->type == CFG_ENUM) {
                const struct cfg_choice *choice;

                for (choice = leaf.item->choices; choice->name; choice++)
                    if (same_letters(choice->name, text + start, strlen(text + start)))
                        add_candidate(choice->name, strlen(choice->name));
            }
        }
        return start;
    }
    argc = split(buffer, argv);
    if (argc < 0) return start;
    position = start == length || argc == 0 ? argc : argc - 1;
    if (position == 0) {
        complete_names(text + start, console_commands);
        return start;
    }
    if (!strcmp(argv[0], "help")) {

        u32 skip = 4U;

        while (skip < length && text[skip] == ' ') skip++;
        return skip + complete(text + skip, length - skip);
    }

    command = NULL;
    for (index = 0; index < position && index < argc; index++) {
        command = find_in(table, argv[index]);
        if (!command) return start;
        if (!command->subcommands) break;
        table = command->subcommands;
    }
    if (command && command->subcommands && index == position) {

        complete_names(text + start, command->subcommands);
        if (!strcmp(command->name, NAMESPACE_CONFIG))
            complete_paths(text + start, 1, 1);
        else if (!strcmp(command->name, NAMESPACE_STATUS))
            complete_paths(text + start, 0, 0);
        return start;
    }
    if (!command || position - index != 1) return start;
    complete_words(text + start, command->words);
    if (command->paths == CONSOLE_PATHS_CONFIG) complete_paths(text + start, 1, 0);
    else if (command->paths == CONSOLE_PATHS_STATUS) complete_paths(text + start, 0, 0);
    return start;
}

static int complete_line(const char *text)
{
    u8 candidate;

    complete(text, strlen(text));
    for (candidate = 0U; candidate < candidate_count; candidate++)
        console_printf("%s\r\n", candidates[candidate]);
    return 0;
}

int console_cmd_complete(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return complete_line("");
}

static void list_candidates(void)
{
    u8 index;

    raw("\r\n", 2U);
    for (index = 0U; index < candidate_count; index++) {
        raw_text(candidates[index]);
        raw(index + 1U == candidate_count ? "\r\n" : "  ", 2U);
    }
    prompt();
}

static void tab(void)
{
    u32 start;
    u32 typed;
    u32 common;
    u8 index;

    line[line_length] = '\0';
    start = complete(line, line_length);
    typed = line_length - start;
    if (!candidate_count) return;
    common = strlen(candidates[0]);
    for (index = 1U; index < candidate_count; index++) {
        u32 same = 0U;

        while (same < common &&
               same_letters(&candidates[index][same], &candidates[0][same], 1U))
            same++;
        common = same;
    }
    if (common > typed ||
        (common == typed && memcmp(line + start, candidates[0], common))) {
        if (start + common >= LINE_SIZE - 1U) return;

        while (line_length > start) {
            line_length--;
            raw_text("\b \b");
        }
        raw(candidates[0], common);
        memcpy(line + start, candidates[0], common);
        line_length = (u8)(start + common);

        if (candidate_count == 1U && !memchr(line + start, '.', common) &&
            line[line_length - 1] != '=' && line_length + 1U < LINE_SIZE - 1U) {
            line[line_length++] = ' ';
            raw(" ", 1U);
        }
        last_key_tab = 0U;
        return;
    }

    if (!last_key_tab && candidate_count > 6U) {
        last_key_tab = 1U;
        return;
    }
    list_candidates();
    last_key_tab = 0U;
}

static void replace_line(const char *text)
{
    raw_text("\r\x1b[K");
    line_length = (u8)strlen(text);
    memcpy(line, text, line_length);
    prompt();
}

static void browse_history(int older)
{
    u8 available = history_count < HISTORY ? history_count : HISTORY;

    if (older) {
        if (history_browse >= available) return;
        history_browse++;
    } else {
        if (!history_browse) return;
        history_browse--;
    }
    replace_line(history_browse ?
                 history[(history_count - history_browse) % HISTORY] : "");
}

static void enter_machine_mode(void)
{
    /* Finish the human prompt before emitting line-oriented protocol replies. */
    if (!machine) raw("\r\n", 2U);
    machine = 1U;
    line_length = 0U;
}

static void key(u8 byte)
{
    if (escape_state == 1U) {
        escape_state = byte == '[' ? 2U : 0U;
        return;
    }
    if (escape_state == 2U) {
        escape_state = 0U;
        if (!machine && byte == 'A') browse_history(1);
        else if (!machine && byte == 'B') browse_history(0);
        return;
    }
    if (byte != '\t') last_key_tab = 0U;
    if (byte == '\r' || byte == '\n') {
        if (byte == '\n' && last_was_cr) {
            last_was_cr = 0U;
            return;
        }
        last_was_cr = byte == '\r';
        if (!machine) raw("\r\n", 2U);
        execute();
        return;
    }
    last_was_cr = 0U;
    switch (byte) {
    case 0x01U:
        if (machine) {
            machine = 0U;
            line_length = 0U;
            prompt();
        }
        return;
    case 0x02U:
        enter_machine_mode();
        return;
    case 0x03U:
        if (machine) return;
        raw_text("^C\r\n");
        line_length = 0U;
        history_browse = 0U;
        prompt();
        return;
    case 0x05U:
        raw("\x06", 1U);
        return;
    case 0x08U:
    case 0x7fU:
        if (line_length) {
            line_length--;
            if (!machine) raw_text("\b \b");
        }
        return;
    case '\t':
        if (!machine) tab();
        return;
    case 0x15U:
        if (!machine) replace_line("");
        else line_length = 0U;
        return;
    case 0x17U:
        while (line_length && line[line_length - 1] == ' ') line_length--;
        while (line_length && line[line_length - 1] != ' ') line_length--;
        if (!machine) {
            line[line_length] = '\0';
            replace_line(line);
        }
        return;
    case 0x1bU:
        escape_state = 1U;
        return;
    default:
        break;
    }
    if (byte < 0x20U || byte > 0x7eU) return;
    if (line_length + 1U >= LINE_SIZE) {
        /* Say so rather than silently dropping what is typed. */
        if (!machine) raw("\x07", 1U);
        return;
    }
    /* A previous client may leave machine mode set without dropping DTR. */
    if (!line_length) {
        if (byte == '@') enter_machine_mode();
        else if (machine) {
            machine = 0U;
            prompt();
        }
    }
    line[line_length++] = (char)byte;
    if (!machine) raw((const char *)&byte, 1U);
}

void console_input(const u8 *data, u32 length)
{
    while (length-- && !cyclone2_usb_uboot_pending()) key(*data++);
}

static void reset_input(void)
{
    machine = 0U;
    line_length = 0U;
    escape_state = 0U;
    last_was_cr = 0U;
    last_key_tab = 0U;
    history_browse = 0U;
    command_id[0] = '\0';
    at_line_start = 1U;
}

void console_opened(void)
{
    reset_input();
    raw_text("\r\nCyclone 2 console. Type help; Tab completes.\r\n");
    prompt();
}

void console_closed(void)
{
    /* Keep only what is logged after the next open: a closed terminal must
     * never hold the controller up. */
    log_read = log_write;
    log_dropped = 0U;
    reset_input();
}

void console_poll(void)
{
    u8 budget = 8U;

    if (!config.log.console) {

        log_read = log_write;
        log_dropped = 0U;
        return;
    }
    while (budget-- && log_read != log_write && !cyclone2_usb_uboot_pending())
        flush_log_line();
}
