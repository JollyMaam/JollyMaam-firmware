/* The stub loader (OTA milestone 1): the image uboot.boot starts.  The main
 * app is linked MAIN_AT above it in the same code window and starts as if
 * uboot.boot had started it: the stub passes uboot's boot information (r0)
 * through untouched and changes no hardware.  Without a main app the stub
 * stops; the ROM loader is then Home+Start at plug-in.  (Entering it as
 * plat_enter_loader() does -- boot token, watchdog at 256 ms -- left the
 * ROM loader enumerated but reset by the host over and over, from the
 * stub; why is not established.)
 *
 * Free-standing: no SDK, no .data or .bss; it runs on uboot's stack. */

typedef unsigned int u32;

/* The code window starts at the app area: CPU 0x1e00000 is the flash
 * offset of app_area_head. */
#define CODE_WINDOW     0x1e00000u
#define MAIN_HEAD       ((const volatile u32 *)(CODE_WINDOW + MAIN_AT))
#define MAIN_ENTRY      (CODE_WINDOW + MAIN_AT + 0x120)
#define MAIN_MAGIC      0x4e49414du     /* "MAIN", written by jlnewfw --main */

void __attribute__((section(".startup.text"), noreturn)) _start(void *boot_info)
{
    if (MAIN_HEAD[0] == MAIN_MAGIC && MAIN_HEAD[3] == MAIN_ENTRY)
        ((void (*)(void *))MAIN_ENTRY)(boot_info);
    for (;;) {
    }
}
