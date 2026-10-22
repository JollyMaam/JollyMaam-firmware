/* Startup expects this controller version record in .sys.version.
 * Preserve the SDK record bytes while linking the owned controller directly. */
const unsigned char lib_btctler_version[]
    __attribute__((section(".sys.version"), aligned(1), used)) =
    "\x96\xe9\xed\x10" "BTCTRLER-@20241231-$d83b33538";

_Static_assert(sizeof(lib_btctler_version) == 34, "controller version record");
