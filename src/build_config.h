/* Build configuration.  Committed and edited by hand; `make` picks up a change
 * through the normal header dependencies, so only what includes this file is
 * recompiled - and the controller archive is re-patched, because its rule
 * depends on this file too (tools/patch_btctrler.py --config reads it).
 */
#ifndef CYCLONE2_BUILD_CONFIG_H
#define CYCLONE2_BUILD_CONFIG_H

/* 1: keep the link-key store, the pad's address and its profile settings in
 * RAM only - every boot starts unpaired and nothing is written to flash
 * (docs/bond-store.md).  0: persist them in syscfg flash, as the product
 * does. */
#define CYCLONE2_BOND_STORE_RAM 0

/* How tools/patch_btctrler.py patches the vendor controller archive
 * btctrler.a.  Keep it a string literal on one line; the patcher parses it.
 *
 *   "none"                    the vendor archive untouched
 *   "notify"                  hand the host the link key the console-driven
 *                             SSP pairing derives
 *   "markers"                 notify + quiet LMP opcode/AU_RAND/SRES/detach
 *                             markers in the bring-up ring (`status`)
 *   "markers-no-supervision"  markers + never send LMP_SUPERVISION_TIMEOUT
 *   "markers-role-guard"      markers + no LMP_SUPERVISION_TIMEOUT as
 *                             peripheral
 *   "trace"                   notify + LMP key printfs (needs DEBUG_LOG)
 *   "diagnostic"              the old sniff-override experiment; breaks SSP
 */
#define CYCLONE2_PATCH_BTCTLER "markers-role-guard"

#endif /* CYCLONE2_BUILD_CONFIG_H */
