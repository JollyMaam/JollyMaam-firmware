/* The SDK's vendor HCI command group (vendor_manager.c, OGF 0x3f): OCF 1
 * sets the FCC test parameters, OCF 2 enables random paging, OCF 0 is a CSR
 * "harmony" query that rewrites bytes of the command in place and returns
 * the whole buffer as a vendor event (byte 1 set to 1).  Other OCFs are not
 * matched (0).  vendor_manager_init, which registers the SDK's handler
 * table, has no caller and is not carried over. */
#include <string.h>
#include "btctrler/glue.h"

void printf_buf(const u8 *buf, int size);
void log_print(int level, const char *file, const char *format, ...);
extern const u8 log_tag_const_i_HCI_LMP;

#define VENDOR_CODE __attribute__((section(".vendor_manager_code")))
#define LOG(...) do { if (log_tag_const_i_HCI_LMP) log_print(2, (const char *)0, __VA_ARGS__); } while (0)

/* The SDK's fixed 12-byte block copied into a CSR harmony reply. */
const u8 vendor_1[12] __attribute__((section(".vendor_manager_const"))) =
    { 0x05, 0x00, 0x00, 0x00, 0xbf, 0xee, 0x7a, 0x27, 0x34, 0x32, 0xda, 0xaf };

/* Application hook for the FCC parameters; non-zero means "handled". */
__attribute__((weak)) VENDOR_CODE u8 user_bredr_fcc(u8 *data) { (void)data; return 0; }

static VENDOR_CODE int vendor_handler(u16 opcode, u8 *data, u32 len)
{
    int ocf = opcode & 1023;
    if (log_tag_const_i_HCI_LMP) {
        log_print(2, (const char *)0, "[Info]: [HCI_LMP]>Vendor cmd : \r\n");
        printf_buf(data, (int)len);
    }
    switch (ocf) {
    case 1:
        LOG("[Info]: [HCI_LMP]VENDOR_BREDR_FCC\r\n");
        if (user_bredr_fcc(data) == 0) bredr_fcc_set_param(data);
        hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 2:
        LOG("[Info]: [HCI_LMP] VENDOR_RAND_PAGE\r\n");
        link_rand_page_enable(data);
        hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 0: {
        u8 kind = data[7];
        switch (kind) {
        case 25:
            LOG("[Info]: [HCI_LMP]CSR harmony %x / %x - %x\r\n", ocf, kind, data[8]);
            data[11] = (u8)hci_param.lmp_pal_subversion;
            data[12] = (u8)(hci_param.lmp_pal_subversion >> 8);
            break;
        case 26:
            LOG("[Info]: [HCI_LMP]CSR harmony %x / %x - %x\r\n", ocf, kind, data[8]);
            data[11] = 7;
            break;
        case 89:
            LOG("[Info]: [HCI_LMP]CSR harmony %x / %x - %x\r\n", ocf, kind, data[8]);
            data[9] = 1;
            break;
        case 3:
            LOG("[Info]: [HCI_LMP]CSR harmony %x / %x - %x - %x - %x\r\n",
                ocf, kind, data[8], data[11], data[12]);
            if (data[11] == 0x8c) {
                if (data[12] == 2) data[17] = 1;
            } else if (data[11] == 0x5c) {
                if (data[12] == 2) memcpy(data + 13, vendor_1, 12);
            }
            break;
        default: break;
        }
        data[1] = 1;
        if (log_tag_const_i_HCI_LMP) {
            log_print(2, (const char *)0, "[Info]: [HCI_LMP]>>Vendor response: \r\n");
            printf_buf(data, (int)len);
        }
        hci_send_event(255, "LB", len, data);
        return 1;
    }
    default:
        return 0;
    }
}

VENDOR_CODE int hci_vendor_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    return vendor_handler(opcode, (u8 *)data, len);
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct hci_parameter hci_param;
const u8 log_tag_const_i_HCI_LMP = 0;
void log_print(int level, const char *file, const char *format, ...) { (void)level; (void)file; (void)format; }
void printf_buf(const u8 *buf, int size) { (void)buf; (void)size; }
static int fcc_calls, rand_calls, completions, events, completion_opcode, completion_status;
static const u8 *fcc_arg, *rand_arg, *event_data;
static u32 event_len;
void bredr_fcc_set_param(u8 *data) { fcc_arg = data; ++fcc_calls; }
int link_rand_page_enable(u8 *addr) { rand_arg = addr; ++rand_calls; return 0; }
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    CHECK(!strcmp(format, "1"));
    completion_status = va_arg(ap, int); completion_opcode = opcode; ++completions;
    va_end(ap); return 0;
}
int hci_send_event(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    CHECK(opcode == 255 && !strcmp(format, "LB"));
    event_len = va_arg(ap, u32); event_data = va_arg(ap, const u8 *); ++events;
    va_end(ap); return 0;
}
static void reset(void)
{
    fcc_calls = rand_calls = completions = events = 0; fcc_arg = rand_arg = event_data = 0;
}
int main(void)
{
    u8 d[32];
    /* Only OCF 0, 1 and 2 are matched (OCF is the low 10 opcode bits). */
    reset(); memset(d, 0, sizeof(d));
    CHECK(hci_vendor_cmd_handler(0xfc03, d, 8) == 0 && !completions && !events);
    CHECK(hci_vendor_cmd_handler(0xfc00 | 0x3ff, d, 8) == 0);
    /* OCF 1: FCC parameters, then command complete status 0. */
    CHECK(hci_vendor_cmd_handler(0xfc01, d, 8) == 1);
    CHECK(fcc_calls == 1 && fcc_arg == d && completions == 1 && completion_opcode == 0xfc01 && completion_status == 0);
    /* The group bits are ignored by the switch (opcode & 1023). */
    reset(); CHECK(hci_vendor_cmd_handler(0x0001, d, 8) == 1 && fcc_calls == 1);
    /* OCF 2: random page. */
    reset(); CHECK(hci_vendor_cmd_handler(0xfc02, d, 8) == 1);
    CHECK(rand_calls == 1 && rand_arg == d && completions == 1 && completion_opcode == 0xfc02 && !fcc_calls);

    /* OCF 0: CSR harmony; the buffer is edited and returned in a vendor event. */
    hci_param.lmp_pal_subversion = 0x22bb;
    reset(); memset(d, 0xaa, sizeof(d)); d[7] = 25;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1);
    CHECK(d[1] == 1 && d[11] == 0xbb && d[12] == 0x22 && d[9] == 0xaa && d[0] == 0xaa);
    CHECK(events == 1 && event_data == d && event_len == 20 && !completions);
    reset(); memset(d, 0xaa, sizeof(d)); d[7] = 26;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1 && d[11] == 7 && d[12] == 0xaa && d[1] == 1);
    reset(); memset(d, 0xaa, sizeof(d)); d[7] = 89;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1 && d[9] == 1 && d[11] == 0xaa && d[1] == 1);
    reset(); memset(d, 0xaa, sizeof(d)); d[7] = 99;      /* unknown kind: only d[1] changes */
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1 && d[1] == 1 && d[7] == 99 && d[9] == 0xaa && events == 1);
    /* Kind 3: keyed on d[11] and d[12]. */
    reset(); memset(d, 0, sizeof(d)); d[7] = 3; d[11] = 0x8c; d[12] = 2;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1 && d[17] == 1 && d[13] == 0);
    reset(); memset(d, 0, sizeof(d)); d[7] = 3; d[11] = 0x8c; d[12] = 3;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1 && d[17] == 0);
    reset(); memset(d, 0, sizeof(d)); d[7] = 3; d[11] = 0x5c; d[12] = 2;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 25) == 1);
    CHECK(!memcmp(d + 13, (u8[]){ 5, 0, 0, 0, 0xbf, 0xee, 0x7a, 0x27, 0x34, 0x32, 0xda, 0xaf }, 12));
    CHECK(!memcmp(vendor_1, d + 13, 12));
    reset(); memset(d, 0, sizeof(d)); d[7] = 3; d[11] = 0x5c; d[12] = 1;
    CHECK(hci_vendor_cmd_handler(0xfc00, d, 20) == 1 && d[13] == 0);
    return TEST_DONE("btctrler_vendor_manager");
}
#endif
