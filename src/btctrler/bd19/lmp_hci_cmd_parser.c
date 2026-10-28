/* BD19 dispatch exports recovered from lmp_hci_cmd_parser.c.o occurrence 1.
 * The baseband export is intentionally not included at this checkpoint. */
#include <string.h>
#include "btctrler/glue.h"
#define HCI_LMP_CODE __attribute__((section(".classic_lmp_code")))
void printf_buf(const u8 *buf, int size);
extern const u8 log_tag_const_i_HCI_LMP;
extern int log_print(int level, const char *tag, const char *format, ...);
#define HCI_LOG(...) do { if (log_tag_const_i_HCI_LMP) log_print(2, 0, __VA_ARGS__); } while (0)

int HCI_LMP_CODE hci_lmp_link_control_cmd_handler(u16 opcode, u8 *data, u32 len)
{
    (void)len;
    int msg[6];
    msg[0] = opcode;
    switch (opcode) {
    case 0x0405: /* Create connection */
        if (log_tag_const_i_HCI_LMP) {
            const char *addr=btctler_bd_addr_to_str(data);
            u8 repetition=data[8]; u16 clock=btctler_little_endian_read_16(data,10);
            HCI_LOG("[Info]: [HCI_LMP]HCI_CREATE_CONNECTION : %s, 0x%x, 0x%x\r\n",
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
            HCI_LOG("[Info]: [HCI_LMP]HCI_INQUIRY : 0x%x / 0x%x / 0x%x\r\n",
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
            HCI_LOG("[Info]: [HCI_LMP]HCI_ACCEPT_CONNECTION_REQUEST : %s, %d\r\n",
                addr,role);
        }
        hci_send_event(15, "112", 0, 1, opcode);
        msg[1] = data[6]; lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x0411:
        HCI_LOG("[Info]: [HCI_LMP]HCI_AUTHENTICATION_REQUESTED\r\n");
        hci_send_event(15, "112", 0, 1, opcode);
        lmp_hci_cmd_handler_to_conn(data, msg); return 1;
    case 0x0413:
        if (log_tag_const_i_HCI_LMP) HCI_LOG("[Info]: [HCI_LMP]HCI_SET_CONNECTION_ENCRYPTION : %d\r\n",data[2]);
        hci_send_event(15, "112", 0, 1, opcode);
        msg[1] = data[2]; lmp_hci_cmd_handler_to_conn(data, msg); return 1;
    case 0x040b:
        HCI_LOG("[Info]: [HCI_LMP]HCI_LINK_KEY_REQUEST_REPLY\r\n");
        memcpy(&msg[1], data + 6, 16);
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x040c: case 0x040e:
        if (opcode == 0x040c) HCI_LOG("[Info]: [HCI_LMP]HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY\r\n");
        else HCI_LOG("[Info]: [HCI_LMP]HCI_PIN_CODE_REQUEST_NEGATIVE_REPLY\r\n");
        hci_send_event_cmd_complete(opcode, "1A", 0, data);
        lmp_hci_cmd_addr_to_conn(data, msg); return 1;
    case 0x040d:
        HCI_LOG("[Info]: [HCI_LMP]HCI_PIN_CODE_REQUEST_REPLY\r\n");
        msg[1] = data[6]; memcpy(&msg[2], data + 7, 16);
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
            HCI_LOG("[Info]: [HCI_LMP] HCI_SEND_KEYPRESS_NOTIFICATION\r\n");
            printf_buf(data,4);
        }
        lmp_hci_cmd_addr_to_conn(data, msg);
        hci_send_event_cmd_complete(opcode, "1A", 0, data); return 1;
    case 0x0419:
        HCI_LOG("[Info]: [HCI_LMP]HCI_REMOTE_NAME_REQUEST\r\n");
        hci_send_event(15, "112", 0, 1, opcode);
        lmp_hci_remote_name_request(data, data[6], btctler_little_endian_read_16(data, 8)); return 1;
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
        hci_send_event(15, "112", 0, 1, opcode);
        lmp_hci_read_clock_offset(btctler_little_endian_read_16(data, 0) & 0x0fff); return 1;
    case 0x0428:
        if (!CONFIG_LMP_MASTER_ESCO_ENABLE) return 1;
        HCI_LOG("[Info]: [HCI_LMP]HCI_SETUP_SYNCHRONOUS_CONNECTION\r\n");
        if (log_tag_const_i_HCI_LMP) {
            u16 lh=btctler_little_endian_read_16(data,0);
            u32 tx=btctler_little_endian_read_32(data,2), rx=btctler_little_endian_read_32(data,6);
            u16 lat=btctler_little_endian_read_16(data,10), voice=btctler_little_endian_read_16(data,12);
            u8 effort=data[14]; u16 packets=btctler_little_endian_read_16(data,15);
            HCI_LOG("[Info]: [HCI_LMP]Connection_Handle = 0x%x\n\t\t\t\t\tTransmit_Bandwidth = %d byte\n\t\t\t\t\tReceive_Bandwidth = %d byte\n\t\t\t\t\tMax_Latency = %d ms\n\t\t\t\t\tVoice_Setting = 0x%x\n\t\t\t\t\tRetransmission_Effort = 0x%x\n\t\t\t\t\tPacket Type = 0x%x\n\r\n",
                lh,tx,rx,lat,voice,effort,packets);
        }
        hci_send_event(15, "112", 0, 1, opcode);
        lmp_hci_setup_sync_connection(btctler_little_endian_read_16(data,0),
          btctler_little_endian_read_32(data,2), btctler_little_endian_read_32(data,6),
          btctler_little_endian_read_16(data,10), btctler_little_endian_read_16(data,12),
          data[14], btctler_little_endian_read_16(data,15)); return 1;
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
            lmp_hci_sniff_mode(c, btctler_little_endian_read_16(data,2), btctler_little_endian_read_16(data,4),
                btctler_little_endian_read_16(data,6), btctler_little_endian_read_16(data,8));
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
        if (log_tag_const_i_HCI_LMP) HCI_LOG("[Info]: [HCI_LMP]HCI_WRITE_LINK_POLICY_SETTINGS : 0x%x\r\n",btctler_little_endian_read_16(data,2));
        lmp_hci_write_link_policy_settings(btctler_little_endian_read_16(data,0)&0xfff,btctler_little_endian_read_16(data,2)); return 1;
    case 0x080e:
        if (log_tag_const_i_HCI_LMP) {
            HCI_LOG("[Info]: [HCI_LMP]HCI_READ_DEFAULT_LINK_POLICY_SETTINGS\r\n");
            HCI_LOG("[Info]: [HCI_LMP]policy = 0x%x\r\n",gbredr_local_dev.default_link_policy_settings);
        }
        hci_send_event_cmd_complete(opcode,"12",0,gbredr_local_dev.default_link_policy_settings); return 1;
    case 0x080f:
        if (log_tag_const_i_HCI_LMP) {
            HCI_LOG("[Info]: [HCI_LMP]HCI_WRITE_DEFAULT_LINK_POLICY_SETTINGS\r\n");
            HCI_LOG("[Info]: [HCI_LMP]policy = 0x%x\r\n",btctler_little_endian_read_16(data,0));
        }
        gbredr_local_dev.default_link_policy_settings=(u8)btctler_little_endian_read_16(data,0);
        hci_send_event_cmd_complete(opcode,"1",0); return 1;
    case 0x0807: {
        if (log_tag_const_i_HCI_LMP) {
            HCI_LOG("[Info]: [HCI_LMP]HCI_QOS_SETUP is NOT supported\r\n",opcode);
            u8 lf=data[2], st=data[3];
            u32 l1=btctler_little_endian_read_32(data,4), l2=btctler_little_endian_read_32(data,8);
            u32 l3=btctler_little_endian_read_32(data,12), l4=btctler_little_endian_read_32(data,16);
            HCI_LOG("[Info]: [HCI_LMP]Flags : 0x%x /                Service_Type: 0x%x /                Token_Rate : 0x%x/                Peak_Bandwidth : 0x%x/                Latency : 0x%x/                Delay_Variation : 0x%x\r\n\r\n",
                lf,st,l1,l2,l3,l4);
        }
        hci_send_event(15,"112",0,1,opcode);
        u16 h=btctler_little_endian_read_16(data,0); u8 a=data[2], b=data[3];
        u32 x=btctler_little_endian_read_32(data,4), y=btctler_little_endian_read_32(data,8);
        u32 z=btctler_little_endian_read_32(data,12), w=btctler_little_endian_read_32(data,16);
        hci_send_event(1,"1H114444",0,h&0xfff,a,b,x,y,z,w); return 1;
    }
    case 0x0805: case 0x0806: case 0x0810: case 0x0811:
        if (log_tag_const_i_HCI_LMP) HCI_LOG("[Info]: [HCI_LMP]OCF:0x%x is NOT supported\r\n",opcode);
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
#ifndef TEST_LOG_ENABLED
#define TEST_LOG_ENABLED 0
#endif
static int ev, evcode, evop, val[8];
static const char *fmt;
const u8 log_tag_const_i_HCI_LMP = TEST_LOG_ENABLED;
static u8 *qos_payload;
static int logs;
u16 btctler_little_endian_read_16(const u8 *p, int n)
{ return p[n] | ((u16)p[n+1] << 8); }
u32 btctler_little_endian_read_32(const u8 *p, int n)
{ return p[n] | ((u32)p[n+1]<<8) | ((u32)p[n+2]<<16) | ((u32)p[n+3]<<24); }
int log_print(int level,const char *tag,const char *f,...)
{
    (void)level; (void)tag; logs++;
    if (qos_payload && strstr(f,"Flags :")) qos_payload[4]=0x99;
    return 0;
}
int hci_send_event(int code,const char *f,...)
{
    if (code==15 && qos_payload) qos_payload[8]=0x77;
    va_list ap; va_start(ap,f); ev++; evcode=code; fmt=f;
    if (!strcmp(f,"112")) { val[0]=va_arg(ap,int); val[1]=va_arg(ap,int); val[2]=va_arg(ap,int); }
    else if (!strcmp(f,"1H114444")) for(int i=0;i<8;i++) val[i]=va_arg(ap,int);
    va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int op,const char *f,...)
{
    va_list ap; va_start(ap,f); ev++; evcode=0x0e; evop=op; fmt=f;
    int n=!strcmp(f,"12122")?5:!strcmp(f,"1H1")?3:1;
    for(int i=0;i<n;i++) val[i]=va_arg(ap,int);
    va_end(ap); return 0;
}
int main(void)
{
    ev=0; CHECK(hci_lmp_informational_parameters_cmd_handler(0x1005,0,0)==1);
    CHECK(ev==1 && evcode==0x0e && evop==0x1005 && !strcmp(fmt,"12122"));
    CHECK(val[0]==0 && val[1]==640 && val[2]==64 && val[3]==10 && val[4]==8);
    ev=0; CHECK(hci_lmp_informational_parameters_cmd_handler(0xffff,0,0)==0 && ev==0);
    u8 p[20]={0x34,0xf2,0x13,4,5};
    ev=0; CHECK(hci_lmp_status_parameters_cmd_handler(0x1403,p,0)==1);
    CHECK(ev==1 && evcode==0x0e && !strcmp(fmt,"1H1") && val[1]==0x234 && val[2]==100);
    ev=0; CHECK(hci_lmp_status_parameters_cmd_handler(0x1406,p,0)==1 && ev==0);
    ev=0; CHECK(hci_lmp_testing_cmd_handler(0x1801,p,1)==0 && ev==0);
    ev=0; logs=0; qos_payload=p;
    CHECK(hci_lmp_link_policy_cmd_handler(0x0807,p,20)==1 && ev==2);
    CHECK(logs==(TEST_LOG_ENABLED?2:0));
    CHECK(evcode==1 && !strcmp(fmt,"1H114444") && val[0]==0 && val[1]==0x234 && val[2]==0x13 && val[3]==4);
    CHECK(val[4]==(TEST_LOG_ENABLED?0x99:5)); CHECK(val[5]==0x77); qos_payload=0;
    CHECK(hci_lmp_link_policy_cmd_handler(0xffff,p,20)==0);
    return TEST_DONE("bd19_lmp_hci_dispatch");
}
#endif
