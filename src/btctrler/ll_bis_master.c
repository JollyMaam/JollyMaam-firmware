/* LE BIG synchronization HCI command dispatch.  The SDK recognizes the two
 * low-10-bit opcodes, logs when enabled, then reports success without parsing
 * either command's payload. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_BIS;
extern void log_print(int level, const char *tag, const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))

int le_cmd_set_bis_master(u16 opcode, const u8 *data, u32 size) LL_CODE;
int le_cmd_set_bis_master(u16 opcode, const u8 *data, u32 size)
{
    (void)data;
    (void)size;
    u32 full_opcode = opcode;
    int match = 0;

    switch (opcode & 0x03ffu) {
    case 107:
        if (log_tag_const_i_LL_BIS)
            log_print(2, NULL, "[Info]: [LL_BIS]HCI_LE_BIG_CREATE_SYNC\r\n");
        match = 1;
        break;
    case 108:
        if (log_tag_const_i_LL_BIS)
            log_print(2, NULL, "[Info]: [LL_BIS]HCI_LE_BIG_TERMINATE_SYNC\r\n");
        match = 1;
        break;
    default:
        break;
    }

    if (match)
        (void)hci_send_event_cmd_complete((int)full_opcode, "1", 0);
    return match;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

u8 log_tag_const_i_LL_BIS;
static int calls, logged, seen_opcode, seen_status;
static const char *seen_log;

void log_print(int level, const char *tag, const char *format, ...)
{
    CHECK(level == 2);
    CHECK(tag == NULL);
    logged++;
    seen_log = format;
}

int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    seen_status = va_arg(args, int);
    va_end(args);
    CHECK(strcmp(format, "1") == 0);
    calls++;
    seen_opcode = opcode;
    return 0;
}

int main(void)
{
    const u8 ignored[] = { 0xff, 0x00 };

    log_tag_const_i_LL_BIS = 0;
    CHECK(le_cmd_set_bis_master(107, ignored, sizeof(ignored)) == 1);
    CHECK(calls == 1 && seen_opcode == 107 && seen_status == 0);
    CHECK(logged == 0);

    log_tag_const_i_LL_BIS = 1;
    CHECK(le_cmd_set_bis_master(107, NULL, 0) == 1);
    CHECK(calls == 2 && seen_opcode == 107 && seen_status == 0);
    CHECK(logged == 1);
    CHECK(strcmp(seen_log, "[Info]: [LL_BIS]HCI_LE_BIG_CREATE_SYNC\r\n") == 0);

    CHECK(le_cmd_set_bis_master((u16)(0x0400u | 108u), NULL, 1234) == 1);
    CHECK(calls == 3 && seen_opcode == 0x0400 + 108 && seen_status == 0);
    CHECK(logged == 2);
    CHECK(strcmp(seen_log, "[Info]: [LL_BIS]HCI_LE_BIG_TERMINATE_SYNC\r\n") == 0);

    CHECK(le_cmd_set_bis_master(106, NULL, 0) == 0);
    CHECK(calls == 3 && logged == 2);
    return TEST_DONE("btctrler_ll_bis_master");
}
#endif
