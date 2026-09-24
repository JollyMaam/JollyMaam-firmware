#ifndef CYCLONE2_HCI_DUMP_CONSOLE_H
#define CYCLONE2_HCI_DUMP_CONSOLE_H

#include <stdint.h>

/* Register the console sink for BTstack's HCI trace and log messages.  Safe to
 * call more than once; the last registration wins (the stack never registers
 * another one on this port). */
void cyclone2_hci_dump_init(void);

/* Packet logging is off by default: a live HID session is mostly input
 * reports, and the console's debug ring is small.  The sink still prints
 * every log message while it is off. */
void cyclone2_hci_dump_set_packets(uint8_t enable);
uint8_t cyclone2_hci_dump_packets_enabled(void);

#endif
