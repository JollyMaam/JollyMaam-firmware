/* BTstack on the Jieli BR23 controller library (btctrler.a): the HCI
 * transport, the run loop, and the few controller calls the host needs.
 * BTstack headers only; the controller's entry points are declared here. */
#include <string.h>
#include "btstack_config.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_base.h"
#include "hci.h"
#include "hci_transport.h"
#include "pad.h"

extern int btctrler_task_init(const void *transport, const void *config);
extern int btctrler_task_ready(void);
extern int btctrler_task_exit(void);
extern const void *hci_transport_h4_controller_instance(void);
extern void (*__h4_send_packet)(int type, const void *packet, int size);
extern void h4_hci_push_data(int type, void *packet, int size);
extern int le_controller_set_mac(const void *address);
extern void lmp_hci_write_local_address(const u8 *address);

static struct { u8 type; u16 size; u8 data[HCI_INCOMING_PACKET_BUFFER_SIZE]; } rx[RX_QUEUE_DEPTH];
static volatile u8 rx_read, rx_write;
static volatile u8 rx_active;
static void (*host_handler)(u8 type, u8 *packet, u16 size);
static btstack_data_source_t rx_source;
static u8 address[6];            /* controller order: least significant byte first */
static u8 controller_up;
static volatile u8 woken, exit_requested;

/* ---- controller --------------------------------------------------------- */

/* Called by btctrler.a for every event and ACL packet; the packet is only
 * valid during the call. */
int hci_packet_handler(u8 type, u8 *packet, u16 size)
{
    u8 next;

    /* HCI Reset puts the factory address back: reapply ours before BTstack
     * reads it. */
    if (type == HCI_EVENT_PACKET && size >= 6 && packet[0] == 0x0e && packet[3] == 0x03 &&
        packet[4] == 0x0c && packet[5] == 0)
        lmp_hci_write_local_address(address);
    if (!rx_active || size > HCI_INCOMING_PACKET_BUFFER_SIZE) return 0;
    next = (u8)((rx_write + 1) % RX_QUEUE_DEPTH);
    if (next == rx_read) return 0;
    rx[rx_write].type = type;
    rx[rx_write].size = size;
    memcpy(rx[rx_write].data, packet, size);
    rx_write = next;
    btstack_run_loop_poll_data_sources_from_irq();
    return 0;
}

void hci_jieli_set_address(const u8 a[6])
{
    memcpy(address, a, 6);
    if (!controller_up) return;
    le_controller_set_mac(address);
    lmp_hci_write_local_address(address);
}

/* Drop the link without the HCI halt, which waits for a peer that may never
 * answer.  The next session opens a fresh controller. */
void hci_jieli_drop_link(void)
{
    if (!rx_active) return;
    rx_active = 0;
    btctrler_task_exit();
    controller_up = 0;
}

/* ---- transport ---------------------------------------------------------- */

static void rx_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t t)
{
    (void)ds;
    (void)t;
    while (rx_read != rx_write) {
        host_handler(rx[rx_read].type, rx[rx_read].data, rx[rx_read].size);
        rx_read = (u8)((rx_read + 1) % RX_QUEUE_DEPTH);
    }
}

static void transport_init(const void *config)
{
    (void)config;
    rx_read = rx_write = 0;
}

/* The controller alone, without the host: the Switch 1 wake transmits on it
 * before a session, which then takes it over. */
int hci_jieli_open(void)
{
    if (!controller_up) {
        if (btctrler_task_init(hci_transport_h4_controller_instance(), 0)) return -1;
        while (!btctrler_task_ready()) plat_delay_ms(20);
        controller_up = 1;
    }
    return 0;
}

void hci_jieli_close(void)
{
    if (controller_up) btctrler_task_exit();
    controller_up = 0;
}

static int transport_open(void)
{
    if (hci_jieli_open()) return -1;
    rx_active = 1;
    return 0;
}

static int transport_close(void)
{
    rx_active = 0;
    hci_jieli_close();
    return 0;
}

static void transport_register(void (*handler)(u8 type, u8 *packet, u16 size))
{
    host_handler = handler;
    btstack_run_loop_set_data_source_handler(&rx_source, rx_process);
    btstack_run_loop_enable_data_source_callbacks(&rx_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&rx_source);
}

static int transport_send(u8 type, u8 *packet, int size)
{
    if (type == HCI_SCO_DATA_PACKET) return -1;
    /* The controller lists Read Encryption Key Size as supported but never
     * answers it, and BTstack holds every secured channel until it does:
     * answer for it (E0 always uses the full 16-byte key). */
    if (type == HCI_COMMAND_DATA_PACKET && size >= 5 &&
        little_endian_read_16(packet, 0) == HCI_OPCODE_HCI_READ_ENCRYPTION_KEY_SIZE) {
        u8 ev[9] = { HCI_EVENT_COMMAND_COMPLETE, 7, 1, packet[0], packet[1], 0,
                     packet[3], packet[4], 16 };
        hci_packet_handler(HCI_EVENT_PACKET, ev, sizeof(ev));
        return 0;
    }
    if (__h4_send_packet) __h4_send_packet(type, packet, size);
    else h4_hci_push_data(type, packet, size);
    return 0;
}

static const hci_transport_t transport = {
    .name = "jieli",
    .init = transport_init,
    .open = transport_open,
    .close = transport_close,
    .register_packet_handler = transport_register,
    .send_packet = transport_send,
};

const hci_transport_t *hci_jieli_transport(void)
{
    return &transport;
}

/* ---- run loop: sleeps until woken, never on a timer tick ---------------- */

static u32 now_ms(void)
{
    return plat_ms();
}

static void set_timer(btstack_timer_source_t *t, u32 ms)
{
    t->timeout = now_ms() + ms + 1;
}

static void wake(void)
{
    woken = 1;
    plat_bt_wake();
}

static volatile u32 loop_runs, loop_us;

/* The run loop's passes and the time in them, for bt_stats(). */
void hci_jieli_loop_stats(u32 *runs, u32 *us)
{
    *runs = loop_runs;
    *us = loop_us;
}

static void execute(void)
{
    u32 t = plat_us();

    while (!exit_requested) {
        btstack_run_loop_base_poll_data_sources();
        btstack_run_loop_base_execute_callbacks();
        btstack_run_loop_base_process_timers(now_ms());
        loop_runs++;
        /* A wake during the pass gets another pass at once.  Measured: without
         * it (sleeping on the semaphore alone, until the next BTstack timer)
         * the pass count fell 1071 -> 394/s but a Switch 2 link carried 131
         * reports/s instead of 195 (2026-10-05); the cause is not found yet
         * (padtool trace). */
        if (woken) {
            woken = 0;
            continue;
        }
        loop_us += plat_us() - t;
        util_trace(TR_BT_SLEEP, 0);
        plat_bt_wait(10);
        util_trace(TR_BT_WAKE, 0);
        t = plat_us();
    }
}

static void loop_init(void)
{
    btstack_run_loop_base_init();
    woken = 1;
    exit_requested = 0;
}

static void on_main_thread(btstack_context_callback_registration_t *cb)
{
    btstack_run_loop_base_add_callback(cb);
    wake();
}

static void trigger_exit(void)
{
    exit_requested = 1;
    wake();
}

static const btstack_run_loop_t run_loop = {
    .init = loop_init,
    .add_data_source = btstack_run_loop_base_add_data_source,
    .remove_data_source = btstack_run_loop_base_remove_data_source,
    .enable_data_source_callbacks = btstack_run_loop_base_enable_data_source_callbacks,
    .disable_data_source_callbacks = btstack_run_loop_base_disable_data_source_callbacks,
    .set_timer = set_timer,
    .add_timer = btstack_run_loop_base_add_timer,
    .remove_timer = btstack_run_loop_base_remove_timer,
    .execute = execute,
    .dump_timer = btstack_run_loop_base_dump_timer,
    .get_time_ms = now_ms,
    .poll_data_sources_from_irq = wake,
    .execute_on_main_thread = on_main_thread,
    .trigger_exit = trigger_exit,
};

const btstack_run_loop_t *hci_jieli_run_loop(void)
{
    return &run_loop;
}

/* ---- link policy, and hooks the controller library calls ----------------- */

extern void lmp_set_sniff_establish_by_remote(u8 enable);
extern void lmp_sniff_t_slot_attemp_reset(u16 slots, u16 attempt);
extern void edr_vendor_keep_connect_for_keymiss(u8 enable);
extern void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr);
extern void set_bt_data_rate_acl_3mbs_mode(u8 en);
extern void bt_production_test(u8 en);
extern void P33_CON_SET(u16 addr, u8 start, u8 len, u8 data);
extern u8 p33_rx_1byte(u16 addr);
extern const int sniff_support_reset_anchor_point;

static u16 sniff_slots = 24, sniff_attempt = 1;

/* Before a session: the profile's sniff parameters, and keep the link when a
 * host's key is missing (it re-pairs instead of dropping). */
void hci_jieli_prepare(u16 slots, u16 attempt)
{
    if (slots) sniff_slots = slots;
    if (attempt) sniff_attempt = attempt;
    edr_vendor_keep_connect_for_keymiss(1);
    /* The controller leaves 3 Mbit/s EDR out of its features unless told, and
     * BTstack then never allows 3-DH1: an 80-byte DualShock 4 or DualSense
     * report took two 2-DH1 packets instead of one, as the stock pad sends it.
     * Before the host stack reads the features. */
    set_bt_data_rate_acl_3mbs_mode(1);
}

/* Transmit power for the next session (docs/radio.md): the controller uses
 * the smallest of the step, its normal-mode cap 4 and any fixed level, from
 * the normal table or, after bt_production_test(1), the pro one.  That call
 * also sets the radio supply VDC13 (P3_ANA_CON6 bits 0-2): step 7 going in,
 * 5 (about 1 dB weaker) going out.  The supply is put back as booted. */
#define P3_ANA_CON6 0x06

void hci_jieli_tx_power(u8 step, u8 pro)
{
    static u8 was_pro;

    bt_max_pwr_set(step, 5, 8, step);
    if (pro || was_pro) {
        u8 vdc13 = p33_rx_1byte(P3_ANA_CON6) & 0x07;

        bt_production_test(pro);
        P33_CON_SET(P3_ANA_CON6, 0, 3, vdc13);
    }
    was_pro = pro;
}

/* Once the stack works: a HID device must accept the host's sniff request. */
void hci_jieli_link_policy(void)
{
    lmp_set_sniff_establish_by_remote(1);
    if (sniff_support_reset_anchor_point) lmp_sniff_t_slot_attemp_reset(sniff_slots, sniff_attempt);
}

void sniff_negotiation_hook(u16 *t_sniff, u16 *attempt, u16 *timeout, u8 negotiation)
{
    (void)negotiation;
    if (!t_sniff || !attempt || !timeout) return;
    *t_sniff = sniff_slots;
    *attempt = sniff_attempt;
    *timeout = 0;
}

u32 user_send_cmd_prepare(int cmd, u16 len, u8 *param)
{
    (void)cmd;
    (void)len;
    (void)param;
    return 0;
}

void btstack_assert_failed(const char *file, u16 line)
{
    util_log("btstack assert %s:%u", file, line);
    for (;;) {
    }
}

/* The patched controller (build_config.h) calls this at its markers; the
 * bring-up log it fed is gone. */
void cyclone2_bringup_record(u32 code)
{
    (void)code;
}
