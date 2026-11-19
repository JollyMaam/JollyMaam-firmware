/* What owned controller C needs beyond the recovered SDK header. */
#ifndef BTCTRLER_GLUE_H
#define BTCTRLER_GLUE_H

#include "btctrler/sdk.h"
#include "btctrler/config.h"

#ifdef UNITTEST
#define BTCTRLER_CONFIG          /* tests vary the configuration */
#else
#define BTCTRLER_CONFIG const
#endif
#define BTCTRLER_CONFIG_ASSER BTCTRLER_CONFIG

/* Our changes to the SDK controller ("Our change:", btctrlermod_*), on in
 * the app on both chips.  The BD19 audit build (tools/btctrler/vanilla.mk)
 * sets 0: the SDK's behaviour, which its objects are compared with. */
#ifndef BTCTRLERMOD
#define BTCTRLERMOD 1
#endif

/* lmp.c's file-static ACL slot; the archive build gives it external linkage. */
extern struct lmp_connection lmp_acl_link[1];
extern u8 lmp_dut_en, lmp_dut_phone_en;
extern void (*lmp_update_rx_handler)(int, struct bredr_rx_bulk *);

/* Entry points absent from the generated prototypes: owned C and SDK
 * file-static helpers exported by the archive build. */
void hci_command_process(u8 *cmd, int size);            /* hci_controller.c */
void lmp_detach_check(void);                            /* lmp.c */
void lmp_rx_handler(struct lmp_connection *conn);       /* lmp.c */
void lmp_send_acl_u_packet_to_host(struct lmp_connection *conn);  /* lmp.c */
void lmp_dhkey_check_accept(struct lmp_connection *conn);         /* lmp.c */
void page_completed(u8 *addr, int err);                           /* lmp_inquiry_page.c */
u8 lmp_cmd_is_match_size(u8 opcode, u8 *rx_data, u8 rx_len);     /* lmp.c */
void lmp_send_aclu_en(struct lmp_connection *conn, u8 enable);    /* lmp.c */
void link_conn_rx_remote_max_power_res(struct link_connection *link, u8 stop_incr);  /* lmp.c */

/* The SDK's ASSERT() (hci_dispatch.c): with config_asser set, print and
 * stop in cpu_assert_debug; otherwise reset the chip. */
void btctrler_assert_failed(const char *what, int value);
extern BTCTRLER_CONFIG_ASSER int config_asser;
void cpu_assert_debug(void);

/* The SDK's cpu.h probes: inside an interrupt handler; interrupts disabled
 * or masked (IPMASK at the controller's level 6). */
#ifndef UNITTEST
static inline int cpu_in_irq(void)
{
    int flag;
    __asm__ volatile("%0 = icfg" : "=r"(flag));
    return flag & 0xff;
}
#ifdef CONFIG_CPU_BD19
/* The BD19 SDK supplies this in its runtime, with its own IRQ mask level. */
int cpu_irq_disabled(void);
#else
static inline int cpu_irq_disabled(void)
{
    int flag;
    __asm__ volatile("%0 = icfg" : "=r"(flag));
    return (flag & 0x300) != 0x300 || *(volatile u32 *)0x10f1a8 == 6;
}
#endif
#else
int cpu_in_irq(void);
int cpu_irq_disabled(void);
#endif

/* CPU and OS runtime, outside btctrler.a. */
void local_irq_disable(void);
void local_irq_enable(void);
int os_sem_post(OS_SEM *sem);
void os_time_dly(int ticks);
void sys_timer_del(u16 id);
int sys_timer_modify(u16 id, u32 msec);
u16 sys_timeout_add(void *priv, void (*func)(void *priv), u32 msec);
u16 sys_timer_add(void *priv, void (*func)(void *priv), u32 msec);
void sys_timeout_del(u16 id);
void p33_soft_reset(void);
void *zalloc(u32 size);
extern BTCTRLER_CONFIG int config_btctler_modules, config_btctler_mode, config_btctler_le_roles;
extern BTCTRLER_CONFIG int CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE;
extern const int sniff_long_interval, sniff_support_reset_anchor_point;   /* platform_jieli_sdk.c */

/* QoS adapter (btstack_qos_jieli.c) and its controller-side calls. */
void btctrlermod_qos_setup(const u8 *parameters);
u8 btctrlermod_lmp_qos_send(u16 handle, u16 slots);
u8 btctrlermod_lmp_qos_answer(u16 handle, u8 status);
extern u16 btctrler_qos_poll_slots;

/* A packet from the central arrived on a connection (Classic or LE), in
 * interrupt context: btstack_hci_jieli.c times the link's anchors with it. */
void btctrlermod_air_rx(void);

/* Host flow control.  btstack_hci_jieli.c: whether its receive queue takes
 * one more ACL packet; refused, Classic data stays in the link's list
 * (lmp_acl.c retries) and LE data in the radio's received list (RF_ble.c),
 * which flows the link off once the radio's buffers run out.  The transport
 * calls btctrlermod_host_ready (hci_transport.c) once it has room again. */
int btctrlermod_host_acl_room(void);
void btctrlermod_host_ready(void);
void *btctrlermod_dma_first(struct _dma_head *head);   /* hw_dma.c: the oldest, not taken */

/* link_layer.c: our own LL_VERSION_IND exchange is under way (ll_slave.c). */
u8 ll_version_exchange_pending(struct le_link *link);
/* platform_jieli.c (BR23): around hci_controller_init (task_dispatch.c). */
void btctrlermod_init_begin(void);
void btctrlermod_init_end(void);

#endif
