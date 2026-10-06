/* Classic LMP transmit construction and handoff, from SDK lmp_rxtx.c.
 * Binary payload serialization and buffer allocation remain SDK APIs. */
#include "btctrler/glue.h"

void lmp_format_packet(struct bredr_tx_bulk *tx, const char *format, va_list argptr)
{
    va_list args; va_copy(args, argptr);
    for (;;) {
        switch (*format) {
        case 'e': tx->enc_prob = (u8)va_arg(args, int) | 4; break;
        case 'E': tx->enc_post = (u8)va_arg(args, int) | 4; break;
        case 'v': tx->event_prob = va_arg(args, int); break;
        case 'V': tx->event_post = va_arg(args, int); break;
        case 'P': tx->pwr_post = 1; break;
        case 'D': tx->dut_post = 1; break;
        case 'S': tx->unsniff_ack_check = 1; break;
        case 'G': tx->pause_enc_debug = 1; break;
        case 'A': tx->set_afh = 1; break;
        case 'M': tx->max_slot_post = va_arg(args, int); break;
        default:
            tx->llid = 3;
            tx->packet = 3;
            tx->len = __vsprintf(tx->payload_head, format, args);
            va_end(args);
            return;
        }
        ++format;
    }
}
static int send_control(struct lmp_connection *conn, const char *format, va_list args, int response)
{
    if (!conn || !conn->link || !format) return -22;
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(17);
    if (!tx) {
        btctrler_assert_failed("LMP transmit allocation", 0);
        return -22;
    }
    lmp_format_packet(tx, format, args);
    if (response) tx->payload_head[0] |= conn->tran_id;
    else if (conn->core.role == 1) tx->payload_head[0] |= 1;
    link_conn_tx_bulk_avaiable(conn->link, tx);
    return 0;
}
int lmp_request(struct lmp_connection *conn, const char *format, ...)
{
    if (!(config_btctler_modules & 1)) return 0;
    va_list args; va_start(args, format);
    int result = send_control(conn, format, args, 0);
    va_end(args);
    return result;
}
int lmp_response(struct lmp_connection *conn, const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = send_control(conn, format, args, 1);
    va_end(args);
    return result;
}

#ifdef UNITTEST
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int config_btctler_modules = 3;
static struct link_connection link;
static struct lmp_connection connection;
static struct bredr_tx_bulk *allocated;
static int allocation_fails, allocations, assertions, serialized, queued;
static u8 initial_opcode, expected_opcode;
static const char *expected_format;
static int serial_arg, serial_result;
static struct bredr_tx_bulk formatted;
void btctrler_assert_failed(const char *what, int value)
{ assert(what && !value); ++assertions; }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size)
{
    assert(size == 17); ++allocations;
    if (allocation_fails) return NULL;
    assert(!allocated); allocated = calloc(1, sizeof(*allocated) + 17); assert(allocated);
    return allocated;
}
int __vsprintf(u8 *data, const char *format, va_list args)
{
    assert(!strcmp(format, expected_format));
    assert(va_arg(args, int) == serial_arg);
    data[0] = initial_opcode; data[1] = 0x5a;
    ++serialized; return serial_result;
}
void link_conn_tx_bulk_avaiable(struct link_connection *lc, struct bredr_tx_bulk *tx)
{
    assert(lc == &link && tx == allocated && serialized == 1);
    assert(tx->llid == 3 && tx->packet == 3 && tx->len == serial_result);
    assert(tx->payload_head[0] == expected_opcode && tx->payload_head[1] == 0x5a);
    ++queued;
}
static void format_direct(struct bredr_tx_bulk *tx, const char *format, ...)
{
    va_list args; va_start(args, format); lmp_format_packet(tx, format, args); va_end(args);
}
static void reset(void)
{
    free(allocated); allocated = NULL;
    memset(&connection, 0, sizeof(connection)); connection.link = &link;
    allocations = assertions = allocation_fails = serialized = queued = 0;
    expected_format = "11"; serial_arg = 0x12345678; serial_result = 2;
    initial_opcode = expected_opcode = 0x7e; config_btctler_modules = 3;
}
int main(void)
{
    /* Prefix arguments must end exactly where payload arguments begin. */
    for (int value = -128; value <= 511; ++value) {
        reset(); memset(&formatted, 0, sizeof(formatted));
        formatted.hold = formatted.tws_start_req = 1;
        format_direct(&formatted, "eEvVPDSGAM11", value, value + 1,
                      value + 2, value + 3, value + 4, serial_arg);
        assert(formatted.enc_prob == ((u8)value | 4));
        assert(formatted.enc_post == ((u8)(value + 1) | 4));
        assert(formatted.event_prob == (u8)(value + 2));
        assert(formatted.event_post == (u8)(value + 3));
        assert(formatted.max_slot_post == (u8)(value + 4));
        assert(formatted.pwr_post && formatted.dut_post && formatted.unsniff_ack_check);
        assert(formatted.pause_enc_debug && formatted.set_afh);
        assert(formatted.hold && formatted.tws_start_req);
        assert(formatted.llid == 3 && formatted.packet == 3 && formatted.len == 2);
        assert(serialized == 1 && !allocations && !queued);
    }
    reset(); memset(&formatted, 0, sizeof(formatted));
    format_direct(&formatted, "eeMM11", 1, 8, 3, 5, serial_arg);
    assert(formatted.enc_prob == 12 && formatted.max_slot_post == 5);
    reset(); expected_format = "11P";
    format_direct(&formatted, "11P", serial_arg);
    reset(); expected_format = " 11";
    format_direct(&formatted, "P 11", serial_arg);
    assert(formatted.pwr_post);
    for (int role = 0; role < 256; ++role) {
        reset(); connection.core.role = role;
        expected_opcode |= role == 1;
        assert(lmp_request(&connection, "11", serial_arg) == 0);
        assert(allocations == 1 && queued == 1 && !assertions);
    }
    for (int transaction = 0; transaction < 256; ++transaction) {
        reset(); connection.tran_id = transaction;
        initial_opcode = transaction ^ 0xa5;
        expected_opcode = initial_opcode | transaction;
        assert(lmp_response(&connection, "11", serial_arg) == 0);
        assert(allocations == 1 && queued == 1 && !assertions);
    }
    for (int modules = 0; modules < 8; ++modules) {
        reset(); config_btctler_modules = modules;
        assert(lmp_request(&connection, "11", serial_arg) == 0);
        assert(allocations == (modules & 1) && queued == (modules & 1));
        reset(); config_btctler_modules = modules;
        assert(lmp_response(&connection, "11", serial_arg) == 0);
        assert(allocations == 1 && queued == 1);
    }
    reset();
    assert(lmp_request(NULL, "11", serial_arg) == -22);
    assert(lmp_response(NULL, "11", serial_arg) == -22);
    assert(lmp_request(&connection, NULL) == -22);
    assert(lmp_response(&connection, NULL) == -22);
    connection.link = NULL;
    assert(lmp_request(&connection, "11", serial_arg) == -22);
    assert(lmp_response(&connection, "11", serial_arg) == -22);
    assert(!allocations && !queued && !serialized);
    config_btctler_modules = 0;
    assert(lmp_request(NULL, NULL) == 0);
    reset(); allocation_fails = 1;
    assert(lmp_request(&connection, "11", serial_arg) == -22);
    assert(allocations == 1 && assertions == 1 && !serialized && !queued);
    reset(); allocation_fails = 1;
    assert(lmp_response(&connection, "11", serial_arg) == -22);
    assert(allocations == 1 && assertions == 1 && !serialized && !queued);
    reset();
    puts("btctrler LMP control transmit tests passed");
}
#endif
