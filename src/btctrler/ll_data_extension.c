/* LE data length extension: the controller's supported/suggested PDU size
 * table, each link's initial and effective data length (the smaller of what
 * each side can send and receive, and the radio's matching limits), and the
 * peer's LL_LENGTH_REQ.  SDK quirks kept: all comparisons are unsigned 16-bit;
 * the supported TX/RX time stay at the table's initial 2120 (only the octet
 * limits come from config_btctler_le_acl_packet_length, truncated to 16 bits);
 * a new link starts with the peer assumed at 27 octets and 328 us; the
 * suggested-default write stores only TX octets and TX time, unvalidated. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_DX;
extern BTCTRLER_CONFIG int config_btctler_le_acl_packet_length;
extern void log_print(int level, const char *tag, const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))

_Static_assert(sizeof(struct ll_data_pdu_length) == 16, "SDK data length table size");
_Static_assert(offsetof(struct ll_data_pdu_length, suggestedMaxTxOctets) == 8, "SDK suggested offset");
_Static_assert(offsetof(struct ll_data_pdu_length, connInitialMaxTxTime) == 14, "SDK initial time offset");
#ifndef UNITTEST
_Static_assert(offsetof(struct le_link, pdu_len) == 284, "SDK link pdu_len offset");
_Static_assert(offsetof(struct data_length, connEffectiveMaxRxTime) == 22, "SDK effective rx time offset");
#endif

static struct ll_data_pdu_length le_data_length
    __attribute__((section(".ble_ll_data"), aligned(2))) = { 251, 2120, 251, 2120, 0, 0, 0, 0 };

struct ll_data_pdu_length *ll_read_maximum_data_length(void) LL_CODE;
struct ll_data_pdu_length *ll_read_maximum_data_length(void)
{
    return &le_data_length;
}

struct ll_data_pdu_length *ll_read_suggested_default_data_length(void) LL_CODE;
struct ll_data_pdu_length *ll_read_suggested_default_data_length(void)
{
    return &le_data_length;
}

void ll_write_suggested_default_data_length(const u8 *data) LL_CODE;
void ll_write_suggested_default_data_length(const u8 *data)
{
    le_data_length.suggestedMaxTxOctets = btctler_little_endian_read_16(data, 0);
    le_data_length.suggestedMaxTxTime = btctler_little_endian_read_16(data, 2);
}

void __set_ll_effective_data_args(struct le_link *link) LL_CODE;
void __set_ll_effective_data_args(struct le_link *link)
{
    u16 tx_octets = link->pdu_len.connMaxTxOctets < link->pdu_len.connRemoteMaxRxOctets
                        ? link->pdu_len.connMaxTxOctets : link->pdu_len.connRemoteMaxRxOctets;
    link->pdu_len.connEffectiveMaxTxOctets = tx_octets;
    __ble_ops->ioctrl(link->hw, 15, (int)tx_octets);
    u16 rx_octets = link->pdu_len.connMaxRxOctets < link->pdu_len.connRemoteMaxTxOctets
                        ? link->pdu_len.connMaxRxOctets : link->pdu_len.connRemoteMaxTxOctets;
    link->pdu_len.connEffectiveMaxRxOctets = rx_octets;
    __ble_ops->ioctrl(link->hw, 16, (int)rx_octets);
    link->pdu_len.connEffectiveMaxTxTime = link->pdu_len.connMaxTxTime < link->pdu_len.connRemoteMaxRxTime
                                               ? link->pdu_len.connMaxTxTime : link->pdu_len.connRemoteMaxRxTime;
    link->pdu_len.connEffectiveMaxRxTime = link->pdu_len.connMaxRxTime < link->pdu_len.connRemoteMaxTxTime
                                               ? link->pdu_len.connMaxRxTime : link->pdu_len.connRemoteMaxTxTime;
}

void ll_data_extension_init(void) LL_CODE;
void ll_data_extension_init(void)
{
    u16 supported = (u16)config_btctler_le_acl_packet_length;
    le_data_length.supportedMaxTxOctets = supported;
    le_data_length.supportedMaxRxOctets = supported;

    u16 octets = le_data_length.suggestedMaxTxOctets ? le_data_length.suggestedMaxTxOctets : supported;
    le_data_length.connInitialMaxTxOctets = octets > supported ? supported : octets;

    u16 supported_time = le_data_length.supportedMaxTxTime;
    u16 time = le_data_length.suggestedMaxTxTime ? le_data_length.suggestedMaxTxTime : supported_time;
    le_data_length.connInitialMaxTxTime = time > supported_time ? supported_time : time;
}

void link_data_extension_init(struct le_link *link) LL_CODE;
void link_data_extension_init(struct le_link *link)
{
    link->pdu_len.connMaxTxOctets = le_data_length.connInitialMaxTxOctets;
    link->pdu_len.connMaxRxOctets = le_data_length.supportedMaxRxOctets;
    link->pdu_len.connMaxTxTime = le_data_length.connInitialMaxTxTime;
    link->pdu_len.connMaxRxTime = le_data_length.supportedMaxRxTime;
    link->pdu_len.connRemoteMaxTxOctets = 27;
    link->pdu_len.connRemoteMaxRxOctets = 27;
    link->pdu_len.connRemoteMaxTxTime = 328;
    link->pdu_len.connRemoteMaxRxTime = 328;
    __set_ll_effective_data_args(link);
}

void __ll_receive_length_req(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __ll_receive_length_req(struct le_link *link, struct ble_rx *rx)
{
    const u8 *params = &rx->data[1];
    link->pdu_len.connRemoteMaxRxOctets = btctler_little_endian_read_16(params, 0);
    link->pdu_len.connRemoteMaxRxTime = btctler_little_endian_read_16(params, 2);
    link->pdu_len.connRemoteMaxTxOctets = btctler_little_endian_read_16(params, 4);
    u16 remote_tx_time = btctler_little_endian_read_16(params, 6);
    link->pdu_len.connRemoteMaxTxTime = remote_tx_time;
    if (log_tag_const_i_LL_DX)
        log_print(2, NULL, "[Info]: [LL_DX]recieve DLE req, rx:%d,%d, tx:%d,%d\r\n",
                  (int)link->pdu_len.connRemoteMaxRxOctets, (int)link->pdu_len.connRemoteMaxRxTime,
                  (int)link->pdu_len.connRemoteMaxTxOctets, (int)remote_tx_time);
    __set_ll_effective_data_args(link);
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

const struct ble_operation *__ble_ops;
u8 log_tag_const_i_LL_DX;
int config_btctler_le_acl_packet_length = 251;

static int ioctrl_count, ioctrl_cmd[4], ioctrl_arg[4], logs;
static int log_args[4];
static void m_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    CHECK(hw == (void *)0x1234);
    ioctrl_cmd[ioctrl_count] = cmd;
    ioctrl_arg[ioctrl_count++] = va_arg(ap, int);
    va_end(ap);
}
void log_print(int level, const char *tag, const char *format, ...)
{
    va_list ap;
    (void)tag;
    CHECK(level == 2 && format != NULL);
    va_start(ap, format);
    for (int i = 0; i < 4; i++) log_args[i] = va_arg(ap, int);
    va_end(ap);
    logs++;
}
uint16_t btctler_little_endian_read_16(const uint8_t *b, int pos) { return (uint16_t)(b[pos] | b[pos + 1] << 8); }

int main(void)
{
    struct ble_operation ops;
    memset(&ops, 0, sizeof(ops));
    ops.ioctrl = m_ioctrl;
    __ble_ops = &ops;
    struct ll_data_pdu_length *t = ll_read_maximum_data_length();

    /* The table starts at 251 octets / 2120 us supported, nothing suggested or initial. */
    CHECK(t == ll_read_suggested_default_data_length());
    CHECK(t->supportedMaxTxOctets == 251 && t->supportedMaxTxTime == 2120 && t->supportedMaxRxOctets == 251 &&
          t->supportedMaxRxTime == 2120 && t->suggestedMaxTxOctets == 0 && t->suggestedMaxTxTime == 0 &&
          t->connInitialMaxTxOctets == 0 && t->connInitialMaxTxTime == 0);

    /* Init with nothing suggested: initial = supported. */
    config_btctler_le_acl_packet_length = 200;
    ll_data_extension_init();
    CHECK(t->supportedMaxTxOctets == 200 && t->supportedMaxRxOctets == 200);
    CHECK(t->connInitialMaxTxOctets == 200 && t->connInitialMaxTxTime == 2120);
    CHECK(t->supportedMaxTxTime == 2120 && t->supportedMaxRxTime == 2120);

    /* A suggested default below the limits is used; above them it is clamped. */
    const u8 write1[] = { 100, 0, 0x34, 0x02, 0xaa, 0xbb };    /* 100 octets, 0x0234 = 564 us */
    ll_write_suggested_default_data_length(write1);
    CHECK(t->suggestedMaxTxOctets == 100 && t->suggestedMaxTxTime == 564);
    ll_data_extension_init();
    CHECK(t->connInitialMaxTxOctets == 100 && t->connInitialMaxTxTime == 564);
    const u8 write2[] = { 0xff, 0x7f, 0xff, 0x7f };            /* 32767 and 32767 */
    ll_write_suggested_default_data_length(write2);
    ll_data_extension_init();
    CHECK(t->connInitialMaxTxOctets == 200 && t->connInitialMaxTxTime == 2120);
    const u8 write3[] = { 0, 0, 0, 0 };                        /* zero means unset again */
    ll_write_suggested_default_data_length(write3);
    ll_data_extension_init();
    CHECK(t->connInitialMaxTxOctets == 200 && t->connInitialMaxTxTime == 2120);
    config_btctler_le_acl_packet_length = 0x10000 + 27;        /* truncated to 16 bits */
    ll_data_extension_init();
    CHECK(t->supportedMaxTxOctets == 27 && t->connInitialMaxTxOctets == 27);
    config_btctler_le_acl_packet_length = 251;
    ll_data_extension_init();
    CHECK(t->supportedMaxTxOctets == 251 && t->supportedMaxRxOctets == 251 && t->connInitialMaxTxOctets == 251);

    /* A new link: ours from the table, the peer at 27 / 328; effective is the smaller of each pair. */
    struct le_link link;
    memset(&link, 0xee, sizeof(link));
    link.hw = (void *)0x1234;
    ioctrl_count = 0;
    link_data_extension_init(&link);
    CHECK(link.pdu_len.connMaxTxOctets == 251 && link.pdu_len.connMaxRxOctets == 251);
    CHECK(link.pdu_len.connMaxTxTime == 2120 && link.pdu_len.connMaxRxTime == 2120);
    CHECK(link.pdu_len.connRemoteMaxTxOctets == 27 && link.pdu_len.connRemoteMaxRxOctets == 27);
    CHECK(link.pdu_len.connRemoteMaxTxTime == 328 && link.pdu_len.connRemoteMaxRxTime == 328);
    CHECK(link.pdu_len.connEffectiveMaxTxOctets == 27 && link.pdu_len.connEffectiveMaxRxOctets == 27);
    CHECK(link.pdu_len.connEffectiveMaxTxTime == 328 && link.pdu_len.connEffectiveMaxRxTime == 328);
    CHECK(link.pdu_len.PreConnEffectiveTxOctets == 0xeeee);
    CHECK(ioctrl_count == 2 && ioctrl_cmd[0] == 15 && ioctrl_arg[0] == 27 && ioctrl_cmd[1] == 16 && ioctrl_arg[1] == 27);

    /* LL_LENGTH_REQ: opcode, then max rx octets, max rx time, max tx octets, max tx time. */
    struct { struct ble_rx rx; u8 pdu[16]; } r;
    memset(&r, 0, sizeof(r));
    const u8 pdu[] = { 20, 0xfb, 0x00, 0x48, 0x08, 0x1b, 0x00, 0x48, 0x01 };    /* 251, 2120, 27, 328 */
    memcpy(r.rx.data, pdu, sizeof(pdu));
    ioctrl_count = 0;
    __ll_receive_length_req(&link, &r.rx);
    CHECK(link.pdu_len.connRemoteMaxRxOctets == 251 && link.pdu_len.connRemoteMaxRxTime == 2120);
    CHECK(link.pdu_len.connRemoteMaxTxOctets == 27 && link.pdu_len.connRemoteMaxTxTime == 328);
    CHECK(link.pdu_len.connEffectiveMaxTxOctets == 251 && link.pdu_len.connEffectiveMaxRxOctets == 27);
    CHECK(link.pdu_len.connEffectiveMaxTxTime == 2120 && link.pdu_len.connEffectiveMaxRxTime == 328);
    CHECK(ioctrl_count == 2 && ioctrl_arg[0] == 251 && ioctrl_arg[1] == 27 && logs == 0);
    log_tag_const_i_LL_DX = 1;
    __ll_receive_length_req(&link, &r.rx);
    CHECK(logs == 1 && log_args[0] == 251 && log_args[1] == 2120 && log_args[2] == 27 && log_args[3] == 328);

    /* Unsigned comparison: 0x8000 is larger than 27. */
    link.pdu_len.connMaxTxOctets = 0x8000; link.pdu_len.connRemoteMaxRxOctets = 27;
    link.pdu_len.connMaxRxOctets = 5; link.pdu_len.connRemoteMaxTxOctets = 0xffff;
    link.pdu_len.connMaxTxTime = 0xfff0; link.pdu_len.connRemoteMaxRxTime = 0x7fff;
    link.pdu_len.connMaxRxTime = 0x1000; link.pdu_len.connRemoteMaxTxTime = 0x1000;
    ioctrl_count = 0;
    __set_ll_effective_data_args(&link);
    CHECK(link.pdu_len.connEffectiveMaxTxOctets == 27 && link.pdu_len.connEffectiveMaxRxOctets == 5);
    CHECK(link.pdu_len.connEffectiveMaxTxTime == 0x7fff && link.pdu_len.connEffectiveMaxRxTime == 0x1000);
    CHECK(ioctrl_arg[0] == 27 && ioctrl_arg[1] == 5);
    return TEST_DONE("btctrler_ll_data_extension");
}
#endif
