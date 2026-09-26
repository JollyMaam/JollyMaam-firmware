/*
 * The few Security Manager entry points att_server and the LE layer call,
 * without BTstack's sm.c.
 *
 * This firmware never runs SMP: the Switch 2 exchanges its key out of band
 * over GATT commands and disconnects when SMP is attempted, and
 * btstack_jl_le.c answers the LE Long Term Key request itself.  sm.c cost
 * about as much flash as the code limit had left, so these answer for it:
 * no identity lookup, no bonding records, nothing to wait for.
 */
#include "btstack_config.h"

#include "ble/sm.h"
#include "gap.h"
#include "hci.h"

void sm_add_event_handler(btstack_packet_callback_registration_t *callback_handler)
{
    (void)callback_handler;
}

void sm_request_pairing(hci_con_handle_t con_handle)
{
    (void)con_handle;
}

int sm_le_device_index(hci_con_handle_t con_handle)
{
    (void)con_handle;
    return -1;
}

/* No identity (IRK) lookup and no re-encryption as central ever runs, so
 * nothing is ever pending: att_server may process requests immediately. */
bool gap_reconnect_security_setup_active(hci_con_handle_t con_handle)
{
    (void)con_handle;
    return false;
}

/* Lives in sm.c upstream (because of random-address handling there). */
void gap_advertisements_set_params(uint16_t adv_int_min, uint16_t adv_int_max,
                                   uint8_t adv_type, uint8_t direct_address_typ,
                                   bd_addr_t direct_address, uint8_t channel_map,
                                   uint8_t filter_policy)
{
    hci_le_advertisements_set_params(adv_int_min, adv_int_max, adv_type,
                                     direct_address_typ, direct_address,
                                     channel_map, filter_policy);
}
