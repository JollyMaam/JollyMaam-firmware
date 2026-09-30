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
    /* Optional microsecond clock for the input-slot wait statistics. */
    uint32_t (*time_us)(void);
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
/* Reports queued for the HID interrupt channel and not yet sent, counting a
 * pending input report. */
uint16_t btstack_jl_core_reports_queued(void);

/* Input reports since the link came up: the Classic HID interrupt channel
 * or LE notifications, whichever is in use. */
typedef struct {
    uint32_t sent;          /* input reports handed to the stack */
    uint32_t replaced;      /* overwritten by a newer one before they went */
    uint32_t messages;      /* queued reports (replies, events) sent */
    uint32_t wait_avg_us;   /* input posted -> handed to the stack */
    uint32_t wait_max_us;
    /* Handed to the controller -> its Number Of Completed Packets: the
     * packet has been acknowledged over the air (or flushed). */
    uint32_t hold_avg_us;
    uint32_t hold_max_us;
    uint32_t hold_count;
    uint32_t sniff_entries; /* the host put the link into sniff */
    /* LE: connection interval (1.25 ms units), peripheral latency, PHY. */
    uint16_t le_interval;
    uint16_t le_latency;
    uint8_t  le_tx_phy;
    uint8_t  le_rx_phy;
    uint16_t le_tx_octets;  /* LE PDU payload after Data Length Change */
    /* Hand-off to acknowledgement over 50 ms: a retransmission burst the
     * player would feel. */
    uint32_t stalls;
    /* Signal of the host as our receiver hears it (HCI Read RSSI, once a
     * second), as a positive number: 62 means -62 dBm.  0 until measured,
     * and always 0 on Classic: there the controller reports the offset from
     * the golden range, 0 inside it.  LE reports absolute dBm. */
    uint8_t  rssi_last;
    uint8_t  rssi_worst;
} btstack_jl_input_stats_t;
void btstack_jl_core_input_stats(btstack_jl_input_stats_t *out);
/* Sequence numbers of input reports: the last one stored by
 * btstack_jl_core_set_input_report(), and the last one the link took.  A
 * report is on its way once taken - (posted) >= 0 in wrapping arithmetic. */
uint32_t btstack_jl_core_input_posted(void);
uint32_t btstack_jl_core_input_taken(void);

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

/* A report that must reach the host - a reply, an event: queued, sent in
 * order, ahead of any input report.  0 on success, 3 when the queue is full. */
int  btstack_jl_core_send_report(const uint8_t *message, uint16_t size);
/* The controller's current input state: replaces any input report that has
 * not gone yet, so the host only ever gets the newest.  0 on success. */
int  btstack_jl_core_set_input_report(const uint8_t *message, uint16_t size);

void btstack_jl_core_delete_all_link_keys(void);
void btstack_jl_core_delete_link_key(const uint8_t address[6]);

void btstack_jl_core_run(void *argument);

#ifdef __cplusplus
}
#endif

#endif
