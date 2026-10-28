/* LE privacy: the resolving list (add, remove, match), resolvable private
 * address generation and resolution, and the periodic RPA refresh timer.
 * An RPA is prand (3 bytes at addr[3..5], top two bits 01) plus the hash
 * ah(IRK, prand) in addr[0..2], computed by the AES block with the 16-byte
 * plaintext holding prand in its first three bytes.  SDK quirks kept: the
 * list holds one entry (adding a second is ignored silently, the counter is
 * compared with 1); removal does not stop at the first match; a failed
 * allocation asserts and then copies into NULL; the local and peer RPA
 * fields of a new entry are left unset; a zero timeout becomes 900000 ms
 * only when privacy is enabled; the random words are read from the hardware
 * RNG at 0x1e3b00 and 0x1e3b04, low byte only.  debug_ll_privacy and
 * ll_vendor_device_address_generate have no caller and are not carried
 * over on BR23. BD19's whole-member ownership also retains the SDK's debug
 * exports and initialized debug table. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_RL, log_tag_const_i_LL_RL;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const u8 *buf, int size);

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifdef UNITTEST
extern volatile u32 ll_resolve_list_rng[2];
#define RNG_WORD0 ll_resolve_list_rng[0]
#define RNG_WORD1 ll_resolve_list_rng[1]
#else
#define RNG_WORD0 (*(volatile u32 *)0x1e3b00)
#define RNG_WORD1 (*(volatile u32 *)0x1e3b04)
#endif

_Static_assert(offsetof(struct resolving_list, entry) == 40, "SDK resolving list entry offset");
_Static_assert(sizeof(struct resolving_list_parameter) == 39, "SDK resolving list parameter size");
_Static_assert(offsetof(struct resolving_list_parameter, Peer_IRK) == 7, "Peer_IRK");
_Static_assert(offsetof(struct resolving_list_parameter, Local_IRK) == 23, "Local_IRK");
#ifndef UNITTEST
_Static_assert(offsetof(struct resolving_list, local_PRA) == 48, "local_PRA");
_Static_assert(offsetof(struct resolving_list, peer_PRA) == 54, "peer_PRA");
_Static_assert(sizeof(struct resolving_list) == 60, "SDK resolving list allocation size");
#endif

static u8 ll_resolving_list_used __attribute__((section(".ble_ll_bss"))) = 0;
struct ll_privacy le_privacy __attribute__((section(".ble_ll_bss")));

#ifdef CONFIG_CPU_BD19
struct rpa rpa_set[2] __attribute__((section(".ble_ll_data"), aligned(1))) = {
    {
        { 0x76, 0x41, 0x46, 0x31, 0x3a, 0x4d },
        { 0x74, 0x01, 0x90, 0x4a, 0xf7, 0x35, 0xa4, 0x38,
          0xd3, 0x20, 0x9a, 0xcd, 0x48, 0xc0, 0x54, 0x40 },
        { 0xe6, 0xea, 0xee, 0x60, 0x31, 0x7b, 0xfc, 0xa2,
          0x3f, 0xa5, 0x79, 0x59, 0xe7, 0x41, 0xcf, 0xc7 },
    },
    {
        { 0xa9, 0x3e, 0xd4, 0x37, 0xb5, 0x75 },
        { 0x74, 0x01, 0x90, 0x4a, 0xf7, 0x35, 0xa4, 0x38,
          0xd3, 0x20, 0x9a, 0xcd, 0x48, 0xc0, 0x54, 0x40 },
        { 0 },
    },
};
#endif

static struct resolving_list *entry_of(struct list_head *node)
{
    return (struct resolving_list *)((u8 *)node - offsetof(struct resolving_list, entry));
}

/* ah(irk, prand): the first three bytes of AES(irk, prand | zeros). */
void irk_enc(const u8 *irk, const u8 *prand, u8 *hash) LL_CODE;
void irk_enc(const u8 *irk, const u8 *prand, u8 *hash)
{
    u8 r[16];
    u8 ah[16];
    memset(&r[3], 0, 13);
    memcpy(r, prand, 3);
    aes128_start((u8 *)irk, r, ah);
    memcpy(hash, ah, 3);
}

/* True when the IRK has any non-zero byte. */
bool ll_resolve_list_IRK_verify(const u8 *irk) LL_CODE;
bool ll_resolve_list_IRK_verify(const u8 *irk)
{
    u8 sum = 0;
    for (int i = 0; i < 16; i++) sum |= irk[i];
    return sum != 0;
}

struct resolving_list *ll_resolving_list_match(const u8 addr_type, const u8 *addr) LL_CODE;
struct resolving_list *ll_resolving_list_match(const u8 addr_type, const u8 *addr)
{
    if (!addr) btctrler_assert_failed("addr != NULL", 0);
    struct list_head *head = &le_param.resolving_list_head;
    for (struct list_head *node = head->next; node != head; node = node->next) {
        struct resolving_list *entry = entry_of(node);
        if (memcmp(addr, entry->resolving_list_param.Peer_Identity_Address, 6) == 0 &&
            entry->resolving_list_param.Peer_Identity_Address_Type == addr_type)
            return entry;
    }
    return NULL;
}

/* prand with the two top bits forced to 01 from the current random48,
 * then the hash; random48 is kept so the address can be reproduced. */
static void device_addr_generate(u8 *addr, const u8 *irk) LL_CODE;
static void device_addr_generate(u8 *addr, const u8 *irk)
{
    le_privacy.random48[2] = (le_privacy.random48[2] & 63) | 64;
    irk_enc(irk, le_privacy.random48, addr);
    memcpy(addr + 3, le_privacy.random48, 3);
}

void ll_resolve_list_set_local_RPA(u8 *addr, struct resolving_list *resolving_list_t) LL_CODE;
void ll_resolve_list_set_local_RPA(u8 *addr, struct resolving_list *resolving_list_t)
{
    if (!resolving_list_t) btctrler_assert_failed("resolving_list_t != NULL", 0);
    if (ll_resolve_list_IRK_verify(resolving_list_t->resolving_list_param.Local_IRK) != 1)
        btctrler_assert_failed("ll_resolve_list_IRK_verify(Local_IRK) == TRUE", 0);
    device_addr_generate(resolving_list_t->local_PRA, resolving_list_t->resolving_list_param.Local_IRK);
    memcpy(addr, resolving_list_t->local_PRA, 6);
}

void ll_resolve_list_set_peer_RPA(u8 *addr, struct resolving_list *resolving_list_t) LL_CODE;
void ll_resolve_list_set_peer_RPA(u8 *addr, struct resolving_list *resolving_list_t)
{
    if (!resolving_list_t) btctrler_assert_failed("resolving_list_t != NULL", 0);
    if (ll_resolve_list_IRK_verify(resolving_list_t->resolving_list_param.Peer_IRK) != 1)
        btctrler_assert_failed("ll_resolve_list_IRK_verify(Peer_IRK) == TRUE", 0);
    device_addr_generate(resolving_list_t->peer_PRA, resolving_list_t->resolving_list_param.Peer_IRK);
    memcpy(addr, resolving_list_t->peer_PRA, 6);
}

/* Resolvable: the two top bits of the last byte are 01 and prand (bytes 3..5,
 * top bits forced) is neither all zeros nor all ones (device_addr_verify). */
bool ll_resolvable_private_addr_verify(const u8 *addr) LL_CODE;
bool ll_resolvable_private_addr_verify(const u8 *addr)
{
    if ((addr[5] & 0xc0) != 0x40) return false;
    return device_addr_verify(addr, 3, 3);
}

/* The entry whose peer IRK resolves the address, or NULL. */
struct resolving_list *ll_resolve_peer_addr(const u8 *addr) LL_CODE;
struct resolving_list *ll_resolve_peer_addr(const u8 *addr)
{
    u8 hash[3];
    if (log_tag_const_i_LL_RL)
        log_print(2, NULL, "[Info]: [LL_RL]conn_receive_adv_addr %s\r\n", btctler_bd_addr_to_str(addr));
    if (!ll_resolvable_private_addr_verify(addr)) {
        if (log_tag_const_e_LL_RL)
            log_print(4, NULL, "<Error>: [LL_RL]peer addr format error\r\n");
        return NULL;
    }
    struct list_head *head = &le_param.resolving_list_head;
    for (struct list_head *node = head->next; node != head; node = node->next) {
        struct resolving_list *entry = entry_of(node);
        irk_enc(entry->resolving_list_param.Peer_IRK, addr + 3, hash);
        if (memcmp(hash, addr, 3) == 0) return entry;
    }
    if (log_tag_const_e_LL_RL)
        log_print(4, NULL, "<Error>: [LL_RL]RPA resolved fail\r\n");
    return NULL;
}

/* Same with the local IRK (no log on the miss paths other than a bad format). */
struct resolving_list *ll_resolve_local_addr(const u8 *addr) LL_CODE;
struct resolving_list *ll_resolve_local_addr(const u8 *addr)
{
    u8 hash[3];
    if (!ll_resolvable_private_addr_verify(addr)) {
        if (log_tag_const_e_LL_RL)
            log_print(4, NULL, "<Error>: [LL_RL]ll resolve local addr format error\r\n");
        return NULL;
    }
    struct list_head *head = &le_param.resolving_list_head;
    for (struct list_head *node = head->next; node != head; node = node->next) {
        struct resolving_list *entry = entry_of(node);
        irk_enc(entry->resolving_list_param.Local_IRK, addr + 3, hash);
        if (memcmp(hash, addr, 3) == 0) return entry;
    }
    return NULL;
}

static void INIT_LIST_HEAD(void) LL_CODE;
static void INIT_LIST_HEAD(void)
{
    le_param.resolving_list_head.next = &le_param.resolving_list_head;
    le_param.resolving_list_head.prev = &le_param.resolving_list_head;
}

void ll_resolving_list_init(void) LL_CODE;
void ll_resolving_list_init(void)
{
    ll_resolving_list_used = 0;
    INIT_LIST_HEAD();
}

static void list_add_tail(struct list_head *entry) LL_CODE;
static void list_add_tail(struct list_head *entry)
{
    struct list_head *head = &le_param.resolving_list_head;
    struct list_head *prev = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = prev;
    prev->next = entry;
}

static void list_del(struct list_head *entry) LL_CODE;
static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

/* data: type, address (6), peer IRK (16), local IRK (16) = 39 bytes. */
void ll_resolving_list_add(const u8 *data) LL_CODE;
void ll_resolving_list_add(const u8 *data)
{
    if (ll_resolving_list_used == 1) return;
    struct resolving_list *entry = btctler_nv_memory_malloc(60);
    if (!entry) {
        /* The SDK ASSERTs once in the allocator and again for the list;
         * both return here if the configured assertion hook returns. */
        btctrler_assert_failed("p != NULL", 0);
        btctrler_assert_failed("list != NULL", 0);
    }
    memcpy(entry, data, 39);
    if (log_tag_const_i_LL_RL) {
        log_print(2, NULL, "[Info]: [LL_RL] ----------Resolving List --------------\r\n");
        log_print(2, NULL, "[Info]: [LL_RL]addr type : %x\r\n",
                  entry->resolving_list_param.Peer_Identity_Address_Type);
        char *address = btctler_bd_addr_to_str(entry->resolving_list_param.Peer_Identity_Address);
        log_print(2, NULL, "[Info]: [LL_RL]addr : %s\r\n", address);
        log_print(2, NULL, "[Info]: [LL_RL]local IRK : \r\n");
        printf_buf(entry->resolving_list_param.Local_IRK, 16);
        log_print(2, NULL, "[Info]: [LL_RL]peer IRK : \r\n");
        printf_buf(entry->resolving_list_param.Peer_IRK, 16);
        list_add_tail(&entry->entry);
        ll_resolving_list_used = (u8)(ll_resolving_list_used + 1);
        log_print(2, NULL, "[Info]: [LL_RL] ----------Resolving List End--------------\r\n");
    } else {
        list_add_tail(&entry->entry);
        ll_resolving_list_used = (u8)(ll_resolving_list_used + 1);
    }
}

/* data: type, address (6).  Every matching entry is removed. */
void ll_resolving_list_remove(const u8 *data) LL_CODE;
void ll_resolving_list_remove(const u8 *data)
{
    struct list_head *head = &le_param.resolving_list_head;
    struct list_head *node = head->next;
    while (node != head) {
        struct list_head *next = node->next;
        struct resolving_list *entry = entry_of(node);
        if (memcmp(data + 1, entry->resolving_list_param.Peer_Identity_Address, 6) == 0 &&
            entry->resolving_list_param.Peer_Identity_Address_Type == data[0]) {
            ll_resolving_list_used = (u8)(ll_resolving_list_used - 1);
            list_del(node);
            btctler_nv_memory_free(entry);
        }
        node = next;
    }
}

void ll_read_peer_resolvable_address(const u8 addr_type, const u8 *addr, u8 *peer_PRA) LL_CODE;
void ll_read_peer_resolvable_address(const u8 addr_type, const u8 *addr, u8 *peer_PRA)
{
    struct resolving_list *entry = ll_resolving_list_match(addr_type, addr);
    if (!entry) {
        if (log_tag_const_e_LL_RL)
            log_print(4, NULL, "<Error>: [LL_RL]- No resolving list match\r\n");
        return;
    }
    memcpy(peer_PRA, entry->peer_PRA, 6);
}

void ll_read_local_resolvable_address(const u8 addr_type, const u8 *addr, u8 *local_PRA) LL_CODE;
void ll_read_local_resolvable_address(const u8 addr_type, const u8 *addr, u8 *local_PRA)
{
    struct resolving_list *entry = ll_resolving_list_match(addr_type, addr);
    if (!entry) {
        if (log_tag_const_e_LL_RL)
            log_print(4, NULL, "<Error>: [LL_RL]- No resolving list match\r\n");
        return;
    }
    memcpy(local_PRA, entry->local_PRA, 6);
}

/* Refresh: new random48, then ask the advertiser to regenerate its address. */
static void __random48_update(void) LL_CODE;
static void __random48_update(void)
{
    for (int i = 0; i < 6; i += 2) {
        le_privacy.random48[i] = (u8)RNG_WORD0;
        le_privacy.random48[i | 1] = (u8)RNG_WORD1;
    }
}

static void ll_RPA_timeout_handler(u32 *timer) LL_CODE;
static void ll_RPA_timeout_handler(u32 *timer)
{
    (void)timer;
    if (log_tag_const_e_LL_RL)
        log_print(4, NULL, "<Error>: [LL_RL]TO -----LL RPA Timeout\r\n");
    __random48_update();
    ll_adv_local_addr_refresh();
}

static void ll_RPA_timeout_stop(void) LL_CODE;
static void ll_RPA_timeout_stop(void)
{
    if (le_privacy.RPA_timeout) {
        sys_timer_remove(le_privacy.RPA_timeout);
        le_privacy.RPA_timeout = 0;
    }
}

static void ll_RPA_timeout_start(u32 timeout) LL_CODE;
static void ll_RPA_timeout_start(u32 timeout)
{
    if (log_tag_const_i_LL_RL)
        log_print(2, NULL, "[Info]: [LL_RL]RPA timeout %x\r\n", timeout);
    sys_timer timer = sys_timer_register_periodic(timeout, (void (*)(void *))ll_RPA_timeout_handler);
    le_privacy.RPA_timeout = timer;
    if (!timer) btctrler_assert_failed("le_privacy.RPA_timeout", 0);
}

/* data: the timeout in seconds (little endian), kept in milliseconds. */
void ll_set_resolvable_private_address_timeout(const u8 *data) LL_CODE;
void ll_set_resolvable_private_address_timeout(const u8 *data)
{
    ll_RPA_timeout_stop();
    u32 timeout = (u32)btctler_little_endian_read_16(data, 0) * 1000;
    le_privacy.RPA_timeout_cnt = timeout;
    ll_RPA_timeout_start(timeout);
}

void ll_resolution_enable(void) LL_CODE;
void ll_resolution_enable(void)
{
    if (le_privacy.RPA_timeout_cnt == 0) le_privacy.RPA_timeout_cnt = 900000;
    __random48_update();
    ll_RPA_timeout_start(le_privacy.RPA_timeout_cnt);
}

void ll_resolution_disable(void) LL_CODE;
void ll_resolution_disable(void)
{
    if ((config_btctler_le_features & 64) && le_param.resolution_enable != 0)
        ll_RPA_timeout_stop();
}

/* Present in the BD19 member even though the current BR23 source did not
 * carry this vendor helper. Preserve its RNG reads for every type, including
 * unknown types that leave the destination untouched. */
#ifdef CONFIG_CPU_BD19
void ll_vendor_device_address_generate(u8 *addr, u8 *irk, u8 type) LL_CODE;
void ll_vendor_device_address_generate(u8 *addr, u8 *irk, u8 type)
{
    u8 random_addr[6];
    for (int i = 0; i < 6; i += 2) {
        random_addr[i] = (u8)RNG_WORD0;
        random_addr[i + 1] = (u8)RNG_WORD1;
    }
    switch (type) {
    case 1:
        random_addr[5] |= 0xc0;
        memcpy(addr, random_addr, 6);
        break;
    case 2:
        random_addr[5] &= 0x3f;
        memcpy(addr, random_addr, 6);
        break;
    case 3: {
        random_addr[2] = (random_addr[2] & 0x3f) | 0x40;
        u8 input[16] = { 0 };
        u8 encrypted[16];
        memcpy(input, random_addr, 3);
        aes128_start(irk, input, encrypted);
        memcpy(addr, encrypted, 3);
        memcpy(addr + 3, random_addr, 3);
        break;
    }
    default:
        break;
    }
}

void debug_ll_privacy(void) LL_CODE;
void debug_ll_privacy(void)
{
    u8 input[16] = { 0 }, encrypted[16], hash[3], generated[6];
    const u8 *peer_prand = rpa_set[0].addr + 3;
    bool verbose = log_tag_const_i_LL_RL != 0;

    if (verbose) {
        log_print(2, NULL, "[Info]: [LL_RL]---------------LL privacy debug---------\r\n");
        log_print(2, NULL, "[Info]: [LL_RL]RPA : %s\r\n", btctler_bd_addr_to_str(rpa_set[0].addr));
        log_print(2, NULL, "[Info]: [LL_RL]peer IRK : \r\n");
        printf_buf(rpa_set[0].peer_irk, 16);
    }
    if (ll_resolvable_private_addr_verify(rpa_set[0].addr)) {
        if (verbose) {
            log_print(2, NULL, "[Info]: [LL_RL]prand \r\n");
            printf_buf(peer_prand, 3);
        }
        memcpy(input, peer_prand, 3);
        aes128_start(rpa_set[0].peer_irk, input, encrypted);
        memcpy(hash, encrypted, 3);
        if (verbose) {
            log_print(2, NULL, "[Info]: [LL_RL]hash \r\n");
            printf_buf(hash, 3);
        }
        if (memcmp(hash, rpa_set[0].addr, 3) == 0 && verbose)
            log_print(2, NULL, "[Info]: [LL_RL]ll peer private addr resolved!\r\n");
    } else if (verbose) {
        log_print(2, NULL, "[Info]: [LL_RL]ll resolve addr format error\r\n");
    }

    le_privacy.random48[2] = (le_privacy.random48[2] & 63) | 64;
    memcpy(input, le_privacy.random48, 3);
    aes128_start(rpa_set[0].local_irk, input, encrypted);
    memcpy(generated, encrypted, 3);
    memcpy(generated + 3, le_privacy.random48, 3);
    if (verbose) {
        log_print(2, NULL, "[Info]: [LL_RL]RPA : %s\r\n", btctler_bd_addr_to_str(generated));
        log_print(2, NULL, "[Info]: [LL_RL]local IRK : \r\n");
        printf_buf(rpa_set[0].local_irk, 16);
        log_print(2, NULL, "[Info]: [LL_RL]prand \r\n");
        printf_buf(generated + 3, 3);
        memset(input, 0, sizeof(input));
        memcpy(input, generated + 3, 3);
        aes128_start(rpa_set[0].local_irk, input, encrypted);
        memcpy(hash, encrypted, 3);
        log_print(2, NULL, "[Info]: [LL_RL]hash \r\n");
        printf_buf(hash, 3);
        if (memcmp(hash, generated, 3) == 0)
            log_print(2, NULL, "[Info]: [LL_RL]ll local private addr resolved!\r\n");
    }
    if (verbose)
        log_print(2, NULL, "[Info]: [LL_RL]---------------LL privacy debug end---------\r\n");
}
#endif

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>

struct le_parameter le_param;
u8 log_tag_const_e_LL_RL, log_tag_const_i_LL_RL;
u64 config_btctler_le_features;
volatile u32 ll_resolve_list_rng[2];

static int errors, infos, asserts, bufs, mallocs, frees, timers_registered, timers_removed, refreshes;
static int malloc_fail, timer_result = 77, verify_result = 1, verify_calls;
static u32 timer_msec;
static void (*timer_cb)(void *);
static int removed_timer;
static void *last_freed;
static u8 verify_arg[6];

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag; (void)format;
    if (level == 4) errors++; else infos++;
}
void printf_buf(const u8 *buf, int size) { (void)buf; (void)size; bufs++; }
char *btctler_bd_addr_to_str(const uint8_t *addr) { (void)addr; return "addr"; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
void *btctler_nv_memory_malloc(int size)
{
    CHECK(size == 60);
    mallocs++;
    return malloc_fail ? NULL : calloc(1, 128);   /* the host structs are larger than the SDK ones */
}
void btctler_nv_memory_free(void *p) { frees++; last_freed = p; }
bool device_addr_verify(const u8 *addr, u8 start, u8 len)
{
    CHECK(start == 3 && len == 3);
    verify_calls++;
    memcpy(verify_arg, addr, 6);
    return verify_result;
}
sys_timer sys_timer_register_periodic(u32 msec, void (*callback)(void *))
{
    timers_registered++; timer_msec = msec; timer_cb = callback;
    return timer_result;
}
void sys_timer_remove(sys_timer timer) { timers_removed++; removed_timer = timer; }
uint16_t btctler_little_endian_read_16(const uint8_t *buffer, int pos) { return (uint16_t)(buffer[pos] | buffer[pos + 1] << 8); }
int ll_adv_local_addr_refresh(void) { refreshes++; return 0; }

/* Independent AES-128 for the model of the engine. */
static u8 sbox[256];
static void sbox_init(void)
{
    u8 p = 1, q = 1;
    do {
        p = p ^ (u8)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
        q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
        u8 x = q ^ (u8)(q << 1 | q >> 7) ^ (u8)(q << 2 | q >> 6) ^ (u8)(q << 3 | q >> 5) ^ (u8)(q << 4 | q >> 4);
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
}
static u8 xt(u8 x) { return (u8)(x << 1) ^ (x & 0x80 ? 0x1b : 0); }
/* Both blocks most significant byte first. */
static void aes_msb(const u8 key[16], const u8 in[16], u8 out[16])
{
    u8 rk[176], s[16];
    memcpy(rk, key, 16);
    for (int i = 16, rc = 1; i < 176; i += 4) {
        u8 t[4]; memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            u8 a = t[0]; t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[a];
            rc = xt((u8)rc);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        u8 t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) s[c * 4 + row] = t[((c + row) % 4) * 4 + row];
        if (r != 10)
            for (int c = 0; c < 4; c++) {
                u8 *col = s + c * 4, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                col[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                col[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                col[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}
/* The engine as irk_enc uses it: byte 0 of key, plaintext and result is the
 * least significant (the order the HCI and the air interface use).  This is
 * the assumption under which the SDK's irk_enc computes the spec's ah(); the
 * real engine's order was not checked here (aes_hw.c). */
static int aes_calls;
void aes128_start(u8 *key, u8 *plaintext, u8 *encrypt)
{
    u8 k[16], p[16], o[16];
    aes_calls++;
    for (int i = 0; i < 16; i++) { k[i] = key[15 - i]; p[i] = plaintext[15 - i]; }
    aes_msb(k, p, o);
    for (int i = 0; i < 16; i++) encrypt[i] = o[15 - i];
}

/* Core spec Vol 3 Part H D.7: k, r and ah, most significant byte first. */
static const u8 spec_k[16] = { 0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05, 0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b };
static const u8 spec_k_le[16] = { 0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec };
static const u8 spec_rpa[6] = { 0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70 };   /* ah 0dfbaa, prand 708194 */
static const u8 other_irk[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

/* type, address, peer IRK, local IRK */
static void make_data(u8 data[39], u8 type, u8 a, const u8 *peer_irk, const u8 *local_irk)
{
    data[0] = type;
    for (int i = 0; i < 6; i++) data[1 + i] = (u8)(a + i);
    memcpy(data + 7, peer_irk, 16);
    memcpy(data + 23, local_irk, 16);
}

int main(void)
{
    sbox_init();
#ifdef CONFIG_CPU_BD19
    /* Original BD19 member initializer is a 2x38-byte .ble_ll_data table. */
    {
        static const u8 expected_addr[6] = { 0x76, 0x41, 0x46, 0x31, 0x3a, 0x4d };
        static const u8 expected_peer_irk[16] = {
            0x74, 0x01, 0x90, 0x4a, 0xf7, 0x35, 0xa4, 0x38,
            0xd3, 0x20, 0x9a, 0xcd, 0x48, 0xc0, 0x54, 0x40,
        };
        static const u8 expected_local_irk[16] = {
            0xe6, 0xea, 0xee, 0x60, 0x31, 0x7b, 0xfc, 0xa2,
            0x3f, 0xa5, 0x79, 0x59, 0xe7, 0x41, 0xcf, 0xc7,
        };
        CHECK(sizeof(rpa_set) == 76);
        CHECK(memcmp(rpa_set[0].addr, expected_addr, 6) == 0);
        CHECK(memcmp(rpa_set[0].peer_irk, expected_peer_irk, 16) == 0);
        CHECK(memcmp(rpa_set[0].local_irk, expected_local_irk, 16) == 0);
    }
    /* BD19 vendor address helper: RNG byte order and type-specific edits. */
    {
        u8 out[6], before[6] = { 9, 8, 7, 6, 5, 4 };
        ll_resolve_list_rng[0] = 0x11223344;
        ll_resolve_list_rng[1] = 0x55667788;
        memset(out, 0, sizeof(out));
        ll_vendor_device_address_generate(out, (u8 *)spec_k_le, 1);
        CHECK(memcmp(out, (u8[]){ 0x44, 0x88, 0x44, 0x88, 0x44, 0xc8 }, 6) == 0);
        ll_vendor_device_address_generate(out, (u8 *)spec_k_le, 2);
        CHECK(memcmp(out, (u8[]){ 0x44, 0x88, 0x44, 0x88, 0x44, 0x08 }, 6) == 0);
        ll_vendor_device_address_generate(out, (u8 *)spec_k_le, 3);
        CHECK(memcmp(out + 3, (u8[]){ 0x44, 0x88, 0x44 }, 3) == 0);
        CHECK((out[5] & 0xc0) == 0x40);
        ll_vendor_device_address_generate(before, (u8 *)spec_k_le, 0);
        CHECK(memcmp(before, (u8[]){ 9, 8, 7, 6, 5, 4 }, 6) == 0);
    }
#endif
    /* FIPS-197 C.1 checks the AES model itself. */
    {
        u8 k[16], p[16], o[16];
        static const u8 want[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
        for (int i = 0; i < 16; i++) { k[i] = (u8)i; p[i] = (u8)(i * 0x11); }
        aes_msb(k, p, o);
        CHECK(memcmp(o, want, 16) == 0);
    }
    /* Convention check of the model: reversing the spec key reproduces spec_k_le. */
    for (int i = 0; i < 16; i++) CHECK(spec_k_le[i] == spec_k[15 - i]);

    /* ah(k, r) of the spec: r = 0x708194 gives ah = 0x0dfbaa. */
    {
        static const u8 prand[3] = { 0x94, 0x81, 0x70 };
        u8 hash[3] = { 0, 0, 0 };
        irk_enc(spec_k_le, prand, hash);
        CHECK(hash[0] == 0xaa && hash[1] == 0xfb && hash[2] == 0x0d);
    }

    /* IRK_verify: any non-zero byte. */
    {
        u8 irk[16] = { 0 };
        CHECK(ll_resolve_list_IRK_verify(irk) == false);
        for (int i = 0; i < 16; i++) {
            memset(irk, 0, 16); irk[i] = 0x80;
            CHECK(ll_resolve_list_IRK_verify(irk) == true);
        }
    }

    /* RPA format: top bits must be 01, then device_addr_verify decides. */
    {
        u8 a[6] = { 1, 2, 3, 4, 5, 0x40 };
        verify_calls = 0;
        CHECK(ll_resolvable_private_addr_verify(a) == true && verify_calls == 1 && memcmp(verify_arg, a, 6) == 0);
        verify_result = 0;
        CHECK(ll_resolvable_private_addr_verify(a) == false && verify_calls == 2);
        verify_result = 1;
        a[5] = 0x7f; CHECK(ll_resolvable_private_addr_verify(a) == true);
        a[5] = 0x80; CHECK(ll_resolvable_private_addr_verify(a) == false);
        a[5] = 0xc0; CHECK(ll_resolvable_private_addr_verify(a) == false);
        a[5] = 0x00; CHECK(ll_resolvable_private_addr_verify(a) == false && verify_calls == 3);   /* only 0x40..0x7f reach the verify */
    }

    /* Resolving list. */
    ll_resolving_list_init();
    CHECK(le_param.resolving_list_head.next == &le_param.resolving_list_head);
    CHECK(le_param.resolving_list_head.prev == &le_param.resolving_list_head);
    CHECK(ll_resolving_list_match(0, (const u8 *)"abcdef") == NULL);
    asserts = 0;
    CHECK(ll_resolving_list_match(0, NULL) == NULL && asserts == 1);   /* empty list: no dereference */
    u8 d1[39], d2[39];
    make_data(d1, 1, 0x10, spec_k_le, other_irk);
    make_data(d2, 0, 0x20, other_irk, spec_k_le);
    log_tag_const_i_LL_RL = 1;
    infos = bufs = 0;
    ll_resolving_list_add(d1);
    CHECK(infos == 6 && bufs == 2);
    log_tag_const_i_LL_RL = 0;
    CHECK(mallocs == 1);
    struct resolving_list *e1 = ll_resolving_list_match(1, d1 + 1);
    CHECK(e1 != NULL && e1->entry.next == &le_param.resolving_list_head && e1->entry.prev == &le_param.resolving_list_head);
    CHECK(le_param.resolving_list_head.next == &e1->entry && le_param.resolving_list_head.prev == &e1->entry);
    CHECK(memcmp(&e1->resolving_list_param, d1, 39) == 0);
    CHECK(ll_resolving_list_match(0, d1 + 1) == NULL);       /* type must match */
    /* A second add is silently ignored: the counter is compared with 1. */
    ll_resolving_list_add(d2);
    CHECK(mallocs == 1 && ll_resolving_list_match(0, d2 + 1) == NULL);
    /* Removal needs address and type. */
    d2[0] = 0; memcpy(d2 + 1, d1 + 1, 6);
    ll_resolving_list_remove(d2);
    CHECK(frees == 0 && ll_resolving_list_match(1, d1 + 1) == e1);
    /* Resolve against the entry. */
    CHECK(ll_resolve_peer_addr(spec_rpa) == e1);              /* peer IRK is the spec key */
    {
        u8 bad[6]; memcpy(bad, spec_rpa, 6); bad[0] ^= 1;
        errors = 0;
        log_tag_const_e_LL_RL = 1;
        CHECK(ll_resolve_peer_addr(bad) == NULL && errors == 1);
        verify_result = 0;
        CHECK(ll_resolve_peer_addr(spec_rpa) == NULL && errors == 2);
        verify_result = 1;
        log_tag_const_e_LL_RL = 0;
        CHECK(ll_resolve_peer_addr(bad) == NULL && errors == 2);
        CHECK(ll_resolve_local_addr(spec_rpa) == NULL);       /* local IRK is the other one */
        log_tag_const_e_LL_RL = 1;
        CHECK(ll_resolve_local_addr(bad) == NULL && errors == 2);   /* no log on a miss */
        verify_result = 0;
        CHECK(ll_resolve_local_addr(spec_rpa) == NULL && errors == 3);
        verify_result = 1;
        log_tag_const_e_LL_RL = 0;
    }
    /* Generate RPAs: random48 = prand LE of the spec sample. */
    {
        u8 out[6];
        le_privacy.random48[0] = 0x94; le_privacy.random48[1] = 0x81; le_privacy.random48[2] = 0x70;
        ll_resolve_list_set_peer_RPA(out, e1);
        CHECK(memcmp(out, spec_rpa, 6) == 0);                 /* spec ah 0dfbaa with prand 708194 */
        CHECK(memcmp(e1->peer_PRA, spec_rpa, 6) == 0);
        CHECK(le_privacy.random48[2] == 0x70);
        u8 pra[6];
        ll_read_peer_resolvable_address(1, d1 + 1, pra);
        CHECK(memcmp(pra, spec_rpa, 6) == 0);
        /* prand's top bits are forced to 01 from the random word. */
        le_privacy.random48[2] = 0xff;
        ll_resolve_list_set_local_RPA(out, e1);
        CHECK(le_privacy.random48[2] == 0x7f && out[5] == 0x7f && out[3] == 0x94 && out[4] == 0x81);
        CHECK(memcmp(out, e1->local_PRA, 6) == 0);
        u8 hash[3];
        irk_enc(other_irk, &out[3], hash);
        CHECK(memcmp(hash, out, 3) == 0);
        CHECK(ll_resolve_local_addr(out) == e1);
        u8 lra[6];
        ll_read_local_resolvable_address(1, d1 + 1, lra);
        CHECK(memcmp(lra, out, 6) == 0);
        /* A miss leaves the output alone. */
        u8 keep[6] = { 9, 9, 9, 9, 9, 9 };
        errors = 0; log_tag_const_e_LL_RL = 1;
        ll_read_peer_resolvable_address(0, d1 + 1, keep);
        ll_read_local_resolvable_address(0, d1 + 1, keep);
        CHECK(errors == 2 && keep[0] == 9 && keep[5] == 9);
        log_tag_const_e_LL_RL = 0;
        /* A zero IRK trips the assertion but generation goes on. */
        asserts = 0;
        memset(e1->resolving_list_param.Local_IRK, 0, 16);
        ll_resolve_list_set_local_RPA(out, e1);
        CHECK(asserts == 1);
        memset(e1->resolving_list_param.Peer_IRK, 0, 16);
        ll_resolve_list_set_peer_RPA(out, e1);
        CHECK(asserts == 2);
    }
    /* Removal, then re-adding works again (counter back to 0). */
    d1[0] = 1;
    ll_resolving_list_remove(d1);
    CHECK(frees == 1 && last_freed == e1);
    CHECK(le_param.resolving_list_head.next == &le_param.resolving_list_head);
    CHECK(le_param.resolving_list_head.prev == &le_param.resolving_list_head);
    ll_resolving_list_remove(d1);                              /* nothing left */
    CHECK(frees == 1);
    ll_resolving_list_add(d2);
    CHECK(mallocs == 2);
    struct resolving_list *e2 = ll_resolving_list_match(0, d2 + 1);
    CHECK(e2 != NULL);
    /* Allocation failure: both asserts fire (the host then copies into NULL, so
     * stop here; the test cannot run the SDK's continuation). */

    /* Timer and enable. */
    {
        u8 secs[2] = { 0x2c, 0x01 };                           /* 300 s */
        le_privacy.RPA_timeout = 0; timers_removed = 0; timers_registered = 0;
        ll_set_resolvable_private_address_timeout(secs);
        CHECK(timers_removed == 0 && timers_registered == 1 && timer_msec == 300000);
        CHECK(le_privacy.RPA_timeout_cnt == 300000 && le_privacy.RPA_timeout == 77);
        secs[0] = 0xff; secs[1] = 0xff;                        /* 65535 s fits in 32 bits */
        ll_set_resolvable_private_address_timeout(secs);
        CHECK(timers_removed == 1 && removed_timer == 77 && timers_registered == 2);
        CHECK(timer_msec == 65535000 && le_privacy.RPA_timeout_cnt == 65535000);
        CHECK(timer_cb != NULL);
        /* Handler: fresh random48 from the two RNG words (low byte), then a refresh. */
        ll_resolve_list_rng[0] = 0x11223344; ll_resolve_list_rng[1] = 0x55667788;
        refreshes = 0;
        timer_cb(NULL);
        CHECK(refreshes == 1);
        for (int i = 0; i < 6; i += 2) CHECK(le_privacy.random48[i] == 0x44 && le_privacy.random48[i + 1] == 0x88);
        /* Timer registration failure asserts but keeps the zero handle. */
        asserts = 0; timer_result = 0;
        ll_set_resolvable_private_address_timeout(secs);
        CHECK(asserts == 1 && le_privacy.RPA_timeout == 0);
        timer_result = 77;
        /* Enable with an unset period uses 15 minutes. */
        le_privacy.RPA_timeout_cnt = 0; timers_registered = 0;
        ll_resolve_list_rng[0] = 0xa1; ll_resolve_list_rng[1] = 0xb2;
        ll_resolution_enable();
        CHECK(le_privacy.RPA_timeout_cnt == 900000 && timer_msec == 900000 && timers_registered == 1);
        CHECK(le_privacy.random48[0] == 0xa1 && le_privacy.random48[1] == 0xb2 && le_privacy.random48[5] == 0xb2);
        le_privacy.RPA_timeout_cnt = 12345;
        ll_resolution_enable();
        CHECK(le_privacy.RPA_timeout_cnt == 12345 && timer_msec == 12345);
        /* Disable stops the timer only with the feature bit and resolution enabled. */
        timers_removed = 0;
        config_btctler_le_features = 0; le_param.resolution_enable = 1;
        ll_resolution_disable(); CHECK(timers_removed == 0);
        config_btctler_le_features = 64; le_param.resolution_enable = 0;
        ll_resolution_disable(); CHECK(timers_removed == 0);
        le_param.resolution_enable = 1;
        ll_resolution_disable(); CHECK(timers_removed == 1 && le_privacy.RPA_timeout == 0);
        ll_resolution_disable(); CHECK(timers_removed == 1);   /* no timer left */
    }
    return TEST_DONE("btctrler_ll_resolve_list");
}
#endif
