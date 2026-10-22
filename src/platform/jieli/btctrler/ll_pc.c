/* The SDK's LE power-control opcodes only log and report success. */
#include "btctrler/glue.h"

#ifdef UNITTEST
u8 log_tag_const_i_LL_PC;
#else
extern const u8 log_tag_const_i_LL_PC;
#endif
void log_print(int level, const char *tag, const char *format, ...);

int __attribute__((section(".ble_ll_code")))
le_cmd_set_pc(u16 opcode, const u8 *data, u32 size)
{
    (void)data;
    (void)size;
    int ocf = opcode & 1023;
    const char *message = NULL;

    switch (ocf) {
    case 118:
        message = "[Info]: [LL_PC]HCI_LE_READ_ENHANCED_TX_POWER\r\n";
        break;
    case 119:
        message = "[Info]: [LL_PC]HCI_LE_READ_REMOTE_TX_POWER\r\n";
        break;
    case 120:
        message = "[Info]: [LL_PC]HCI_LE_SET_PATH_LOSS_REPORTING_PARAMS\r\n";
        break;
    case 121:
        message = "[Info]: [LL_PC]HCI_LE_SET_PATH_LOSS_REPORTING_ENABLE\r\n";
        break;
    case 122:
        message = "[Info]: [LL_PC]HCI_LE_SET_TX_POWER_REPORT_ENABLE\r\n";
        break;
    default:
        return 0;
    }

    if (log_tag_const_i_LL_PC != 0) log_print(2, NULL, message);
    (void)hci_send_event_cmd_complete(opcode, "1", 0);
    return 1;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

static int log_calls;
static int log_level;
static const char *log_format;
static int event_calls;
static int event_opcode;
static const char *event_format;
static int event_status;

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag;
    log_calls++;
    log_level = level;
    log_format = format;
}

int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    event_status = va_arg(args, int);
    va_end(args);
    event_calls++;
    event_opcode = opcode;
    event_format = format;
    return 0;
}

int main(void)
{
    static const char *const messages[] = {
        "[Info]: [LL_PC]HCI_LE_READ_ENHANCED_TX_POWER\r\n",
        "[Info]: [LL_PC]HCI_LE_READ_REMOTE_TX_POWER\r\n",
        "[Info]: [LL_PC]HCI_LE_SET_PATH_LOSS_REPORTING_PARAMS\r\n",
        "[Info]: [LL_PC]HCI_LE_SET_PATH_LOSS_REPORTING_ENABLE\r\n",
        "[Info]: [LL_PC]HCI_LE_SET_TX_POWER_REPORT_ENABLE\r\n",
    };

    for (int i = 0; i < 5; i++) {
        const u16 opcode = (u16)(0x4000 | (118 + i));
        log_tag_const_i_LL_PC = 1;
        log_calls = event_calls = 0;
        CHECK(le_cmd_set_pc(opcode, NULL, 0) == 1);
        CHECK(log_calls == 1 && log_level == 2 && strcmp(log_format, messages[i]) == 0);
        CHECK(event_calls == 1 && event_opcode == opcode && strcmp(event_format, "1") == 0 && event_status == 0);

        log_tag_const_i_LL_PC = 0;
        log_calls = event_calls = 0;
        CHECK(le_cmd_set_pc((u16)(0x8000 | (118 + i)), NULL, 123) == 1);
        CHECK(log_calls == 0 && event_calls == 1 && event_status == 0);
    }

    log_tag_const_i_LL_PC = 1;
    log_calls = event_calls = 0;
    CHECK(le_cmd_set_pc(117, NULL, 0) == 0);
    CHECK(le_cmd_set_pc(123, NULL, 0) == 0);
    CHECK(le_cmd_set_pc((u16)(0x8000 | 117), NULL, 0) == 0);
    CHECK(log_calls == 0 && event_calls == 0);
    return TEST_DONE("btctrler_ll_pc");
}
#endif
