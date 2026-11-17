/* Link-layer white-list storage and matching. The SDK's duplicate entries,
 * address-only removal, post-free comparisons, and 25-entry equality guard
 * are kept as emitted. */
#include "btctrler/glue.h"
#include <string.h>

_Static_assert(offsetof(struct white_list, entry) == 8, "white-list entry offset");
#ifndef UNITTEST
_Static_assert(offsetof(struct le_parameter, white_list_head) == 152,
               "white-list head offset");
_Static_assert(offsetof(struct white_list, cnt) == 16, "white-list count offset");
_Static_assert(sizeof(struct white_list) == 20, "white-list allocation size");
#endif

static u8 ll_white_list_used __attribute__((section(".ble_ll_bss"))) = 0;

extern const u8 log_tag_const_e_LL_WL;
extern const u8 log_tag_const_i_LL_WL;
extern void log_print(int level, const char *tag, const char *format, ...);

static void INIT_LIST_HEAD(void)
{
    struct list_head *head = &le_param.white_list_head;
    head->next = head;
    head->prev = head;
}

static void list_add(struct list_head *entry)
{
    struct list_head *head = &le_param.white_list_head;
    struct list_head *next = head->next;
    next->prev = entry;
    entry->next = next;
    entry->prev = head;
    head->next = entry;
}

static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

struct white_list *__attribute__((section(".ble_ll_code")))
ll_white_list_match(void *priv, u8 addr_type, const u8 *addr)
{
    (void)priv;
    struct list_head *head = &le_param.white_list_head;
    for (struct list_head *node = head->next; node != head; node = node->next) {
        struct white_list *entry = (struct white_list *)((u8 *)node - 8);
        if (memcmp(addr, entry->white_list_param.Address, 6) == 0 &&
            entry->white_list_param.Address_Type == addr_type)
            return entry;
    }
    return NULL;
}

void __attribute__((section(".ble_ll_code"))) ll_white_list_init(void)
{
    ll_white_list_used = 0;
    INIT_LIST_HEAD();
}

void __attribute__((section(".ble_ll_code"))) ll_white_list_add(const u8 *data)
{
    if (ll_white_list_used == 25) {
        if (log_tag_const_e_LL_WL)
            log_print(4, NULL, "<Error>: [LL_WL]No white list\r\n");
        return;
    }

    struct white_list *entry = btctler_nv_memory_malloc(20);
    if (!entry) {
        /* The SDK ASSERTs once in the allocator and again in its caller.
         * Both calls return here if the configured assertion hook returns. */
        btctrler_assert_failed("p != NULL", 0);
        btctrler_assert_failed("list != NULL", 0);
    }
    memcpy(entry, data, 12);
    entry->cnt = 0;
    list_add(&entry->entry);
    ll_white_list_used = (u8)(ll_white_list_used + 1);

    if (log_tag_const_i_LL_WL) {
        char *address = btctler_bd_addr_to_str(entry->white_list_param.Address);
        log_print(2, NULL, "[Info]: [LL_WL]White list add : %d - %d / %s\r\n",
                  ll_white_list_used, entry->white_list_param.Address_Type, address);
    }
}

u8 __attribute__((section(".ble_ll_code"))) ll_white_list_remove(const u8 *data)
{
    struct list_head *head = &le_param.white_list_head;
    const u8 *address = data + 1;
    for (struct list_head *node = head->next; node != head;) {
        struct list_head *next = node->next;
        struct white_list *entry = (struct white_list *)((u8 *)node - 8);
        if (memcmp(address, entry->white_list_param.Address, 6) == 0) {
            ll_white_list_used = (u8)(ll_white_list_used - 1);
            list_del(node);
            if (log_tag_const_i_LL_WL) {
                char *formatted = btctler_bd_addr_to_str(entry->white_list_param.Address);
                log_print(2, NULL, "[Info]: [LL_WL]White list remove: %d / %s\r\n",
                          entry->white_list_param.Address_Type, formatted);
            }
            btctler_nv_memory_free(entry);
        }
        /* This second compare deliberately uses the entry after free, and
         * checks the address before the requested type as the SDK does. */
        if (memcmp(address, entry->white_list_param.Address, 6) == 0 &&
            entry->white_list_param.Address_Type == data[0])
            return 0;
        node = next;
    }
    return 18;
}

#ifdef UNITTEST
#include "test.h"

static u8 allocations[64][64] __attribute__((aligned(16)));
static unsigned int allocation_count, free_count, assert_count, reset_count;
static unsigned int log_count, error_log_count;
struct le_parameter le_param __attribute__((aligned(8)));
int config_asser;
const u8 log_tag_const_e_LL_WL = 1;
const u8 log_tag_const_i_LL_WL = 0;

void *btctler_nv_memory_malloc(int size)
{
    CHECK(size == 20);
    CHECK(allocation_count < sizeof(allocations) / sizeof(allocations[0]));
    return allocations[allocation_count++];
}

void btctler_nv_memory_free(void *ptr)
{
    CHECK(ptr != NULL);
    ++free_count; /* Keep backing bytes so the SDK's post-free read is testable. */
}

void btctrler_assert_failed(const char *what, int value)
{
    CHECK(value == 0);
    CHECK(!strcmp(what, "p != NULL") || !strcmp(what, "list != NULL"));
    ++assert_count;
}

void p33_soft_reset(void) { ++reset_count; }

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag;
    if (level == 4) ++error_log_count;
    ++log_count;
    CHECK(format != NULL);
}

char *btctler_bd_addr_to_str(const uint8_t *addr)
{
    (void)addr;
    return "00:00:00:00:00:00";
}

static void make_data(u8 data[12], u8 type, u8 address)
{
    memset(data, 0, 12);
    data[0] = type;
    for (unsigned int i = 0; i < 6; ++i) data[i + 1] = (u8)(address + i);
}

int main(void)
{
    u8 item[12], query[12];
    const u8 address[] = { 1, 2, 3, 4, 5, 6 };
    const u8 other_address[] = { 2, 3, 4, 5, 6, 7 };

    ll_white_list_init();
    CHECK(ll_white_list_match(NULL, 0, address) == NULL);
    make_data(query, 9, 40);
    CHECK(ll_white_list_remove(query) == 18);

    make_data(item, 3, 1);
    item[7] = 0xa7; /* memcpy's byte 12 target includes this padding byte. */
    ll_white_list_add(item);
    struct white_list *found = ll_white_list_match(NULL, 3, item + 1);
    CHECK(found != NULL && found->white_list_param.Address_Type == 3);
    CHECK(ll_white_list_match(NULL, 4, item + 1) == NULL);
    CHECK(ll_white_list_match(NULL, 3, address) == found);
    CHECK(ll_white_list_match(NULL, 3, other_address) == NULL);
    CHECK(found->cnt == 0);
    CHECK(((const u8 *)found)[7] == 0xa7);

    /* Duplicate address entries remain allowed. Address-only removal deletes
     * a wrong-type entry, reads its bytes after free, then keeps searching. */
    make_data(item, 1, 20);
    ll_white_list_add(item);
    make_data(item, 2, 20);
    ll_white_list_add(item);
    struct white_list *duplicate_type_1 = ll_white_list_match(NULL, 1, item + 1);
    struct white_list *duplicate_type_2 = ll_white_list_match(NULL, 2, item + 1);
    CHECK(duplicate_type_1 != NULL && duplicate_type_2 != NULL &&
          duplicate_type_1 != duplicate_type_2);
    make_data(query, 7, 20);
    unsigned int before_frees = free_count;
    CHECK(ll_white_list_remove(query) == 18);
    CHECK(free_count == before_frees + 2);
    CHECK(ll_white_list_match(NULL, 1, query + 1) == NULL);
    CHECK(ll_white_list_match(NULL, 2, query + 1) == NULL);

    /* Reinitialization abandons nodes without freeing them. */
    make_data(item, 5, 60);
    ll_white_list_add(item);
    before_frees = free_count;
    unsigned int before_allocations = allocation_count;
    ll_white_list_init();
    CHECK(free_count == before_frees && allocation_count == before_allocations);
    CHECK(ll_white_list_match(NULL, 5, item + 1) == NULL);

    /* The guard is equality-only, and the SDK count is an 8-bit counter. */
    ll_white_list_used = 26;
    make_data(item, 4, 70);
    before_allocations = allocation_count;
    ll_white_list_add(item);
    CHECK(allocation_count == before_allocations + 1 && ll_white_list_used == 27);
    ll_white_list_init();
    ll_white_list_used = 255;
    make_data(item, 4, 71);
    ll_white_list_add(item);
    CHECK(ll_white_list_used == 0);
    ll_white_list_init();

    /* The full guard is equality with 25 and logs instead of allocating. */
    before_allocations = allocation_count;
    for (u8 i = 0; i < 25; ++i) {
        make_data(item, i, (u8)(80 + i));
        ll_white_list_add(item);
    }
    CHECK(allocation_count == before_allocations + 25);
    make_data(item, 1, 200);
    before_allocations = allocation_count;
    unsigned int before_errors = error_log_count;
    ll_white_list_add(item);
    CHECK(allocation_count == before_allocations);
    CHECK(error_log_count == before_errors + 1);
    CHECK(ll_white_list_match(NULL, 1, item + 1) == NULL);

    make_data(query, 0, 80);
    CHECK(ll_white_list_remove(query) == 0);
    CHECK(ll_white_list_match(NULL, 0, query + 1) == NULL);
    make_data(item, 1, 200);
    before_allocations = allocation_count;
    ll_white_list_add(item);
    CHECK(allocation_count == before_allocations + 1);
    CHECK(ll_white_list_match(NULL, 1, item + 1) != NULL);
    CHECK(assert_count == 0 && reset_count == 0);
    CHECK(log_count == error_log_count);
    return TEST_DONE("btctrler_ll_white_list");
}
#endif
