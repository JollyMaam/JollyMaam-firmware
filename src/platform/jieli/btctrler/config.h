/* The controller configuration this firmware is built with.  The SDK reads
 * it through the constants in platform_jieli_sdk.c; owned controller code
 * is written for exactly these values and asserts the ones it relies on,
 * so changing one here fails the build where owned code would need work. */
#ifndef BTCTRLER_CONFIG_H
#define BTCTRLER_CONFIG_H

#define BTCFG_HCI_STANDARD     1   /* config_btctler_hci_standard: a standard HCI host */
#define BTCFG_TWS              0   /* CONFIG_BTCTLER_TWS_ENABLE: true-wireless pairs */
#define BTCFG_FAST_CONNECT     0   /* CONFIG_BTCTLER_FAST_CONNECT_ENABLE */
#define BTCFG_BREDR_MASTER     0   /* config_btctler_bredr_master: always stay central */
#define BTCFG_NAME_REQ         1   /* CONFIG_LMP_NAME_REQ_ENABLE: remote name requests */
#define BTCFG_PASSKEY          0   /* CONFIG_LMP_PASSKEY_ENABLE: SSP passkey entry */
#define BTCFG_MASTER_ESCO      0   /* CONFIG_LMP_MASTER_ESCO_ENABLE: voice links */
#define BTCFG_ESCO_FORWARD     0   /* ESCO_FORWARD_ENABLE */
#define BTCFG_BREDR_INQUIRY    0   /* CONFIG_BREDR_INQUIRY: we never inquire */
#define BTCFG_ASSER            0   /* config_asser: assertion failures reset the chip */

#endif
