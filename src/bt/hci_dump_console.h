#ifndef CYCLONE2_HCI_DUMP_CONSOLE_H
#define CYCLONE2_HCI_DUMP_CONSOLE_H

#include <stdint.h>

/* Register the console sink for BTstack's HCI trace and log messages.  Safe to
 * call more than once; the last registration wins (the stack never registers
 * another one on this port). */
void cyclone2_hci_dump_init(void);

#endif
