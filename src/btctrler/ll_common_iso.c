/* Isochronous (LE Audio) common layer: the HCI ISO data entry, the ISO data
 * header unpack, the CIS SDU fragmentation, and the BIS codec send.  The rest
 * of the SDK member (ISO init, the LE ISO command handler, reassembly and the
 * self tests) is unreferenced in this firmware and is dropped with it.  SDK
 * quirks kept: the CIS fragmentation writes its PDU through a null pdu
 * pointer (the SDK never assigned it); hci_push_iso_sdu logs "ISO SDU : %d"
 * with no argument; unpack leaves the SDU length and packet status
 * uninitialised for continuation fragments and clears the timestamp for
 * first-fragment and complete SDUs; lctrIsoSendCodecSdu leaks its buffer when
 * the codec returns data. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_ISO, log_tag_const_e_LL_ISO, log_tag_const_d_LL_ISO;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
#ifdef UNITTEST
#include <stdlib.h>
void *ll_iso_test_malloc(u32 size);
void ll_iso_test_free(void *ptr);
#define malloc ll_iso_test_malloc
#define free ll_iso_test_free
#else
extern void *malloc(u32 size);
extern void free(void *ptr);
#endif

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifdef UNITTEST
extern u8 *ll_iso_null_pdu;           /* stands in for address 0 */
#define NULL_PDU ll_iso_null_pdu
#else
#define NULL_PDU ((u8 *)0)
#endif

#ifndef UNITTEST
_Static_assert(sizeof(hci_iso_hdr_t) == 16, "SDK ISO header size");
_Static_assert(sizeof(ll_iso_rt_cfg_t) == 14, "SDK ISO runtime config size");
_Static_assert(offsetof(ll_iso_rt_cfg_t, maxIsoSduLen) == 2, "SDK maxIsoSduLen offset");
_Static_assert(sizeof(le_cis_t) == 356, "SDK CIS size");
_Static_assert(offsetof(le_cis_t, role) == 24, "SDK CIS role offset");
_Static_assert(offsetof(le_cis_t, framing) == 200, "SDK CIS framing offset");
_Static_assert(offsetof(le_cis_t, pduSizeMToS) == 218, "SDK CIS pdu size offset");
_Static_assert(offsetof(le_cis_t, bnMToS) == 232, "SDK CIS bnMToS offset");
_Static_assert(offsetof(le_cis_t, bnSToM) == 233, "SDK CIS bnSToM offset");
_Static_assert(offsetof(le_bis_t, big) == 12, "SDK BIS big offset");
_Static_assert(offsetof(le_big_t, maxSdu) == 22, "SDK BIG maxSdu offset");
#endif

ll_iso_rt_cfg_t ll_iso_rt_cfg __attribute__((section(".ble_ll_data"), aligned(2)));
ll_codec_t ll_codec __attribute__((section(".ble_ll_bss"), aligned(4)));
ll_iso_handle_t ll_iso_hdl __attribute__((section(".ble_ll_bss"), aligned(4)));
ll_dispatch_msg_t ll_dispatch_msg_table[15] __attribute__((section(".ble_ll_bss"), aligned(4)));

u8 ll_iso_unpack_hdr(const u8 *sdu, hci_iso_hdr_t *hdr) LL_CODE;
u8 ll_iso_unpack_hdr(const u8 *sdu, hci_iso_hdr_t *hdr)
{
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]-----Unpack\r\n");
    hdr->handle = sdu[0] | ((sdu[1] << 8) & 0xf00);
    hdr->pb_flag = (sdu[1] >> 4) & 3;
    hdr->ts_flag = (sdu[1] >> 6) & 1;
    hdr->rfu = 0;
    hdr->iso_data_load_length = sdu[2] | ((sdu[3] << 8) & 0x3f00);
    const u8 *data = sdu + 4;
    u8 offset = 4;
    if (hdr->ts_flag) {
        u32 time_stamp = sdu[4] | (sdu[5] << 8) | (sdu[6] << 16) | ((u32)sdu[7] << 24);
        hdr->time_stamp = time_stamp;
        data = sdu + 8;
        offset = 8;
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]+[1]TS                : 0x%x\r\n", (int)time_stamp);
    }
    if (hdr->pb_flag == 0 || hdr->pb_flag == 2) {
        hdr->time_stamp = 0;
        u32 sequence = data[0] | (data[1] << 8);
        hdr->packet_sequence_num = sequence;
        hdr->iso_sdu_length = data[2] | ((data[3] << 8) & 0xf00);
        data += 4;
        offset = (u8)(offset + 4);
        if (log_tag_const_i_LL_ISO) {
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]PSN               : 0x%x\r\n", (int)sequence);
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]ISO_SDU_LENGTH    : 0x%x\r\n", (int)hdr->iso_sdu_length);
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]PS                : 0x%x\r\n", (int)hdr->packet_status_flag);
        }
    }
    hdr->iso_sdu = (u8 *)data;
    return offset;
}

void ll_iso_cis_sdu_fragmentation(le_cis_t *cis, u8 *iso_data, u16 size) LL_CODE;
void ll_iso_cis_sdu_fragmentation(le_cis_t *cis, u8 *iso_data, u16 size)
{
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]frag_pos  0x%x /  sdu_size : 0x%x\r\n", iso_data, (int)size);
    /* The SDK's pdu pointer is never assigned and compiles to null: the PDU
     * header word is read once, rebuilt for each fragment and written back
     * once, and the payload goes to address 2. */
    u16 header = *(volatile u16 *)NULL_PDU;
    u8 *frag_pos = iso_data;
    u16 remaining = size;
    u16 next;
    do {
        u16 fragment = remaining > cis->pduSizeMToS ? cis->pduSizeMToS : remaining;
        next = (u16)(remaining - fragment);
        header = (u16)(((u16)(fragment << 8) | (header & 252)) | (next != 0));
        __builtin_memcpy(NULL_PDU + 2, frag_pos, fragment);
        frag_pos += fragment;
        remaining = next;
    } while (next != 0);
    *(volatile u16 *)NULL_PDU = header;
}

void hci_push_iso_sdu(u8 *packet, u16 size) LL_CODE;
void hci_push_iso_sdu(u8 *packet, u16 size)
{
    hci_iso_hdr_t sdu_hdr;
    (void)size;

    ll_iso_unpack_hdr(packet, &sdu_hdr);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]ISO SDU : %d\r\n");
        printf_buf(sdu_hdr.iso_sdu, sdu_hdr.iso_sdu_length);
    }
    u16 handle = (u16)(sdu_hdr.handle & 4095);
    le_cis_t *cis = ll_cis_by_handle(handle);
    if (!cis) {
        ll_bis_by_handle(handle);
        return;
    }
    if (ll_cis_is_established(cis) == 0) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]CIS not established\r\n");
        return;
    }
    if (cis->bnMToS == 0 && cis->role == 0) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]MToS no allow transmissions\r\n");
        return;
    }
    if (cis->bnSToM == 0 && cis->role == 1) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]SToM no allow transmissions\r\n");
        return;
    }
    if (cis->framing == 0)
        ll_iso_cis_sdu_fragmentation(cis, sdu_hdr.iso_sdu, (u16)(sdu_hdr.iso_sdu_length & 4095));
}

void lctrIsoSendCodecSdu(u16 handle) LL_CODE;
void lctrIsoSendCodecSdu(u16 handle)
{
    u32 pktCtr;
    le_bis_t *bis = ll_bis_by_handle(handle);
    if (!bis) {
        if (log_tag_const_d_LL_ISO)
            log_print(1, NULL, "[Debug]: [LL_ISO]lctrIsoSendCodecSdu: handle=%u not found\r\n", (int)handle);
        return;
    }
    u8 *buffer = malloc((u32)ll_iso_rt_cfg.maxIsoSduLen + 8);
    if (!buffer) {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]!!! Out of memory; dropping input SDU, stream=%u\r\n", (int)handle);
        return;
    }
    if (!ll_codec.in) btctrler_assert_failed("ll_codec.in", 0);
    if (ll_codec.in(handle, buffer + 12, bis->big->maxSdu, &pktCtr) == 0) {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]ISO audio stream data not available, stream=%u\r\n", (int)handle);
        free(buffer);
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#undef malloc
#undef free

u8 log_tag_const_i_LL_ISO, log_tag_const_e_LL_ISO, log_tag_const_d_LL_ISO;
static u8 null_pdu_area[64];
u8 *ll_iso_null_pdu = null_pdu_area;

static int logs_i, logs_e, logs_d, printf_buf_len, malloc_size, malloc_fail, free_count;
static void *freed_ptr, *malloc_ptr;
static char trace[256];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }
static le_cis_t cis;
static le_bis_t bis;
static le_big_t big;
static int cis_established = 1;
static int bis_calls, bis_handle;
static u16 cis_handle_seen;

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag; (void)format;
    if (level == 4) logs_e++; else if (level == 1) logs_d++; else logs_i++;
}
void printf_buf(const void *b, int n) { (void)b; printf_buf_len = n; T("pbuf"); }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; T("assert"); }
void *ll_iso_test_malloc(u32 size) { malloc_size = (int)size; return malloc_fail ? NULL : (malloc_ptr = calloc(1, size)); }
void ll_iso_test_free(void *p) { freed_ptr = p; free_count++; }
le_cis_t *ll_cis_by_handle(u16 h) { cis_handle_seen = h; return h == 5 ? &cis : NULL; }
u8 ll_cis_is_established(le_cis_t *c) { CHECK(c == &cis); return (u8)cis_established; }
le_bis_t *ll_bis_by_handle(u16 h) { bis_calls++; bis_handle = h; return h == 7 ? &bis : NULL; }

static u16 codec_result, codec_arg_handle, codec_arg_len;
static u8 *codec_arg_buf;
static u32 *codec_arg_ctr;
static u16 codec_in(u16 h, u8 *buf, u16 len, u32 *ctr) { codec_arg_handle = h; codec_arg_buf = buf; codec_arg_len = len; codec_arg_ctr = ctr; return codec_result; }

int main(void)
{
    hci_iso_hdr_t hdr;

    /* Unpack: complete SDU (pb 2), no timestamp.  Handle 0x123 (top bits of byte 1 ignored for the handle),
     * load length 12 (byte 3 high bits ignored), psn 0x0a0b, sdu length 8. */
    u8 a[16] = { 0x23, 0x21 | 0x80, 12, 0x00 | 0xc0, 0x0b, 0x0a, 0x08, 0xf0, 1, 2, 3, 4, 5, 6, 7, 8 };
    memset(&hdr, 0xff, sizeof(hdr));
    CHECK(ll_iso_unpack_hdr(a, &hdr) == 8);
    CHECK(hdr.handle == 0x123 && hdr.pb_flag == 2 && hdr.ts_flag == 0 && hdr.iso_data_load_length == 12);
    CHECK(hdr.time_stamp == 0 && hdr.packet_sequence_num == 0x0a0b && hdr.iso_sdu_length == 8 && hdr.iso_sdu == a + 8);
    CHECK(hdr.rfu == 0 && hdr.rfu2 == 3 && hdr.rfu3 == 3 && hdr.packet_status_flag == 3);   /* unwritten bits keep their old value */

    /* Timestamp and pb 0: the timestamp is read, then cleared by the SDU header parse. */
    u8 b[20] = { 0xbc, 0x0a | 0x40, 20, 0, 0x11, 0x22, 0x33, 0x44, 0x34, 0x12, 0x05, 0x0f };
    memset(&hdr, 0, sizeof(hdr));
    CHECK(ll_iso_unpack_hdr(b, &hdr) == 12);
    CHECK(hdr.handle == 0xabc && hdr.pb_flag == 0 && hdr.ts_flag == 1 && hdr.iso_data_load_length == 20);
    CHECK(hdr.time_stamp == 0 && hdr.packet_sequence_num == 0x1234 && hdr.iso_sdu_length == 0xf05 && hdr.iso_sdu == b + 12);

    /* Timestamp and pb 3 (last fragment): timestamp kept, no SDU header, SDU length untouched. */
    u8 c[12] = { 0x05, 0x30 | 0x40, 4, 0, 0x78, 0x56, 0x34, 0x12, 9, 9, 9, 9 };
    memset(&hdr, 0, sizeof(hdr));
    hdr.iso_sdu_length = 0x777;
    CHECK(ll_iso_unpack_hdr(c, &hdr) == 8);
    CHECK(hdr.pb_flag == 3 && hdr.ts_flag == 1 && hdr.time_stamp == 0x12345678 && hdr.iso_sdu_length == 0x777 && hdr.iso_sdu == c + 8);

    /* pb 1, no timestamp: just the four header bytes. */
    u8 d[8] = { 0x01, 0x10, 4, 0, 1, 2, 3, 4 };
    memset(&hdr, 0, sizeof(hdr));
    hdr.time_stamp = 0x55;
    CHECK(ll_iso_unpack_hdr(d, &hdr) == 4);
    CHECK(hdr.pb_flag == 1 && hdr.time_stamp == 0x55 && hdr.iso_sdu == d + 4);

    /* Logging only when the info tag is set (PS reads the unwritten bits). */
    log_tag_const_i_LL_ISO = 1;
    logs_i = 0;
    ll_iso_unpack_hdr(a, &hdr);
    CHECK(logs_i == 4);                     /* banner, PSN, length, PS */
    logs_i = 0;
    ll_iso_unpack_hdr(b, &hdr);
    CHECK(logs_i == 5);                     /* banner, TS, PSN, length, PS */
    log_tag_const_i_LL_ISO = 0;

    /* hci_push_iso_sdu: complete SDU of 10 bytes for CIS handle 5. */
    u8 pkt[24] = { 5, 0x20, 14, 0, 0x01, 0x00, 10, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    cis.pduSizeMToS = 4; cis.bnMToS = 1; cis.bnSToM = 1; cis.role = 0; cis.framing = 0;
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    null_pdu_area[0] = 0xa5; null_pdu_area[1] = 0xff;
    hci_push_iso_sdu(pkt, 18);
    CHECK(cis_handle_seen == 5);
    /* fragments 4, 4, 2: header low byte keeps (0xa5 & 0xfc) = 0xa4, high byte = last fragment length 2, more = 0. */
    CHECK(null_pdu_area[0] == 0xa4 && null_pdu_area[1] == 2);
    CHECK(null_pdu_area[2] == 8 && null_pdu_area[3] == 9 && null_pdu_area[4] == 6 && null_pdu_area[5] == 7 && null_pdu_area[6] == 0xee);

    /* A single fragment sets the length with more = 0; a second one before it sets more = 1 in the running header. */
    pkt[6] = 3;
    null_pdu_area[0] = 0x03; null_pdu_area[1] = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(null_pdu_area[0] == 0x00 && null_pdu_area[1] == 3 && null_pdu_area[2] == 0 && null_pdu_area[3] == 1 && null_pdu_area[4] == 2);
    pkt[6] = 0;                             /* zero-length SDU still produces one empty fragment */
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    hci_push_iso_sdu(pkt, 18);
    CHECK(null_pdu_area[0] == (0xee & 0xfc) && null_pdu_area[1] == 0 && null_pdu_area[2] == 0xee);
    pkt[6] = 10;

    /* Rejections. */
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    log_tag_const_e_LL_ISO = 1;
    cis_established = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 1 && null_pdu_area[0] == 0xee);
    cis_established = 1;
    cis.bnMToS = 0;                         /* master with bnMToS 0 is refused; as a slave it is fine */
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 2 && null_pdu_area[0] == 0xee);
    cis.role = 1;
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 2 && null_pdu_area[0] != 0xee);
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    cis.bnSToM = 0;                         /* slave with bnSToM 0 is refused */
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 3 && null_pdu_area[0] == 0xee);
    cis.role = 0; cis.bnMToS = 1;
    hci_push_iso_sdu(pkt, 18);              /* master with bnSToM 0 is fine */
    CHECK(logs_e == 3 && null_pdu_area[0] != 0xee);
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    cis.framing = 1;                        /* framed: nothing is sent */
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 3 && null_pdu_area[0] == 0xee);
    log_tag_const_e_LL_ISO = 0;

    /* No CIS: the BIS lookup is made (and ignored); the handle is masked to 12 bits. */
    pkt[0] = 7; pkt[1] = 0x20;
    bis_calls = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(bis_calls == 1 && bis_handle == 7 && cis_handle_seen == 7);
    pkt[0] = 0xff; pkt[1] = 0x2f;
    hci_push_iso_sdu(pkt, 18);
    CHECK(bis_handle == 0xfff);
    log_tag_const_i_LL_ISO = 1;
    logs_i = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_i == 5 && printf_buf_len == 10);    /* unpack: banner, PSN, length, PS; push: "ISO SDU" */
    log_tag_const_i_LL_ISO = 0;

    /* Codec send. */
    ll_iso_rt_cfg.maxIsoSduLen = 100;
    ll_codec.in = codec_in;
    big.maxSdu = 77;
    bis.big = &big;
    codec_result = 0;
    log_tag_const_d_LL_ISO = 1;
    lctrIsoSendCodecSdu(9);                 /* unknown handle: debug log */
    CHECK(logs_d == 1 && malloc_size == 0);
    log_tag_const_d_LL_ISO = 0;
    free_count = 0;
    lctrIsoSendCodecSdu(7);
    CHECK(malloc_size == 108 && codec_arg_handle == 7 && codec_arg_buf == (u8 *)malloc_ptr + 12 && codec_arg_len == 77);
    CHECK(codec_arg_ctr != NULL && free_count == 1 && freed_ptr == malloc_ptr);       /* no data: buffer freed */
    codec_result = 5;
    free_count = 0;
    lctrIsoSendCodecSdu(7);
    CHECK(free_count == 0);                 /* data available: the SDK leaks the buffer */
    malloc_fail = 1;
    log_tag_const_i_LL_ISO = 1;
    logs_i = 0;
    codec_arg_handle = 0;
    lctrIsoSendCodecSdu(7);
    CHECK(logs_i == 1 && codec_arg_handle == 0 && free_count == 0);
    malloc_fail = 0;
    ll_iso_rt_cfg.maxIsoSduLen = 0xffff;    /* the size is a 32-bit sum */
    lctrIsoSendCodecSdu(7);
    CHECK(malloc_size == 0xffff + 8);
    return TEST_DONE("btctrler_ll_common_iso");
}
#endif
