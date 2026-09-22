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

/* Events raised by the BTstack task. */
enum {
    BTSTACK_JL_EVENT_READY = 1,          /* stack powered on, scans applied */
    BTSTACK_JL_EVENT_CONNECTED,          /* ACL connected */
    BTSTACK_JL_EVENT_ENCRYPTED,          /* link encrypted */
    BTSTACK_JL_EVENT_DISCONNECTED,       /* ACL disconnected, status = reason */
    BTSTACK_JL_EVENT_HID_OPENED,         /* HID control + interrupt channels up */
    BTSTACK_JL_EVENT_HID_CLOSED,         /* HID channels gone */
    BTSTACK_JL_EVENT_HCI_STATUS,         /* raw HCI status event, status = code */
};

typedef struct {
    uint8_t type;
    uint8_t code;      /* HCI event code for BTSTACK_JL_EVENT_HCI_STATUS */
    uint8_t status;
    uint8_t address[6];
} btstack_jl_event_t;

typedef void (*btstack_jl_event_handler_t)(const btstack_jl_event_t *event);

/* Link key storage, implemented on the SDK side (syscfg backed). */
typedef int  (*btstack_jl_link_key_get_t)(const uint8_t address[6], uint8_t key[16], uint8_t *type);
typedef void (*btstack_jl_link_key_put_t)(const uint8_t address[6], const uint8_t key[16], uint8_t type);
typedef void (*btstack_jl_link_key_delete_t)(const uint8_t address[6]);
typedef int  (*btstack_jl_link_key_iterator_t)(uint8_t index, uint8_t address[6], uint8_t key[16], uint8_t *type);

typedef struct {
    /* Local identity. */
    const char *local_name;
    uint32_t    class_of_device;
    uint8_t     bd_addr[6];

    /* HID report descriptor. */
    const uint8_t *report_descriptor;
    uint16_t       report_descriptor_size;

    /* SSP settings, values as in the Bluetooth spec (HCI IO capability and
     * authentication requirement). */
    uint8_t io_capability;
    uint8_t authentication_requirement;

    /* Link policy, in milliseconds. */
    uint16_t page_timeout_ms;
    uint16_t supervision_timeout_ms;

    /* Provider hooks. */
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

    /* HIDP message received from the host, with the one-octet HIDP header and
     * the report id where the report descriptor declares one, i.e. exactly the
     * bytes the application's HID output handler expects:
     *   0xa2, report id, body   - interrupt-channel output report
     *   0x52, report id, body   - control-channel SET_REPORT */
    void     (*hid_output)(const uint8_t *packet, uint16_t size);
} btstack_jl_config_t;

/* Prepare the host stack.  Only records the configuration: the heavy
 * initialization runs on the "btstack" task (see btstack_jl_core_run), because
 * the application task that calls this has a very small stack. */
int  btstack_jl_core_init(const btstack_jl_config_t *config,
                          btstack_jl_event_handler_t handler);

/* Ask the run-loop task to power the controller down and stop. */
void btstack_jl_core_request_shutdown(void);
/* Power the controller down (run-loop task only). */
void btstack_jl_core_power_off(void);

/* Apply the requested page/inquiry scan state (1 = enable). */
void btstack_jl_core_set_scan(uint8_t connectable, uint8_t discoverable);

/* Replace the local device address; the controller keeps its own copy too. */
void btstack_jl_core_set_address(const uint8_t address[6]);
const uint8_t *btstack_jl_core_get_address(void);

/* Open the HID channels towards a peer (pages the peer if needed). */
int  btstack_jl_core_hid_connect(const uint8_t address[6]);
/* Tear the current connection down. */
int  btstack_jl_core_disconnect(void);
int  btstack_jl_core_hid_connected(void);

/* Queue one complete HIDP message for the interrupt channel.  Returns 0 when
 * queued, 1 when no HID channel is open, 2 when the message is too large and
 * 3 when the queue is full. */
int  btstack_jl_core_send_report(const uint8_t *message, uint16_t size);

/* Bond management. */
void btstack_jl_core_delete_all_link_keys(void);
void btstack_jl_core_delete_link_key(const uint8_t address[6]);

/* Body of the "btstack" OS task. */
void btstack_jl_core_run(void *argument);

#ifdef __cplusplus
}
#endif

#endif
