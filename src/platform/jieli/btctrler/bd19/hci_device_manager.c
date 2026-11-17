/* BD19 hci_device_manager.c: the event mask is the first eight bytes of
 * hci_param and the SDK copies exactly one unaligned 64-bit value. */
#ifdef CONFIG_CPU_BD19
#include <string.h>
#include "btctrler/glue.h"

typedef u64 hci_mask_load_u64 __attribute__((aligned(1), may_alias));
typedef u64 hci_mask_store_u64 __attribute__((aligned(2), may_alias));

void dev_hci_set_event_mask(const u8 *mask)
{
    u64 value = *(const hci_mask_load_u64 *)(const void *)mask;
    *(hci_mask_store_u64 *)(void *)hci_param.event_mask = value;
}
#endif

#ifdef UNITTEST
#ifndef CONFIG_CPU_BD19
int main(void) { return 0; }
#else
#include "test.h"

struct hci_parameter hci_param;

int main(void)
{
    static const u8 expected[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    u8 *raw = (u8 *)&hci_param;
    memcpy(hci_param.event_mask, expected, sizeof(expected));
    raw[8] = 0xa5;
    dev_hci_set_event_mask(expected);
    CHECK(memcmp(hci_param.event_mask, expected, sizeof(expected)) == 0);
    CHECK(raw[8] == 0xa5);

    for (u8 i = 0; i < 9; i++) raw[i] = i;
    dev_hci_set_event_mask(raw + 1);
    for (u8 i = 0; i < 8; i++) CHECK(hci_param.event_mask[i] == i + 1);
    CHECK(raw[8] == 8);
    return TEST_DONE("hci_device_manager_bd19");
}
#endif
#endif
