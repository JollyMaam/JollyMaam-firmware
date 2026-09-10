#include "cyclone2_app.h"

/* The SDK implementation is compiled under this private name. Keeping this
 * wrapper in our project lets recovery run before the vendor memory/clock/OS
 * initialization without editing cpu/br23/setup.c. */
extern void sdk_setup_arch(void);

void setup_arch(void)
{
    cyclone2_early_boot();
    sdk_setup_arch();
}
