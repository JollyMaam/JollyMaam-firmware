#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "console.h"

static char output[8192];
static unsigned output_length;
const struct cfg_item status_items[] = {{0}};
const u16 status_item_count = 0;

int syscfg_read(u16 id, void *data, u16 size)
{
    (void)id; (void)data; (void)size;
    return -1;
}
int syscfg_write(u16 id, void *data, u16 size)
{
    (void)id; (void)data;
    return size;
}
void get_random_number(u8 *data, u8 size) { memset(data, 1, size); }
u8 *get_norflash_uuid(void) { return NULL; }
u32 sys_timer_get_ms(void) { return 0; }
int cyclone2_radio_quiet(void) { return 1; }
int cyclone2_usb_uboot_pending(void) { return 0; }
void cyclone2_usb_write(const void *data, u32 size)
{
    assert(output_length + size < sizeof(output));
    memcpy(output + output_length, data, size);
    output_length += size;
    output[output_length] = 0;
}

static int ping(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_ok("pong");
    return 0;
}
const struct console_command console_commands[] = {
    {.name = "ping", .usage = "", .help = "ping", .run = ping},
    {0},
};

static void clear_output(void) { output_length = 0; output[0] = 0; }
static void input(const char *text) { console_input((const u8 *)text, strlen(text)); }
static void expect(const char *text)
{
    if (strcmp(output, text)) {
        fprintf(stderr, "expected [%s], received [%s]\n", text, output);
        assert(0);
    }
    clear_output();
}

int main(void)
{
    config_defaults(&config);
    console_opened();
    assert(strstr(output, "c2> "));
    clear_output();
    input("ping\r\n");
    expect("ping\r\nOK pong\r\nc2> ");

    input("\x02@12 ping\r\n");
    expect("\r\n=12 OK pong\r\n.12 OK\r\n");
    input("\x05");
    expect("\x06");

    /* A client that leaves DTR high must not strand the next human in machine mode. */
    input("p");
    expect("c2> p");
    input("ing\r");
    expect("ing\r\nOK pong\r\nc2> ");

    /* Tagged clients can recover after a human command without reopening the tty. */
    input("@13 ping\n");
    expect("\r\n=13 OK pong\r\n.13 OK\r\n");
    input("@14 bad\r");
    assert(strstr(output, ".14 ERR "));
    assert(!strstr(output, "c2> "));
    clear_output();

    /* Neither an incomplete escape sequence nor CRLF state survives a session. */
    input("\x1b");
    console_closed();
    console_opened();
    clear_output();
    input("ping\n");
    expect("ping\r\nOK pong\r\nc2> ");
    input("\x02@15 ping\r");
    clear_output();
    console_closed();
    console_opened();
    clear_output();
    input("\n");
    expect("\r\nc2> ");
    input("ping\r");
    expect("ping\r\nOK pong\r\nc2> ");

    input("\x02\x01ping\r");
    expect("\r\nc2> ping\r\nOK pong\r\nc2> ");
    puts("console: all checks passed");
    return 0;
}
