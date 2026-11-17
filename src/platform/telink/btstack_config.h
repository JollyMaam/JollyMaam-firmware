#ifndef TELINK_BTSTACK_CONFIG_H
#define TELINK_BTSTACK_CONFIG_H

/* Only the upstream ATT database interpreter and byte utilities. Telink
 * continues to own the controller, L2CAP reassembly, GAP and SMP. */
#define HAVE_STDINT_H
#define HAVE_STDBOOL_H
#define ENABLE_BLE

#endif
