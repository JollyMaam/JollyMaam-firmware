#include "btstack_config.h"
#include "gap.h"
#include "hci.h"
#include "ble/sm.h"

/* BTstack's SM entry points for a host that never pairs (the updater):
 * instead of sm.c and btstack_crypto.c, some 9 KB.  The ATT server asks
 * gap_reconnect_security_setup_active() before every request; with no SM
 * there is no address lookup or re-encryption to wait for.  An SMP packet
 * from the peer goes unanswered. */

void sm_init(void) {}
void sm_set_io_capabilities(io_capability_t io_capability) { (void)io_capability; }
void sm_set_authentication_requirements(uint8_t auth_req) { (void)auth_req; }
void sm_add_event_handler(btstack_packet_callback_registration_t *callback_handler) { (void)callback_handler; }
void sm_request_pairing(hci_con_handle_t con_handle) { (void)con_handle; }
int sm_le_device_index(hci_con_handle_t con_handle) { (void)con_handle; return -1; }
bool gap_reconnect_security_setup_active(hci_con_handle_t con_handle) { (void)con_handle; return false; }

/* In sm.c with the rest of LE address handling. */
void gap_advertisements_set_params(uint16_t adv_int_min, uint16_t adv_int_max, uint8_t adv_type,
                                   uint8_t direct_address_typ, bd_addr_t direct_address,
                                   uint8_t channel_map, uint8_t filter_policy)
{
    hci_le_advertisements_set_params(adv_int_min, adv_int_max, adv_type, direct_address_typ,
                                     direct_address, channel_map, filter_policy);
}
