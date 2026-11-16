/* BD19 version.z.o: the controller version record startup keeps in
 * .sys.version (sdk_ld.c KEEP(*(.sys.version))). The SDK member is a plain
 * ELF object holding only this global; these are its exact bytes. */
const unsigned char lib_btctler_version[]
    __attribute__((section(".sys.version"), aligned(1), used)) =
    "\x7f\xec\xed\x10" "BTCTRLER-@20241231-$d83b33538";

_Static_assert(sizeof(lib_btctler_version) == 34, "controller version record");
