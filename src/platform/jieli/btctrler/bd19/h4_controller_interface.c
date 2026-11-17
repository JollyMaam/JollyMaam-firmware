/* BD19's in-chip H4 transport from h4_controller_interface.c.o. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_HCI_LMP;
void log_print(int level, const char *file, const char *format, ...);
int hci_packet_handler(u8 type, u8 *packet, u16 size);
#define H4_CONTROLLER_CODE __attribute__((section(".hci_controller_code")))

static H4_CONTROLLER_CODE void h4_controller_init(const void *config)
{
    (void)config;
    if (log_tag_const_i_HCI_LMP)
        log_print(2, (const char *)0, "[Info]: [HCI_LMP]init\r\n");
}

static H4_CONTROLLER_CODE int h4_controller_open(void)
{
    if (log_tag_const_i_HCI_LMP)
        log_print(2, (const char *)0, "[Info]: [HCI_LMP]open\r\n");
    return 0;
}

static H4_CONTROLLER_CODE int h4_controller_close(void)
{
    if (log_tag_const_i_HCI_LMP)
        log_print(2, (const char *)0, "[Info]: [HCI_LMP]close\r\n");
    return 0;
}

static H4_CONTROLLER_CODE void h4_controller_register_packet_handler(
    void (*handler)(int, const u8 *, int))
{
    __h4_send_packet = handler;
}

static H4_CONTROLLER_CODE int h4_controller_can_send_now(uint8_t type)
{
    (void)type;
    return 0;
}

const h4_flow_control __h4_can_send_packet_now
    __attribute__((section(".h4_const"), aligned(4))) =
        h4_controller_can_send_now;

static H4_CONTROLLER_CODE int h4_controller_send_packet(int type, const u8 *packet, int size)
{
    hci_packet_handler((u8)type, (u8 *)packet, (u16)size);
    return 0;
}

static const hci_transport_t hci_transport_controller
    __attribute__((section(".hci_controller_const"), aligned(4))) = {
    .name = "H4_Controller",
    .init = h4_controller_init,
    .open = h4_controller_open,
    .close = h4_controller_close,
    .register_packet_handler = h4_controller_register_packet_handler,
    .can_send_packet_now = h4_controller_can_send_now,
    .send_packet = h4_controller_send_packet,
};

__attribute__((section(".hci_controller_code")))
const hci_transport_t *hci_transport_h4_controller_instance(void)
{
    return &hci_transport_controller;
}

#ifdef UNITTEST
#include <string.h>
#include "test.h"

u8 log_tag_const_i_HCI_LMP;
h4_transport_t __h4_send_packet;
static int log_count, packet_calls, packet_type, packet_size, packet_result;
static int mutate_packet;
static const char *last_log;
static u8 *packet_pointer, packet_first;

void log_print(int level, const char *file, const char *format, ...)
{
    CHECK(level == 2 && file == NULL);
    last_log = format;
    log_count++;
}

int hci_packet_handler(u8 type, u8 *packet, u16 size)
{
    packet_calls++;
    packet_type = type;
    packet_size = size;
    packet_pointer = packet;
    packet_first = packet ? packet[0] : 0;
    if (packet && mutate_packet) packet[0] = 0xc3;
    return packet_result;
}

static void registered_handler(int type, const u8 *packet, int size)
{ (void)type; (void)packet; (void)size; }

int test_h4_controller_interface_bd19(void)
{
    const hci_transport_t *t = hci_transport_h4_controller_instance();
    CHECK(t != NULL && strcmp(t->name, "H4_Controller") == 0);
    CHECK(t->set_baudrate == NULL && t->reset_link == NULL && t->set_sco_config == NULL);
    CHECK(t->can_send_packet_now == __h4_can_send_packet_now);
    CHECK(__h4_can_send_packet_now(0) == 0 && __h4_can_send_packet_now(255) == 0);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        log_tag_const_i_HCI_LMP = enabled;
        log_count = 0;
        t->init(NULL);
        CHECK(log_count == (log_tag_const_i_HCI_LMP ? 1 : 0));
        if (log_tag_const_i_HCI_LMP) CHECK(strcmp(last_log, "[Info]: [HCI_LMP]init\r\n") == 0);
        CHECK(t->open() == 0 && log_count == (log_tag_const_i_HCI_LMP ? 2 : 0));
        if (log_tag_const_i_HCI_LMP) CHECK(strcmp(last_log, "[Info]: [HCI_LMP]open\r\n") == 0);
        CHECK(t->close() == 0 && log_count == (log_tag_const_i_HCI_LMP ? 3 : 0));
        if (log_tag_const_i_HCI_LMP) CHECK(strcmp(last_log, "[Info]: [HCI_LMP]close\r\n") == 0);
    }
    t->register_packet_handler(registered_handler);
    CHECK(__h4_send_packet == registered_handler);
    t->register_packet_handler(NULL); CHECK(__h4_send_packet == NULL);
    {
        u8 packet[] = { 0x5a, 0xa5 };
        packet_result = -7;
        CHECK(t->send_packet(0x1234, packet, 0x10002) == 0);
        CHECK(packet_calls == 1 && packet_type == 0x34 && packet_size == 2);
        CHECK(packet_pointer == packet && packet_first == 0x5a);
        CHECK(packet[0] == 0x5a);
        mutate_packet = 1;
        CHECK(t->send_packet(-1, packet, -1) == 0);
        CHECK(packet_calls == 2 && packet_type == 255 && packet_size == 65535);
        CHECK(packet_pointer == packet && packet[0] == 0xc3);
    }
    return TEST_DONE("h4_controller_interface_bd19");
}

int main(void) { return test_h4_controller_interface_bd19(); }
#endif
