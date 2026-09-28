/*
 * Boundary between the Jieli SDK side of the firmware and the upstream
 * BTstack host running on top of btctrler.a.
 *
 * This header is included from both sides, so it must not pull in either the
 * Jieli SDK headers (u8/u16/bool typedefs, bt_event, ...) or the upstream
 * BTstack headers (btstack_config.h, hci.h, ...).  Everything exchanged here
 * uses plain C types.
 */
#ifndef BTSTACK_JL_H
#define BTSTACK_JL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BTSTACK_JL_EVENT_READY = 1,
    BTSTACK_JL_EVENT_CONNECTED,
    BTSTACK_JL_EVENT_ENCRYPTED,
    BTSTACK_JL_EVENT_DISCONNECTED,
    BTSTACK_JL_EVENT_HID_OPENED,
    BTSTACK_JL_EVENT_HID_CLOSED,
    BTSTACK_JL_EVENT_HCI_STATUS,
};

typedef struct {
    uint8_t type;
    uint8_t code;
    uint8_t status;
    uint8_t address[6];
} btstack_jl_event_t;

typedef void (*btstack_jl_event_handler_t)(const btstack_jl_event_t *event);

typedef int  (*btstack_jl_link_key_get_t)(const uint8_t address[6], uint8_t key[16], uint8_t *type);
typedef void (*btstack_jl_link_key_put_t)(const uint8_t address[6], const uint8_t key[16], uint8_t type);
typedef void (*btstack_jl_link_key_delete_t)(const uint8_t address[6]);
typedef int  (*btstack_jl_link_key_iterator_t)(uint8_t index, uint8_t address[6], uint8_t key[16], uint8_t *type);

typedef struct {

    const char *local_name;
    uint32_t    class_of_device;
    uint8_t     bd_addr[6];

    const uint8_t *report_descriptor;
    uint16_t       report_descriptor_size;

    uint8_t io_capability;
    uint8_t authentication_requirement;

    uint16_t page_timeout_ms;
    uint16_t supervision_timeout_ms;

    btstack_jl_link_key_get_t      link_key_get;
    btstack_jl_link_key_put_t      link_key_put;
    btstack_jl_link_key_delete_t   link_key_delete;
    btstack_jl_link_key_iterator_t link_key_iterator;

    uint32_t (*time_ms)(void);
    void     (*critical_enter)(void);
    void     (*critical_exit)(void);
    /* Optional progress trace; the code is stored in the reset-surviving
     * bring-up record (see src/main.h). */
    void     (*trace)(uint32_t code);

    void     (*hid_output)(const uint8_t *packet, uint16_t size);

    uint16_t device_id_vendor;
    uint16_t device_id_product;
    uint16_t device_id_version;

    int (*hid_get_report)(uint8_t report_type, uint8_t report_id,
                          uint8_t *report, uint16_t size);
} btstack_jl_config_t;

int  btstack_jl_core_init(const btstack_jl_config_t *config,
                          btstack_jl_event_handler_t handler);

void btstack_jl_core_request_shutdown(void);
/* End the session even if the power-down never completed (run-loop task). */
void btstack_jl_core_force_shutdown(void);

void btstack_jl_core_request_start(void);

void btstack_jl_core_request_unsniff(void);
/* Input reports queued for the HID interrupt channel and not yet sent. */
uint16_t btstack_jl_core_reports_queued(void);

uint8_t btstack_jl_core_idle(void);

uint8_t btstack_jl_core_link_up(void);

void btstack_jl_core_power_off(void);

void btstack_jl_core_set_scan(uint8_t connectable, uint8_t discoverable);
/* Start LE advertising with this payload (applied on the BTstack run loop),
 * or stop it with length 0. */
void btstack_jl_core_le_advertise(const uint8_t *data, uint8_t length);

void btstack_jl_core_set_address(const uint8_t address[6]);
const uint8_t *btstack_jl_core_get_address(void);

int  btstack_jl_core_hid_connect(const uint8_t address[6]);

int  btstack_jl_core_disconnect(void);
int  btstack_jl_core_hid_connected(void);

int  btstack_jl_core_send_report(const uint8_t *message, uint16_t size);

void btstack_jl_core_delete_all_link_keys(void);
void btstack_jl_core_delete_link_key(const uint8_t address[6]);

void btstack_jl_core_run(void *argument);

#ifdef __cplusplus
}
#endif

#endif
