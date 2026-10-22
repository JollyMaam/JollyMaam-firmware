/* The SDK's BLE HCI command entry (hci_ll.c): ll_hci_cmd_handler receives the
 * LE controller commands the HCI task posts as an array of words (opcode
 * first, then one word per parameter), starts or stops advertising, scanning
 * and initiating, hands link-layer commands to the link tasks
 * (ll_push_control_data) and answers with HCI events.  Any opcode it does not
 * handle goes to le_controller_handler_for_ble5.  ble_update_target registers
 * ll_hci_destory with the SDK's update-target table (section .update_target).
 *
 * SDK quirks kept: the disconnect command (0x0406) is handled here only when
 * a role is enabled and otherwise falls through to the OCF switch (OCF 6 is
 * LE Set Advertising Parameters); the LE Set Scan Enable path passes a scan
 * parameter whose "enable" byte is never set (only filter_duplicates is);
 * parameter structures are uninitialised locals, as the SDK has them; several
 * commands ignore the role/feature gate (create connection, cancel, encrypt
 * RAND, connection update, P-256, DH key).  The SDK's
 * per-command ll_hci_* wrappers have no caller and are not carried over. */
#include <string.h>
#include "btctrler/glue.h"

void log_print(int level, const char *file, const char *format, ...);
extern void ll_read_local_p256_public_key_ir(void) __asm__("ll_read_local_p256_public_key");
extern void ll_generate_dh_key_ir(const u8 *data, const u8 *size) __asm__("ll_generate_dh_key");
#ifdef UNITTEST
extern u8 log_tag_const_i_HCI_LL;
extern u64 config_btctler_le_features;
#else
extern const u8 log_tag_const_i_HCI_LL;
extern const u64 config_btctler_le_features;
#endif

#define HCI_LL_CODE __attribute__((section(".ble_hci_code")))
#define LL_LOG(...) do { if (log_tag_const_i_HCI_LL) log_print(2, (const char *)0, __VA_ARGS__); } while (0)
#define ARG(n) ((const u8 *)(uintptr_t)(u32)cmd[n])

HCI_LL_CODE void ll_hci_destory(void)
{
    ll_ext_destroy();
    ll_destroy();
}

const struct update_target ble_update_target __attribute__((section(".update_target"), used)) = {
    "ble", (update_handler_t)ll_hci_destory
};

HCI_LL_CODE void ll_event_handler(int *msg)
{
    (void)msg;
}

HCI_LL_CODE void ll_hci_cmd_handler(int *cmd)
{
    struct disconnect_paramter disconnect;
    u8 adv_param[15], adv_data[32], scan_resp_data[32];
    struct scan_enable_parameter scan_enable;
    u8 key[16], plaintext_data[16], enc[16], rand[8];
    struct long_term_key_request_reply_parameter ltk_reply;
    struct long_term_key_request_negative_reply_parameter ltk_negative;
    struct connection_update_parameter conn_update;
    struct test_key_num_parameter key_num;
    u16 hold_par[2];
    struct set_data_length_parameter data_length;
    struct set_phy_param phy_param;
    u16 opcode = (u16)cmd[0];
    u32 opcode32 = (u32)cmd[0] & 65535;

    if (opcode32 == 1030) {
        if ((config_btctler_le_roles & 3) != 0) {
            LL_LOG("[Info]: [HCI_LL]HCI_DISCONNECT 0x%x, 0x%x\r\n", cmd[1], cmd[2]);
            disconnect.connection_handle = (u16)cmd[1];
            disconnect.reason = (u8)cmd[2];
            int result = ll_push_control_data(opcode, (const u8 *)&disconnect);
            hci_send_event(15, "112", result, 1, 1030);
            return;
        }
    }
    switch (cmd[0] & 1023) {
    case 6:
        if ((config_btctler_le_roles & 4) == 0) return;
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_ADVERTISING_PARAMETERS\r\n");
        memcpy(adv_param, cmd + 1, 15);
        ll_adv_set_params(adv_param);
        return;
    case 8:
        if ((config_btctler_le_roles & 4) == 0) return;
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_ADVERTISING_DATA\r\n");
        memcpy(adv_data, cmd + 1, 32);
        ll_adv_set_adv_data(adv_data);
        return;
    case 9:
        if ((config_btctler_le_roles & 4) == 0) return;
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_SCAN_RESPONSE_DATA\r\n");
        memcpy(scan_resp_data, cmd + 1, 32);
        ll_adv_set_scan_rsp_data(scan_resp_data);
        return;
    case 10:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_ADVERTISE_ENABLE %s\r\n", cmd[1] != 0 ? "Enable" : "Disable");
        if (cmd[1] == 0) ll_adv_close();
        else ll_adv_open();
        return;
    case 12:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_SCAN_ENABLE %s\r\n", cmd[1] != 0 ? "Enable" : "Disable");
        if (cmd[1] == 0) {
            ll_scan_close();
        } else {
            scan_enable.filter_duplicates = (u8)cmd[2];
            ll_scan_open(&scan_enable.enable);
        }
        return;
    case 13:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_CREATE_CONNECTION\r\n");
        ll_init_open(ARG(1));
        return;
    case 14:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_CREATE_CONNECTION_CANCEL\r\n");
        destroy_le_ext_init();
        ll_init_close();
        return;
    case 23:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_ENCRYPT\r\n");
        if ((config_btctler_le_features & 1) == 0) return;
        memcpy(key, cmd + 1, 16);
        memcpy(plaintext_data, cmd + 5, 16);
        aes128_start(key, plaintext_data, enc);
        hci_send_event_cmd_complete(8215, "1c16", 0, enc);
        return;
    case 24:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_RAND\r\n");
        pseudo_random_genrate(rand, 8);
        hci_send_event_cmd_complete(opcode32, "1c08", 0, rand);
        return;
    case 25: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_START_ENCRYPT\r\n");
        if ((config_btctler_le_roles & 3) == 0) return;
        if ((config_btctler_le_features & 1) == 0) return;
        int result = ll_push_control_data(opcode, ARG(1));
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 26: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_LONG_TERM_KEY_REQUEST_REPLY\r\n");
        if ((config_btctler_le_roles & 3) == 0) return;
        if ((config_btctler_le_features & 1) == 0) return;
        ltk_reply.connection_handle = (u16)cmd[1];
        memcpy(ltk_reply.long_term_key, cmd + 2, 16);
        int result = ll_push_control_data(opcode, (const u8 *)&ltk_reply);
        hci_send_event_cmd_complete(opcode32, "12", result, btctler_little_endian_read_16((const u8 *)&ltk_reply, 0) & 4095);
        return;
    }
    case 27: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_LONG_TERM_KEY_REQUEST_NAGATIVE_REPLY\r\n");
        if ((config_btctler_le_roles & 3) == 0) return;
        if ((config_btctler_le_features & 1) == 0) return;
        ltk_negative.connection_handle = (u16)cmd[1];
        int result = ll_push_control_data(opcode, (const u8 *)&ltk_negative);
        hci_send_event_cmd_complete(opcode32, "12", result, btctler_little_endian_read_16((const u8 *)&ltk_negative, 0) & 4095);
        return;
    }
    case 19: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_CONNECTION_UPDATE\r\n");
        conn_update.connection_handle = (u16)cmd[1];
        conn_update.Conn_Interval_Min = (u16)cmd[2];
        conn_update.Conn_Interval_Max = (u16)cmd[3];
        conn_update.Conn_Latency = (u16)cmd[4];
        conn_update.Supervision_Timeout = (u16)cmd[5];
        conn_update.Minimum_CE_Length = (u16)cmd[6];
        conn_update.Maximum_CE_Length = (u16)cmd[7];
        int result = ll_push_control_data(opcode, (const u8 *)&conn_update);
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 37:
        hci_send_event(15, "112", 0, 1, opcode32);
        ll_read_local_p256_public_key_ir();
        return;
    case 38:
        hci_send_event(15, "112", 0, 1, opcode32);
        ll_generate_dh_key_ir(ARG(1), ARG(2));
        return;
    case 131: {
        if ((config_btctler_le_roles & 3) == 0) return;
        key_num.connection_handle = (u16)cmd[1];
        key_num.key_num = (u8)cmd[2];
        int result = ll_push_control_data(opcode, (const u8 *)&key_num);
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 132: {
        if ((config_btctler_le_roles & 3) == 0) return;
        hold_par[0] = (u16)cmd[1];
        hold_par[1] = (u16)cmd[2];
        int result = ll_push_control_data(opcode, (const u8 *)hold_par);
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 34: {
        if ((config_btctler_le_features & 32) == 0) return;
        data_length.connection_handle = (u16)cmd[1];
        data_length.txoctets = (u16)cmd[2];
        data_length.txtime = (u16)cmd[3];
        int result = ll_push_control_data(opcode, (const u8 *)&data_length);
        hci_send_event_cmd_complete(opcode32, "12", result, btctler_little_endian_read_16((const u8 *)&data_length, 0) & 4095);
        return;
    }
    case 20: {
        int result = ll_push_control_data(opcode, ARG(1));
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 50:
        if ((config_btctler_le_features & 2304) == 0) return;
        phy_param.Connection_Handle = (u16)cmd[1];
        phy_param.ALL_PHYs = (u8)cmd[2];
        phy_param.TX_PHYs = (u8)cmd[3];
        phy_param.RX_PHYs = (u8)cmd[4];
        phy_param.PHY_Options = (u16)cmd[5];
        le_controller_handler_for_ble5(opcode, (const u8 *)&phy_param, 7);
        return;
    default:
        le_controller_handler_for_ble5(opcode, ARG(1), (u32)cmd[2]);
        return;
    }
}

#ifdef UNITTEST
#include <stdarg.h>
#include <stdio.h>
#include "test.h"

int config_btctler_le_roles;
u64 config_btctler_le_features;
u8 log_tag_const_i_HCI_LL;

static char trace[2048];
static void T(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    size_t n = strlen(trace);
    vsnprintf(trace + n, sizeof(trace) - n, format, ap);
    va_end(ap);
}
#define EXPECT(s) do { if (strcmp(trace, (s))) { fprintf(stderr, "trace:    %s\nexpected: %s\n", trace, (s)); CHECK(0); } trace[0] = 0; } while (0)

static u8 push_result = 7;
static u8 pushed[32];
void log_print(int level, const char *file, const char *format, ...) { (void)file; T("log%d(%s);", level, format); }
void ll_ext_destroy(void) { T("ext_destroy;"); }
void ll_destroy(void) { T("destroy;"); }
u8 ll_push_control_data(u16 opcode, const u8 *param)
{
    static const struct { u16 opcode; int size; } sizes[] = {
        { 0x0406, 3 }, { 0x201a, 18 }, { 0x201b, 2 }, { 0x2013, 14 }, { 0x2022, 6 }, { 0xfc83, 3 }, { 0xfc84, 4 },
    };
    T("push(%x);", opcode);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (sizes[i].opcode == opcode) memcpy(pushed, param, sizes[i].size);
    return push_result;
}
int hci_send_event(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int a = va_arg(ap, int), b = va_arg(ap, int), c = va_arg(ap, int);
    T("event(%d,%s,%d,%d,%d);", opcode, format, a, b, c);
    va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int a = va_arg(ap, int);
    if (!strcmp(format, "12")) T("complete(%x,%s,%d,%d);", opcode, format, a, va_arg(ap, int));
    else { const u8 *p = va_arg(ap, const u8 *); T("complete(%x,%s,%d,%02x%02x);", opcode, format, a, p[0], p[1]); }
    va_end(ap); return 0;
}
int ll_adv_set_params(const u8 *d) { T("adv_params(%d,%d);", d[0], d[14]); return 0; }
int ll_adv_set_adv_data(const u8 *d) { T("adv_data(%d,%d);", d[0], d[31]); return 0; }
int ll_adv_set_scan_rsp_data(const u8 *d) { T("scan_rsp(%d,%d);", d[0], d[31]); return 0; }
int ll_adv_open(void) { T("adv_open;"); return 0; }
int ll_adv_close(void) { T("adv_close;"); return 0; }
static u8 scan_filter;
int ll_scan_open(const u8 *d) { scan_filter = d[1]; T("scan_open;"); return 0; }
int ll_scan_close(void) { T("scan_close;"); return 0; }
int ll_init_open(const u8 *d) { T("init_open(%lx);", (unsigned long)(uintptr_t)d); return 0; }
int ll_init_close(void) { T("init_close;"); return 0; }
void destroy_le_ext_init(void) { T("destroy_ext_init;"); }
void aes128_start(u8 *key, u8 *plain, u8 *out) { T("aes(%d,%d);", key[0], plain[0]); memset(out, 0xa5, 16); out[0] = key[15]; out[1] = plain[15]; }
void pseudo_random_genrate(uint8_t *dest, unsigned int size) { T("rand(%u);", size); memset(dest, 0x5a, size); }
uint16_t btctler_little_endian_read_16(const uint8_t *b, int pos) { return b[pos + 1] << 8 | b[pos]; }
int le_controller_handler_for_ble5(u16 opcode, const u8 *data, u32 size)
{
    if (size == 7) T("ble5(%x,7);phy(%02x%02x%02x%02x%02x%02x%02x);", opcode, data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
    else T("ble5(%x,%lx,%u);", opcode, (unsigned long)(uintptr_t)data, size);
    return 0;
}
void p256_stub(void) __asm__("ll_read_local_p256_public_key");
void p256_stub(void) { T("p256;"); }
void dhkey_stub(const u8 *a, const u8 *b) __asm__("ll_generate_dh_key");
void dhkey_stub(const u8 *a, const u8 *b) { T("dhkey(%lx,%lx);", (unsigned long)(uintptr_t)a, (unsigned long)(uintptr_t)b); }

static void run(int *cmd) { trace[0] = 0; ll_hci_cmd_handler(cmd); }

int main(void)
{
    /* The update target and the shutdown hook. */
    CHECK(!strcmp(ble_update_target.name, "ble"));
    CHECK(ble_update_target.driver_close == (update_handler_t)ll_hci_destory);
    trace[0] = 0; ll_hci_destory(); EXPECT("ext_destroy;destroy;");
    int msg[2] = { 1, 2 }; ll_event_handler(msg); EXPECT("");

    config_btctler_le_roles = 7; config_btctler_le_features = ~0ull;
    int cmd[10];

    /* Disconnect (0x0406): a link-layer push and the event with the constant opcode.  Without a role bit
     * (roles & 3 == 0) the command falls through to the OCF switch: OCF 6 is advertising parameters. */
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 0x0406; cmd[1] = 0x1234; cmd[2] = 0x13;
    run(cmd); EXPECT("push(406);event(15,112,7,1,1030);");
    CHECK(pushed[0] == 0x34 && pushed[1] == 0x12 && pushed[2] == 0x13);
    config_btctler_le_roles = 4; run(cmd); EXPECT("adv_params(52,0);");
    config_btctler_le_roles = 0; run(cmd); EXPECT("");
    config_btctler_le_roles = 7;
    log_tag_const_i_HCI_LL = 1; run(cmd);
    EXPECT("log2([Info]: [HCI_LL]HCI_DISCONNECT 0x%x, 0x%x\r\n);push(406);event(15,112,7,1,1030);");
    log_tag_const_i_HCI_LL = 0;

    /* Advertising parameters / data / scan response: 15 and 32 bytes copied from cmd + 1; role bit 4. */
    int words[20];
    memset(words, 0, sizeof(words));
    memcpy(words + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62);
    words[0] = 0x2006; run(words); EXPECT("adv_params(65,79);");
    words[0] = 0x2008; run(words); EXPECT("adv_data(65,102);");
    words[0] = 0x2009; run(words); EXPECT("scan_rsp(65,102);");
    config_btctler_le_roles = 3;
    words[0] = 0x2006; run(words); EXPECT("");
    words[0] = 0x2008; run(words); EXPECT("");
    words[0] = 0x2009; run(words); EXPECT("");
    config_btctler_le_roles = 0;
    /* Advertise / scan enable are not gated. */
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 0x200a; cmd[1] = 1; run(cmd); EXPECT("adv_open;");
    cmd[1] = 0; run(cmd); EXPECT("adv_close;");
    cmd[0] = 0x200c; cmd[1] = 1; cmd[2] = 0x1ff; run(cmd); EXPECT("scan_open;"); CHECK(scan_filter == 0xff);
    cmd[1] = 0; run(cmd); EXPECT("scan_close;");
    log_tag_const_i_HCI_LL = 1;
    cmd[0] = 0x200a; cmd[1] = 5; run(cmd); EXPECT("log2([Info]: [HCI_LL]HCI_LE_SET_ADVERTISE_ENABLE %s\r\n);adv_open;");
    log_tag_const_i_HCI_LL = 0;
    /* Create connection (a pointer in cmd[1]) and cancel. */
    cmd[0] = 0x200d; cmd[1] = 0x1000; run(cmd); EXPECT("init_open(1000);");
    cmd[0] = 0x200e; run(cmd); EXPECT("destroy_ext_init;init_close;");

    /* LE Encrypt: feature bit 0; key = cmd + 1 (16 bytes), plaintext = cmd + 5; reply opcode constant 0x2017. */
    config_btctler_le_features = 1;
    memset(words, 0, sizeof(words));
    for (int i = 0; i < 32; i++) ((u8 *)(words + 1))[i] = i + 1;
    words[0] = 0x2017; run(words); EXPECT("aes(1,17);complete(2017,1c16,0,1020);");
    config_btctler_le_features = 0; run(words); EXPECT("");
    /* LE Rand: eight random bytes, reply opcode from the command. */
    words[0] = 0x2018; run(words); EXPECT("rand(8);complete(2018,1c08,0,5a5a);");

    /* Commands that need a role and the encryption feature. */
    config_btctler_le_roles = 1; config_btctler_le_features = 1;
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 0x2019; cmd[1] = 0x7;
    run(cmd); EXPECT("push(2019);event(15,112,7,1,8217);");
    config_btctler_le_features = 0; run(cmd); EXPECT("");
    config_btctler_le_features = 1; config_btctler_le_roles = 0; run(cmd); EXPECT("");
    config_btctler_le_roles = 1;
    cmd[0] = 0x201a; cmd[1] = 0x0abc; for (int i = 0; i < 4; i++) cmd[2 + i] = 0x11223344 + i;
    run(cmd); EXPECT("push(201a);complete(201a,12,7,2748);");
    CHECK(pushed[0] == 0xbc && pushed[1] == 0x0a && pushed[2] == 0x44 && pushed[3] == 0x33 && pushed[6] == 0x45);
    config_btctler_le_features = 0; run(cmd); EXPECT(""); config_btctler_le_features = 1;
    cmd[0] = 0x201b; cmd[1] = 0xf123; run(cmd); EXPECT("push(201b);complete(201b,12,7,291);");
    config_btctler_le_roles = 0; run(cmd); EXPECT(""); config_btctler_le_roles = 1;
    /* Vendor commands need a role only. */
    config_btctler_le_features = 0;
    cmd[0] = 0xfc83; cmd[1] = 0x33; cmd[2] = 0x44; run(cmd); EXPECT("push(fc83);event(15,112,7,1,64643);");
    CHECK(pushed[0] == 0x33 && pushed[2] == 0x44);
    cmd[0] = 0xfc84; cmd[1] = 0x1111; cmd[2] = 0x2222; run(cmd); EXPECT("push(fc84);event(15,112,7,1,64644);");
    CHECK(pushed[0] == 0x11 && pushed[1] == 0x11 && pushed[2] == 0x22 && pushed[3] == 0x22);
    config_btctler_le_roles = 0; run(cmd); EXPECT("");

    /* Connection update: seven fields, not gated. */
    for (int i = 0; i < 10; i++) cmd[i] = 0x100 * (i + 1) + i;
    cmd[0] = 0x2013; run(cmd); EXPECT("push(2013);event(15,112,7,1,8211);");
    CHECK(pushed[0] == 1 && pushed[1] == 2 && pushed[2] == 2 && pushed[3] == 3 && pushed[12] == 7);
    /* OCF 20: the push argument is the word cmd[1] itself. */
    cmd[0] = 0x2014; cmd[1] = 0x2000; run(cmd); EXPECT("push(2014);event(15,112,7,1,8212);");
    /* P-256 key and DH key: the event first, then the call. */
    cmd[0] = 0x2025; run(cmd); EXPECT("event(15,112,0,1,8229);p256;");
    cmd[0] = 0x2026; cmd[1] = 0x300; cmd[2] = 0x400; run(cmd); EXPECT("event(15,112,0,1,8230);dhkey(300,400);");
    /* Data length: feature bit 5. */
    config_btctler_le_features = 32;
    cmd[0] = 0x2022; cmd[1] = 0x0eef; cmd[2] = 0xfb; cmd[3] = 0x848; run(cmd);
    EXPECT("push(2022);complete(2022,12,7,3823);");
    CHECK(pushed[2] == 0xfb && pushed[4] == 0x48 && pushed[5] == 0x08);
    config_btctler_le_features = 31; run(cmd); EXPECT("");
    /* Set PHY: feature bit 8 or 11; the seven parameter bytes go to the LE 5 handler. */
    cmd[0] = 0x2032; cmd[1] = 0x0042; cmd[2] = 1; cmd[3] = 2; cmd[4] = 3; cmd[5] = 0x0405;
    config_btctler_le_features = 0x100; run(cmd); EXPECT("ble5(2032,7);phy(42000102030504);");
    config_btctler_le_features = 0x800; run(cmd); EXPECT("ble5(2032,7);phy(42000102030504);");
    config_btctler_le_features = 0x6ff; run(cmd); EXPECT("");
    /* Any other opcode goes to the LE 5 handler with cmd[1] as a pointer and cmd[2] as the size. */
    cmd[0] = 0x2040; cmd[1] = 0x500; cmd[2] = 9; run(cmd); EXPECT("ble5(2040,500,9);");
    cmd[0] = 0x3fc1; run(cmd); EXPECT("ble5(3fc1,500,9);");
    return TEST_DONE("btctrler_hci_ll");
}
#endif
