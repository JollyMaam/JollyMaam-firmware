#ifndef BTSTACK_QOS_JIELI_H
#define BTSTACK_QOS_JIELI_H

/* Hooks run in the SDK controller thread; tick runs in the BTstack thread.
 * This adapter has one ACL connection. State and event enqueue are protected
 * against task/IRQ preemption; no vendor connection pointer is retained. */
void btctrlermod_qos_setup(const unsigned char *parameters);
int btctrlermod_qos_rx(unsigned short handle, unsigned char role,
                   unsigned char header, const unsigned char *parameters, unsigned short size);
void btctrlermod_qos_link_event(const unsigned char *event, unsigned short size);
void btctrlermod_qos_reset(void);
void btctrlermod_qos_tick(void);

#endif
