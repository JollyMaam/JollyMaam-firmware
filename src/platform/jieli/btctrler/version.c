/* The SDK's version.z.o: the controller version record startup keeps in
 * .sys.version (sdk_ld.c KEEP(*(.sys.version))). A plain ELF member holding
 * only this global; these are each chip's exact SDK bytes. */
const unsigned char lib_btctler_version[]
    __attribute__((section(".sys.version"), aligned(1), used)) =
#ifdef CONFIG_CPU_BD19
    "\x7f\xec\xed\x10" "BTCTRLER-@20241231-$d83b33538";
#else
    "\x96\xe9\xed\x10" "BTCTRLER-@20241231-$d83b33538";
#endif

_Static_assert(sizeof(lib_btctler_version) == 34, "controller version record");
