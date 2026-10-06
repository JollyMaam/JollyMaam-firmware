/* The SDK's Classic HCI event emitters, recovered from lmp.c. */
#include "btctrler/glue.h"

void lmp_send_event_encryption_change(struct lmp_connection *conn)
{ hci_send_event(0x08, "1H1", 0, conn->core_s.handle, 1); }

void lmp_send_event_role_change(struct lmp_connection *conn, u8 result)
{ hci_send_event(0x12, "1A1", result, conn->core_s.remote_addr, conn->core.role); }

void lmp_send_event_link_key_notification(struct lmp_connection *conn, int ret)
{ hci_send_event(0x18, "Ac161", conn->core_s.remote_addr, conn->core.link_key, ret); }

void lmp_send_event_connection_request(struct lmp_connection *conn, u8 link_type)
{ hci_send_event(0x04, "A31", conn->core_s.remote_addr, conn->core_s.remote_class_dev, link_type); }

void lmp_send_event_link_request(struct lmp_connection *conn)
{ hci_send_event(0x17, "A", conn->core_s.remote_addr); }

void lmp_send_event_auth_complete(struct lmp_connection *conn, u8 result)
{ hci_send_event(0x06, "1H", result, conn->core_s.handle); }

void lmp_send_event_connection_complete(struct lmp_connection *conn, u8 err_code, u16 handle, u8 link_type)
{
    hci_send_event(0x03, "1HA11", err_code, handle, conn->core_s.remote_addr, link_type, 0);
    os_time_dly(1);
}

void lmp_send_event_mode_change(struct lmp_connection *conn, u16 T_sniff)
{ hci_send_event(0x14, "1212", 0, conn->core_s.handle, conn->core.operation_mode, T_sniff); }
