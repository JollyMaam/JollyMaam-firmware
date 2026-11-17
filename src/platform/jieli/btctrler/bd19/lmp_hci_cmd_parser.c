/* BD19 dispatch exports recovered from lmp_hci_cmd_parser.c.o occurrence 1.
 * The baseband export is supplied by lmp_hci_baseband.c. */
#include <string.h>
#include "btctrler/glue.h"
#define HCI_LMP_CODE __attribute__((section(".classic_lmp_code")))
void printf_buf(const u8 *buf, int size);
extern BTCTRLER_CONFIG u8 log_tag_const_i_HCI_LMP;
void log_print(int level, const char *tag, const char *format, ...);
#define HCI_INFO(...) log_print(2, 0, __VA_ARGS__)
#define HCI_LOG(...) do { if (log_tag_const_i_HCI_LMP) HCI_INFO(__VA_ARGS__); } while (0)

int HCI_LMP_CODE hci_lmp_link_control_cmd_handler(u16 opcode, u8 *data, u32 len)
{
    (void)len;
    int msg[6];
    msg[0] = opcode;
    switch (opcode) {
    case 0x0405: /* Create connection */
        if (log_tag_const_i_HCI_LMP) {
            const char *addr=btctler_bd_addr_to_str(data);
            u8 repetition=data[8]; u16 clock=btctler_little_endian_read_16(data + 10,0);
            HCI_INFO("[Info]: [HCI_LMP]HCI_CREATE_CONNECTION : %s, 0x%x, 0x%x\r\n",
                addr,repetition,clock);
        }
        hci_send_event(15, "112", 0, 1, opcode);
        link_page_try_start_enable(0, data, data[8], 0);
        return 1;
    case 0x0401: {
        if (!CONFIG_BREDR_INQUIRY) return 1;
        if (log_tag_const_i_HCI_LMP) {
            u32 log_lap=btctler_little_endian_read_24(data,0);
            u8 log_length=data[3], log_count=data[4];
            HCI_INFO("[Info]: [HCI_LMP]HCI_INQUIRY : 0x%x / 0x%x / 0x%x\r\n",
                log_lap,log_length,log_count);
        }
        u32 lap = btctler_little_endian_read_24(data, 0);
        hci_send_event(15, "112", 0, 1, opcode);
        link_inquiry_enable((u8 *)&lap, data[3] * 1280, data[4], 0);
        return 1;
    }
    case 0x0402:
        if (!CONFIG_BREDR_INQUIRY) return 1;
        HCI_LOG("[Info]: [HCI_LMP]HCI_INQUIRY_CANCEL\r\n");
        hci_send_event_cmd_complete(opcode, "1", 0);
        link_inquiry_disable();
        hci_send_event(1, "1", 0);
        return 1;
    case 0x0409:
        if (log_tag_const_i_HCI_LMP) {
            const char *addr=btctler_bd_addr_to_str(data); u8 role=data[6];
            HCI_INFO("[Info]: [HCI_LMP]HCI_ACCEPT_CONNECTION_REQUEST : %s, %d\r\n",
                addr,role);
        }
        hci_send_event(15, "112", 0, 1, opcode);
        msg[1] = data[6]; lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x0411:
        HCI_LOG("[Info]: [HCI_LMP]HCI_AUTHENTICATION_REQUESTED\r\n");
        hci_send_event(15, "112", 0, 1, opcode);
        lmp_hci_cmd_handler_to_conn(data, msg); return 1;
    case 0x0413:
        if (log_tag_const_i_HCI_LMP) HCI_INFO("[Info]: [HCI_LMP]HCI_SET_CONNECTION_ENCRYPTION : %d\r\n",data[2]);
        hci_send_event(15, "112", 0, 1, opcode);
        msg[1] = data[2]; lmp_hci_cmd_handler_to_conn(data, msg); return 1;
    case 0x040b:
        HCI_LOG("[Info]: [HCI_LMP]HCI_LINK_KEY_REQUEST_REPLY\r\n");
        __builtin_memcpy(&msg[1], data + 6, 16);
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x040c: case 0x040e:
        if (opcode == 0x040c) HCI_LOG("[Info]: [HCI_LMP]HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY\r\n");
        else HCI_LOG("[Info]: [HCI_LMP]HCI_PIN_CODE_REQUEST_NEGATIVE_REPLY\r\n");
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x040d:
        HCI_LOG("[Info]: [HCI_LMP]HCI_PIN_CODE_REQUEST_REPLY\r\n");
        msg[1] = data[6]; __builtin_memcpy(&msg[2], data + 7, 16);
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x042b:
        HCI_LOG("[Info]: [HCI_LMP]HCI_IO_CAPABILITY_REQUEST_REPLY\r\n");
        msg[1] = data[6]; msg[2] = data[7]; msg[3] = data[8];
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x042c:
        HCI_LOG("[Info]: [HCI_LMP]HCI_USER_CONFIRMATION_REQUEST_REPLY\r\n");
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x042d: case 0x042e: case 0x042f: case 0x0c60:
        if (opcode == 0x042d) HCI_LOG("[Info]: [HCI_LMP]HCI_USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY\r\n");
        else if (opcode == 0x042e) HCI_LOG("[Info]: [HCI_LMP] HCI_USER_PASSKEY_REQUEST_REPLY\r\n");
        else if (opcode == 0x042f) HCI_LOG("[Info]: [HCI_LMP]  HCI_USER_PASSKEY_REQUEST_NEGATIVE_REPLY\r\n");
        else if (log_tag_const_i_HCI_LMP) {
            HCI_INFO("[Info]: [HCI_LMP] HCI_SEND_KEYPRESS_NOTIFICATION\r\n");
            printf_buf(data,4);
        }
        lmp_hci_cmd_addr_to_conn(data, msg);
        hci_send_event_cmd_complete(opcode, "1A", 0, data); return 1;
    case 0x0419:
        HCI_LOG("[Info]: [HCI_LMP]HCI_REMOTE_NAME_REQUEST\r\n");
        hci_send_event(15, "112", 0, 1, opcode);
        { u8 repetition = data[6];
          u16 clock = btctler_little_endian_read_16(data, 8);
          lmp_hci_remote_name_request(data, repetition, clock); }
        return 1;
    case 0x041a:
        hci_send_event_cmd_complete(opcode, "1", 0); link_page_disable(4); return 1;
    case 0x041b: case 0x041c: {
        HCI_LOG(opcode==0x041b ? "[Info]: [HCI_LMP]HCI_READ_REMOTE_SUPPORTED_FEATURES\r\n" :
                                "[Info]: [HCI_LMP]HCI_READ_REMOTE_EXTENDED_FEATURES\r\n");
        u16 h = btctler_little_endian_read_16(data, 0) & 0x0fff;
        if (!lmp_conn_for_handle(h)) { hci_send_event(15, "112", 2, 1, opcode); return 1; }
        hci_send_event(15, "112", 0, 1, opcode);
        if (opcode == 0x041b) lmp_hci_read_remote_supported_features(h);
        else lmp_hci_read_remote_extended_features(h);
        return 1;
    }
    case 0x041f:
        HCI_LOG("[Info]: [HCI_LMP]HCI_READ_CLOCK_OFFSET\r\n");
        { u16 handle = btctler_little_endian_read_16(data, 0) & 0x0fff;
          hci_send_event(15, "112", 0, 1, opcode);
          lmp_hci_read_clock_offset(handle); }
        return 1;
    case 0x0428:
        if (!CONFIG_LMP_MASTER_ESCO_ENABLE) return 1;
        if (log_tag_const_i_HCI_LMP) {
            HCI_INFO("[Info]: [HCI_LMP]HCI_SETUP_SYNCHRONOUS_CONNECTION\r\n");
            u16 lh=btctler_little_endian_read_16(data,0);
            u32 tx=btctler_little_endian_read_32(data,2), rx=btctler_little_endian_read_32(data,6);
            u16 lat=btctler_little_endian_read_16(data,10), voice=btctler_little_endian_read_16(data,12);
            u8 effort=data[14]; u16 packets=btctler_little_endian_read_16(data,15);
            HCI_INFO("[Info]: [HCI_LMP]Connection_Handle = 0x%x\n\t\t\t\t\tTransmit_Bandwidth = %d byte\n\t\t\t\t\tReceive_Bandwidth = %d byte\n\t\t\t\t\tMax_Latency = %d ms\n\t\t\t\t\tVoice_Setting = 0x%x\n\t\t\t\t\tRetransmission_Effort = 0x%x\n\t\t\t\t\tPacket Type = 0x%x\n\r\n",
                lh,tx,rx,lat,voice,effort,packets);
        }
        hci_send_event(15, "112", 0, 1, opcode);
        { u16 handle = btctler_little_endian_read_16(data, 0);
          u32 tx = btctler_little_endian_read_32(data, 2);
          u32 rx = btctler_little_endian_read_32(data, 6);
          u16 latency = btctler_little_endian_read_16(data, 10);
          u16 voice = btctler_little_endian_read_16(data, 12);
          u8 effort = data[14];
          u16 packets = btctler_little_endian_read_16(data, 15);
          lmp_hci_setup_sync_connection(handle, tx, rx, latency, voice, effort, packets); }
        return 1;
    case 0x0429: case 0x042a:
        if (!CONFIG_LMP_MASTER_ESCO_ENABLE) return 1;
        msg[1] = data[2]; lmp_hci_cmd_handler_to_conn(data, msg);
        if (opcode == 0x0429) hci_send_event(15, "112", 0, 1, opcode);
        return 1;
    default: return 0;
    }
}

int HCI_LMP_CODE hci_lmp_link_policy_cmd_handler(u16 opcode, u8 *data, u32 len)
{
    (void)len;
    switch (opcode) {
    case 0x0803: case 0x0804: {
        HCI_LOG(opcode==0x0803 ? "[Info]: [HCI_LMP]HCI_SNIFF_MODE\r\n" :
                                "[Info]: [HCI_LMP]HCI_EXIT_SNIFF_MODE\r\n");
        struct lmp_connection *c = lmp_conn_for_handle(btctler_little_endian_read_16(data,0));
        if (!c) return 1;
        hci_send_event(15, "112", 0, 1, opcode);
        if (opcode == 0x0803)
        { u16 maximum = btctler_little_endian_read_16(data, 2);
          u16 minimum = btctler_little_endian_read_16(data, 4);
          u16 attempt = btctler_little_endian_read_16(data, 6);
          u16 timeout = btctler_little_endian_read_16(data, 8);
          lmp_hci_sniff_mode(c, maximum, minimum, attempt, timeout); }
        else lmp_hci_exit_sniff_mode(c);
        return 1;
    }
    case 0x0809:
        HCI_LOG("[Info]: [HCI_LMP]HCI_ROLE_DISCOVERY\r\n");
        lmp_hci_role_discovery(btctler_little_endian_read_16(data,0)&0xfff); return 1;
    case 0x080b:
        HCI_LOG("[Info]: [HCI_LMP]HCI_SWITCH_ROLE\r\n");
        lmp_hci_switch_role_command(data,data[6]); return 1;
    case 0x080c:
        HCI_LOG("[Info]: [HCI_LMP]HCI_READ_LINK_POLICY_SETTINGS\r\n");
        lmp_hci_read_link_policy_settings(btctler_little_endian_read_16(data,0)&0xfff); return 1;
    case 0x080d:
        if (log_tag_const_i_HCI_LMP) HCI_INFO("[Info]: [HCI_LMP]HCI_WRITE_LINK_POLICY_SETTINGS : 0x%x\r\n",btctler_little_endian_read_16(data,2));
        { u16 handle = btctler_little_endian_read_16(data, 0) & 0xfff;
          u16 policy = btctler_little_endian_read_16(data, 2);
          lmp_hci_write_link_policy_settings(handle, policy); }
        return 1;
    case 0x080e:
        if (log_tag_const_i_HCI_LMP) {
            HCI_INFO("[Info]: [HCI_LMP]HCI_READ_DEFAULT_LINK_POLICY_SETTINGS\r\n");
            HCI_INFO("[Info]: [HCI_LMP]policy = 0x%x\r\n",gbredr_local_dev.default_link_policy_settings);
        }
        hci_send_event_cmd_complete(opcode,"12",0,gbredr_local_dev.default_link_policy_settings); return 1;
    case 0x080f:
        if (log_tag_const_i_HCI_LMP) {
            HCI_INFO("[Info]: [HCI_LMP]HCI_WRITE_DEFAULT_LINK_POLICY_SETTINGS\r\n");
            HCI_INFO("[Info]: [HCI_LMP]policy = 0x%x\r\n",btctler_little_endian_read_16(data,0));
        }
        gbredr_local_dev.default_link_policy_settings=(u8)btctler_little_endian_read_16(data,0);
        hci_send_event_cmd_complete(opcode,"1",0); return 1;
    case 0x0807: {
        u16 h;
        if (log_tag_const_i_HCI_LMP) {
            HCI_INFO("[Info]: [HCI_LMP]HCI_QOS_SETUP is NOT supported\r\n",opcode);
            h = btctler_little_endian_read_16(data, 0);
            u8 lf=data[2], st=data[3];
            u32 l1=btctler_little_endian_read_32(data,4), l2=btctler_little_endian_read_32(data,8);
            u32 l3=btctler_little_endian_read_32(data,12), l4=btctler_little_endian_read_32(data,16);
            HCI_INFO("[Info]: [HCI_LMP]Flags : 0x%x /                Service_Type: 0x%x /                Token_Rate : 0x%x/                Peak_Bandwidth : 0x%x/                Latency : 0x%x/                Delay_Variation : 0x%x\r\n\r\n",
                lf,st,l1,l2,l3,l4);
        } else h = btctler_little_endian_read_16(data, 0);
        hci_send_event(15,"112",0,1,opcode);
        u8 a=data[2], b=data[3];
        u32 x=btctler_little_endian_read_32(data,4), y=btctler_little_endian_read_32(data,8);
        u32 z=btctler_little_endian_read_32(data,12), w=btctler_little_endian_read_32(data,16);
        hci_send_event(1,"1H114444",0,h&0xfff,a,b,x,y,z,w); return 1;
    }
    case 0x0805: case 0x0806: case 0x0810: case 0x0811:
        if (log_tag_const_i_HCI_LMP) HCI_INFO("[Info]: [HCI_LMP]OCF:0x%x is NOT supported\r\n",opcode);
        return 1;
    default: return 0;
    }
}

int HCI_LMP_CODE hci_lmp_informational_parameters_cmd_handler(u16 opcode,const u8 *data,u32 len)
{ (void)data; (void)len; if(opcode!=0x1005)return 0; if(log_tag_const_i_HCI_LMP) HCI_LOG("[Info]: [HCI_LMP]HCI_READ_BUFFER_SIZE : acl(%d) / sco(%d) / acl total num(%d) / sco total num(%d)\r\n",640,64,10,8); hci_send_event_cmd_complete(opcode,"12122",0,640,64,10,8); return 1; }

int HCI_LMP_CODE hci_lmp_status_parameters_cmd_handler(u16 opcode,const u8 *data,u32 len)
{
    (void)len;
    switch(opcode){
    case 0x1403:
        HCI_LOG("[Info]: [HCI_LMP]HCI_READ_LINK_QUALITY(debugging) : ret 100\r\n");
        hci_send_event_cmd_complete(opcode,"1H1",0,btctler_little_endian_read_16(data,0)&0xfff,100); return 1;
    case 0x1405:
        HCI_LOG("[Info]: [HCI_LMP]HCI_READ_RSSI(debugging) : ret 0\r\n");
        hci_send_event_cmd_complete(opcode,"1H1",0,btctler_little_endian_read_16(data,0)&0xfff,0); return 1;
    case 0x1406: HCI_LOG("[Info]: [HCI_LMP]HCI_READ_AFH_CHANNEL_MAP\r\n"); return 1;
    case 0x1408: HCI_LOG("[Info]: [HCI_LMP]HCI_READ_ENCRYPTION_KEY_SIZE\r\n"); return 1;
    default:return 0;
    }
}

int HCI_LMP_CODE hci_lmp_vendor_cmd_handler(u16 opcode,u8 *data,u32 len)
{ (void)len; if((opcode&0x3ff)!=1)return 0; HCI_LOG("[Info]: [HCI_LMP]VENDOR_BREDR_FCC\r\n"); bredr_fcc_set_param(data); hci_send_event_cmd_complete(opcode,"1",0); return 1; }

int HCI_LMP_CODE hci_lmp_testing_cmd_handler(u16 opcode,const u8 *data,u32 len)
{ (void)opcode; (void)data; (void)len; return 0; }

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
u8 log_tag_const_i_HCI_LMP;
int CONFIG_BREDR_INQUIRY = 1, CONFIG_LMP_MASTER_ESCO_ENABLE = 1;
struct bredr_local_device gbredr_local_dev;
static u8 payload[256];
static struct lmp_connection connection;
static int events, event_code, event_opcode, values[8], logs, dumps, found;
static const char *event_format;
static int mutate_status, mutate_complete, mutate_log_gate, mutate_qos_logs;
static int helper_calls, helper_handle, helper_args[8], copied[6], page_rep;
static char trace[64];
static unsigned int trace_pos;
static const u8 *read_ptrs[32];
static int read_offs[32], read_count, mutate_reader;
static void mark(char c) { CHECK(trace_pos < sizeof(trace)-1); trace[trace_pos++] = c; trace[trace_pos] = 0; }
u16 btctler_little_endian_read_16(const u8 *p, int off)
{
    if (read_count < 32) { read_ptrs[read_count] = p; read_offs[read_count] = off; } read_count++;
    if (mutate_reader && off == 8) payload[6] ^= 0x55;
    return p[off] | (u16)p[off+1] << 8;
}
u32 btctler_little_endian_read_24(const u8 *p, int off)
{ return p[off] | (u32)p[off+1]<<8 | (u32)p[off+2]<<16; }
u32 btctler_little_endian_read_32(const u8 *p, int off)
{ return p[off] | (u32)p[off+1]<<8 | (u32)p[off+2]<<16 | (u32)p[off+3]<<24; }
char *btctler_bd_addr_to_str(const u8 *addr) { CHECK(addr == payload); return "address"; }
void log_print(int level, const char *tag, const char *fmt, ...)
{
    CHECK(level == 2 && !tag); logs++;
    if (mutate_log_gate) log_tag_const_i_HCI_LMP = 0;
    if (mutate_qos_logs && strstr(fmt, "NOT supported")) payload[0] = 0x65;
    if (mutate_qos_logs && strstr(fmt, "Flags :")) payload[4] = 0x99;
}
void printf_buf(const u8 *p, int size) { CHECK(p == payload && size == 4); dumps++; }
static void capture_message(u8 *p, int *msg, char kind)
{
    CHECK(p == payload); mark(kind); helper_calls++;
    unsigned int n;
    switch (msg[0]) {
    case 0x0409: case 0x0413: case 0x0429: case 0x042a: n = 2; break;
    case 0x040b: n = 5; break;
    case 0x040d: n = 6; break;
    case 0x042b: n = 4; break;
    default: n = 1; break;
    }
    /* Never read SDK message words that have not been initialized. */
    __builtin_memcpy(copied, msg, n * sizeof(*msg));
}
void lmp_hci_cmd_addr_to_conn(u8 *p, int *msg) { capture_message(p, msg, 'A'); }
void lmp_hci_cmd_handler_to_conn(u8 *p, int *msg) { capture_message(p, msg, 'H'); }
void link_page_try_start_enable(u8 remote_name, u8 *addr, u8 repetition, int clock)
{ CHECK(!remote_name && addr == payload && !clock); helper_calls++; page_rep = repetition; mark('P'); }
int link_inquiry_enable(u8 *lap, int timeout, u8 count, int (*done)(struct link_inquiry_result *))
{ CHECK(!done); helper_args[0] = lap[0] | lap[1]<<8 | lap[2]<<16; helper_args[1] = timeout; helper_args[2] = count; helper_calls++; mark('I'); return -1; }
void link_inquiry_disable(void) { helper_calls++; mark('D'); }
void link_page_disable(int reason) { CHECK(reason == 4); helper_calls++; mark('D'); }
struct lmp_connection *lmp_conn_for_handle(int handle)
{ helper_handle = handle; return found ? &connection : NULL; }
void lmp_hci_read_remote_supported_features(u16 h) { helper_handle = h; helper_calls++; mark('R'); }
void lmp_hci_read_remote_extended_features(u16 h) { helper_handle = h; helper_calls++; mark('R'); }
void lmp_hci_remote_name_request(u8 *addr, u8 repetition, u16 clock)
{ CHECK(addr == payload); helper_args[0] = repetition; helper_args[1] = clock; helper_calls++; mark('R'); }
void lmp_hci_read_clock_offset(u16 h) { helper_handle = h; helper_calls++; mark('R'); }
void lmp_hci_setup_sync_connection(u16 h, u32 tx, u32 rx, u16 lat, u16 voice, u8 effort, u16 type)
{ helper_handle = h; helper_args[0] = tx; helper_args[1] = rx; helper_args[2] = lat; helper_args[3] = voice; helper_args[4] = effort; helper_args[5] = type; helper_calls++; mark('R'); }
void lmp_hci_sniff_mode(struct lmp_connection *c, u16 max, u16 min, u16 attempt, u16 timeout)
{ CHECK(c == &connection); helper_args[0] = max; helper_args[1] = min; helper_args[2] = attempt; helper_args[3] = timeout; helper_calls++; mark('R'); }
void lmp_hci_exit_sniff_mode(struct lmp_connection *c)
{ CHECK(c == &connection); helper_calls++; mark('R'); }
void lmp_hci_role_discovery(u16 h) { helper_handle = h; helper_calls++; mark('R'); }
void lmp_hci_switch_role_command(u8 *p, u8 role) { CHECK(p == payload); helper_args[0] = role; helper_calls++; mark('R'); }
void lmp_hci_read_link_policy_settings(u16 h) { helper_handle = h; helper_calls++; mark('R'); }
void lmp_hci_write_link_policy_settings(u16 h, u16 policy) { helper_handle = h; helper_args[0] = policy; helper_calls++; mark('R'); }
void bredr_fcc_set_param(u8 *p) { CHECK(p == payload); helper_calls++; mark('F'); }
int hci_send_event(int code, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    events++; event_code = code; event_format = fmt; mark(code == 15 ? 'S' : 'E');
    int n = !strcmp(fmt, "112") ? 3 : !strcmp(fmt, "1H114444") ? 8 : 1;
    for (int i = 0; i < n; i++) values[i] = va_arg(ap, int);
    va_end(ap);
    if (code == 15 && mutate_status) { payload[0] = 0x99; payload[2] = 0x66; payload[8] = 0xaa; }
    return -1;
}
int hci_send_event_cmd_complete(int opcode, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    events++; event_code = 14; event_opcode = opcode; event_format = fmt; mark('C');
    values[0] = va_arg(ap, int);
    if (!strcmp(fmt, "1A")) CHECK(va_arg(ap, u8 *) == payload);
    else {
        int n = !strcmp(fmt, "12122") ? 5 : !strcmp(fmt, "1H1") ? 3 : !strcmp(fmt, "12") ? 2 : 1;
        for (int i = 1; i < n; i++) values[i] = va_arg(ap, int);
    }
    va_end(ap);
    if (mutate_complete) payload[6] ^= 0xff;
    return -1;
}
static void reset(int logging)
{
    for (unsigned int i = 0; i < sizeof(payload); i++) payload[i] = i + 1;
    payload[0] = 0x34; payload[1] = 0xf2;
    events = logs = dumps = helper_calls = read_count = 0; trace_pos = 0; trace[0] = 0;
    mutate_status = mutate_complete = mutate_log_gate = mutate_qos_logs = mutate_reader = 0;
    log_tag_const_i_HCI_LMP = logging; found = 1;
    CONFIG_BREDR_INQUIRY = CONFIG_LMP_MASTER_ESCO_ENABLE = 1;
    __builtin_memset(copied, 0, sizeof(copied));
}
int main(void)
{
    static const u16 control[] = {0x0405,0x0401,0x0402,0x0409,0x0411,0x0413,
        0x042b,0x042c,0x042d,0x0c60,0x042e,0x042f,0x041b,0x041c,0x0419,
        0x041a,0x040b,0x040c,0x040d,0x040e,0x041f,0x0428,0x0429,0x042a};
    static const u16 policy[] = {0x0803,0x0804,0x080e,0x080f,0x0809,0x080b,
        0x080c,0x080d,0x0807,0x0805,0x0806,0x0810,0x0811};
    for (int logging = 0; logging < 2; logging++) {
        for (unsigned int i = 0; i < sizeof(control)/sizeof(*control); i++) {
            reset(logging); CHECK(hci_lmp_link_control_cmd_handler(control[i], payload, 0) == 1);
            CHECK(events >= (control[i] == 0x042a ? 0 : 1));
        }
        for (unsigned int i = 0; i < sizeof(policy)/sizeof(*policy); i++) {
            reset(logging); CHECK(hci_lmp_link_policy_cmd_handler(policy[i], payload, 0) == 1);
        }
        reset(logging); CHECK(hci_lmp_informational_parameters_cmd_handler(0x1005, NULL, 0) == 1);
        CHECK(events == 1 && event_code == 14 && event_opcode == 0x1005 && !strcmp(event_format,"12122"));
        CHECK(values[0] == 0 && values[1] == 640 && values[2] == 64 && values[3] == 10 && values[4] == 8);
        reset(logging); CHECK(hci_lmp_status_parameters_cmd_handler(0x1403, payload, 0) == 1);
        CHECK(event_code == 14 && values[1] == 0x234 && values[2] == 100);
        reset(logging); CHECK(hci_lmp_status_parameters_cmd_handler(0x1405, payload, 0) == 1 && values[2] == 0);
        reset(logging); CHECK(hci_lmp_status_parameters_cmd_handler(0x1406, NULL, 0) == 1 && !events);
        CHECK(hci_lmp_status_parameters_cmd_handler(0x1408, NULL, 0) == 1 && !events);
        reset(logging); CHECK(hci_lmp_vendor_cmd_handler(0xfc01, payload, 0) == 1 && !strcmp(trace,"FC"));
        CHECK(!hci_lmp_vendor_cmd_handler(0xfc02, NULL, 0));
        reset(logging); CHECK(hci_lmp_testing_cmd_handler(0x1801, NULL, 0) == 0 && !events);
        CHECK(hci_lmp_link_control_cmd_handler(0xffff, NULL, 0) == 0);
        CHECK(hci_lmp_link_policy_cmd_handler(0xffff, NULL, 0) == 0);
        CHECK(hci_lmp_informational_parameters_cmd_handler(0xffff, NULL, 0) == 0);
        CHECK(hci_lmp_status_parameters_cmd_handler(0xffff, NULL, 0) == 0);
        reset(logging); mutate_status = 1; hci_lmp_link_control_cmd_handler(0x0405, payload, 0);
        CHECK(!strcmp(trace,"SP") && page_rep == 0xaa);
        if (logging) CHECK(read_ptrs[0] == payload + 10 && read_offs[0] == 0);
        reset(logging); mutate_status = 1; hci_lmp_link_control_cmd_handler(0x041f, payload, 0);
        CHECK(!strcmp(trace,"SR") && helper_handle == 0x234);
        reset(logging); mutate_reader = 1; hci_lmp_link_control_cmd_handler(0x0419, payload, 0);
        CHECK(helper_args[0] == 7 && payload[6] == (7 ^ 0x55));
        reset(logging); mutate_complete = 1; hci_lmp_link_control_cmd_handler(0x040b, payload, 0);
        CHECK(!strcmp(trace,"CA") && copied[0] == 0x040b);
        CHECK((u32)copied[1] == 0x0a090807 && payload[6] == (7 ^ 0xff));
        reset(logging); hci_lmp_link_control_cmd_handler(0x042e, payload, 0);
        CHECK(!strcmp(trace,"AC") && copied[0] == 0x042e);
        reset(logging); hci_lmp_link_control_cmd_handler(0x0c60, payload, 0);
        CHECK(dumps == logging && !strcmp(trace,"AC"));
        reset(logging); found = 0; hci_lmp_link_control_cmd_handler(0x041b, payload, 0);
        CHECK(!helper_calls && events == 1 && values[0] == 2);
        reset(logging); found = 0; hci_lmp_link_policy_cmd_handler(0x0803, payload, 0);
        CHECK(helper_handle == 0xf234 && !events && !helper_calls);
        reset(logging); mutate_status = 1; hci_lmp_link_policy_cmd_handler(0x0803, payload, 0);
        CHECK(helper_handle == 0xf234 && helper_args[0] == 0x466);
        reset(logging); mutate_status = mutate_qos_logs = 1;
        hci_lmp_link_policy_cmd_handler(0x0807, payload, 0);
        CHECK(events == 2 && event_code == 1 && values[1] == (logging ? 0x265 : 0x234));
        CHECK(values[2] == 0x66 && values[4] == (logging ? 0x08070699 : 0x08070605));
        CHECK(values[5] == 0x0c0b0aaa);
        reset(logging); CONFIG_BREDR_INQUIRY = CONFIG_LMP_MASTER_ESCO_ENABLE = 0;
        hci_lmp_link_control_cmd_handler(0x0401, NULL, 0); hci_lmp_link_control_cmd_handler(0x0428, NULL, 0);
        CHECK(!events && !helper_calls && !logs);
    }
    reset(1); mutate_log_gate = 1; hci_lmp_link_policy_cmd_handler(0x080e, payload, 0);
    CHECK(logs == 2 && !log_tag_const_i_HCI_LMP);
    reset(1); mutate_log_gate = 1; hci_lmp_link_control_cmd_handler(0x0428, payload, 0);
    CHECK(logs == 2 && !log_tag_const_i_HCI_LMP && helper_calls == 1);
    return TEST_DONE("bd19_lmp_hci_dispatch");
}
#endif
