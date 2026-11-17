/* The SDK keeps this externally called TWS synchronization hook empty. */
#include "btctrler/glue.h"

void tws_tx_sync_init(void)
{
}

#ifdef UNITTEST
#include "test.h"

int main(void)
{
    tws_tx_sync_init();
    return TEST_DONE("btctrler_tws_tx_sync");
}
#endif
