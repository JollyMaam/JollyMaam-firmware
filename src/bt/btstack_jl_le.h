#ifndef BTSTACK_JL_LE_H
#define BTSTACK_JL_LE_H

/*
 * LE peripheral on the BTstack port: one GATT server connection, advertising,
 * notifications, and the LE Long Term Key request answered by the profile
 * (the Switch 2 does its own out-of-band key exchange, not SMP).
 *
 * Functions marked (any task) may be called from the application; the rest
 * run on the BTstack run loop.  The read/write/ltk/state callbacks are called
 * on the run loop.
 */

#include <stdint.h>

struct btstack_jl_le_config {
    const uint8_t *att_db;                 /* BTstack ATT DB, version byte first */
    /* ATT read of a dynamic attribute: copy up to size bytes from offset, or
     * with buffer == NULL return the full length. */
    uint16_t (*read)(uint16_t handle, uint16_t offset, uint8_t *buffer,
                     uint16_t size);
    /* ATT write (with or without response) to a dynamic attribute. */
    void (*write)(uint16_t handle, const uint8_t *data, uint16_t size);
    /* The console asks for the link's LTK: fill it and return 1, or 0 to
     * refuse (negative reply). */
    int (*get_ltk)(uint8_t ltk[16]);
    /* Connection state changes: 1 connected, 0 disconnected. */
    void (*state)(int connected);

    /* Security Manager.  With sm_er set, BTstack's SM runs this session:
     * legacy "just works" pairing with bonding, a security request as soon as
     * a central connects, and the LE LTK request answered by SM (our LTK is
     * rebuilt from ER and the EDIV/RAND the host sends back, so bonds need no
     * key storage here).  get_ltk is then unused.  sm_er and sm_ir are this
     * session's 16-byte root keys; keep them stable for a slot, or its hosts'
     * bonds stop working. */
    const uint8_t *sm_er;
    const uint8_t *sm_ir;
    /* Optional: the link became encrypted (new pairing or a bond reused).
     * A bonded host expects its notifications to still be enabled; restore
     * them here with btstack_jl_le_set_ccc(). */
    void (*encrypted)(void);
    /* Optional: connection interval to ask the central for once encrypted,
     * in 1.25 ms units (0 = leave it to the central). */
    uint16_t conn_interval_min;
    uint16_t conn_interval_max;
};

/* (any task) Install the configuration used by the next btstack_init(), or
 * NULL for none.  Keeps a pointer; the config must stay valid. */
void btstack_jl_le_configure(const struct btstack_jl_le_config *config);

/* (any task) Advertise this payload (ADV_IND, connectable) while not
 * connected; length 0 stops advertising. */
void btstack_jl_le_set_advertising(const uint8_t *data, uint8_t length);
/* (any task) Scan response to go with it; length 0 for none. */
void btstack_jl_le_set_scan_response(const uint8_t *data, uint8_t length);

/* (any task) Latest value for a notifying attribute (the input report).  Only
 * the newest one is kept; it goes out when the link can send and the client
 * has enabled notifications on it. */
void btstack_jl_le_set_report(uint16_t handle, const uint8_t *data,
                              uint16_t length);

/* (any task) */
int btstack_jl_le_connected(void);
/* The connected (or last connected) central's address, least significant
 * byte first as the application stores addresses.  Returns 0 if none yet. */
int btstack_jl_le_peer_address(uint8_t address[6]);
void btstack_jl_le_disconnect(void);

/* (run loop) Notify now if possible, otherwise keep it (one pending) and send
 * it as soon as ATT can.  For command responses from the write callback. */
void btstack_jl_le_notify(uint16_t handle, const uint8_t *data,
                          uint16_t length);

/* (run loop) Whether the client enabled notifications on a value handle
 * (its CCC is the next handle). */
int btstack_jl_le_notifications_enabled(uint16_t value_handle);
/* (run loop) Set a value handle's CCC as if the client had written it. */
void btstack_jl_le_set_ccc(uint16_t value_handle, uint16_t value);

/* Hooks the core calls. */
void btstack_jl_le_setup(void);
void btstack_jl_le_working(void);
void btstack_jl_le_poll(void);
void btstack_jl_le_teardown(void);

#endif
