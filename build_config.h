/* Build configuration: the switches that used to be make variables.
 *
 * Edit a value here and run plain `make`; only what the change affects is
 * rebuilt.  The Makefile reads this file on every run (it is not compiled
 * directly) and writes the values into $O/cyclone2_build_config.h, touching
 * that header only when a value actually changed.  A `make VAR=value` on the
 * command line still overrides a value for that one run.
 *
 * Keep each value on its own `#define NAME value` line; the Makefile parses
 * them with sed.
 */

/* 1: keep the link-key store, the pad's address and its profile settings in
 * RAM only - every boot starts unpaired and nothing is written to flash
 * (docs/bond-store.md).  0: persist them in syscfg flash, as the product
 * does.  Changing it recompiles only the files that use it. */
#define BOND_STORE_RAM 1

/* How to patch the vendor controller archive btctrler.a
 * (tools/patch_btctrler.py).  Changing it re-patches the archive and relinks.
 *
 *   0                       the vendor archive untouched
 *   1                       notify: hand the host the link key the
 *                           console-driven SSP pairing derives
 *   markers                 1 + quiet LMP opcode/AU_RAND/SRES/detach markers
 *                           in the bring-up ring (`status`)
 *   markers-no-supervision  markers + never send LMP_SUPERVISION_TIMEOUT
 *   markers-role-guard      markers + no LMP_SUPERVISION_TIMEOUT as peripheral
 *   trace                   1 + LMP key printfs (needs DEBUG_LOG)
 *   diag                    the old sniff-override experiment; breaks SSP
 */
#define PATCH_BTCTLER markers
