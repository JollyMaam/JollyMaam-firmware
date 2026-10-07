/* Bluetooth encryption work queue and synchronous Simple Pairing APIs.
 * Keep the SDK's unsigned argc assertion, queue-full retry rule, and the
 * scalar-versus-pointer E22/E3 message mismatch. */
#include <stdarg.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btctrler/glue.h"
#define HCI_CODE __attribute__((section(".hci_controller_code")))

#ifdef UNITTEST
static void *test_alloc(size_t size);
static void test_release(void *ptr);
#define malloc test_alloc
#define free test_release
#endif

extern int task_create(void (*entry)(void *), void *priv, const char *name);
extern int task_kill(const char *name);
extern int os_sem_create(OS_SEM *sem, int count);
extern int os_sem_pend(OS_SEM *sem, int timeout);
extern int os_sem_set(OS_SEM *sem, u16 count);
extern int os_taskq_post_type(const char *name, int type, int argc, int *argv);
extern int os_taskq_pend(const char *name, int *msg, int len);
extern BTCTRLER_CONFIG int config_bt_function;
extern u32 g_function(u8 *U, u8 *V, u8 *X, u8 *Y);
extern int f1_function(u8 *U, u8 *V, u8 *X, u8 *Z, u8 *re);
extern int f2_function(u8 *W, u8 *N1, u8 *N2, u8 *A1, u8 *A2, u8 *re);
extern int f3_function(u8 *W, u8 *N1, u8 *N2, u8 *R, u8 *IOcap, u8 *A1, u8 *A2, u8 *re);
extern int uECC_compute_public_key(const uint8_t *private_key, uint8_t *public_key,
                                   uECC_Curve curve);
extern int uECC_shared_secret(const uint8_t *public_key, const uint8_t *private_key,
                              uint8_t *secret, uECC_Curve curve);

/* The SDK uses byte offsets when packing these request records. */
_Static_assert(offsetof(struct e1_para, rand) == 6, "e1 rand offset");
_Static_assert(offsetof(struct e1_para, k) == 22, "e1 key offset");
_Static_assert(offsetof(struct e1_para, re) == 38, "e1 result offset");
_Static_assert(offsetof(struct e21_para, re) == 22, "e21 result offset");
_Static_assert(offsetof(struct e22_para, rand) == 16, "e22 rand offset");
_Static_assert(offsetof(struct e22_para, len) == 32, "e22 length offset");
_Static_assert(offsetof(struct e22_para, addr) == 33, "e22 address offset");
_Static_assert(offsetof(struct e22_para, re) == 39, "e22 result offset");
_Static_assert(offsetof(struct e3_para, rand) == 16, "e3 rand offset");
_Static_assert(offsetof(struct e3_para, cof) == 32, "e3 cof offset");
_Static_assert(offsetof(struct e3_para, len) == 44, "e3 length offset");
_Static_assert(offsetof(struct e3_para, re) == 45, "e3 result offset");
_Static_assert(offsetof(struct g_para, v) == 48, "g v offset");
_Static_assert(offsetof(struct g_para, x) == 96, "g x offset");
_Static_assert(offsetof(struct g_para, y) == 112, "g y offset");
_Static_assert(offsetof(struct f1_para, v) == 48, "f1 v offset");
_Static_assert(offsetof(struct f1_para, x) == 96, "f1 x offset");
_Static_assert(offsetof(struct f1_para, z) == 112, "f1 z offset");
_Static_assert(offsetof(struct f1_para, re) == 113, "f1 result offset");
_Static_assert(offsetof(struct f2_para, n1) == 24, "f2 n1 offset");
_Static_assert(offsetof(struct f2_para, n2) == 40, "f2 n2 offset");
_Static_assert(offsetof(struct f2_para, a1) == 56, "f2 a1 offset");
_Static_assert(offsetof(struct f2_para, a2) == 62, "f2 a2 offset");
_Static_assert(offsetof(struct f2_para, re) == 68, "f2 result offset");
_Static_assert(offsetof(struct f3_para, n1) == 24, "f3 n1 offset");
_Static_assert(offsetof(struct f3_para, n2) == 40, "f3 n2 offset");
_Static_assert(offsetof(struct f3_para, r) == 56, "f3 r offset");
_Static_assert(offsetof(struct f3_para, iocap) == 72, "f3 iocap offset");
_Static_assert(offsetof(struct f3_para, a1) == 75, "f3 a1 offset");
_Static_assert(offsetof(struct f3_para, a2) == 81, "f3 a2 offset");
_Static_assert(offsetof(struct f3_para, re) == 87, "f3 result offset");
_Static_assert(offsetof(struct ecc_cpk_para, public_key) == 24, "ecc public key offset");
_Static_assert(offsetof(struct ecc_ss_para, private_key) == 48, "ecc private key offset");
_Static_assert(offsetof(struct ecc_ss_para, secret) == 72, "ecc secret offset");
_Static_assert(offsetof(struct ecc_ss_para, re) == 96, "ecc result offset");

__attribute__((section(".hci_controller_bss"))) OS_SEM *btencry_sem;
__attribute__((section(".hci_controller_bss"))) struct e1_para *e1;
__attribute__((section(".hci_controller_bss"))) struct e21_para *e21;
__attribute__((section(".hci_controller_bss"))) struct e22_para *e22;
__attribute__((section(".hci_controller_bss"))) struct e3_para *e3;
__attribute__((section(".hci_controller_bss"))) struct g_para *g;
__attribute__((section(".hci_controller_bss"))) struct f1_para *f1;
__attribute__((section(".hci_controller_bss"))) struct f2_para *f2;
__attribute__((section(".hci_controller_bss"))) struct f3_para *f3;
__attribute__((section(".hci_controller_bss"))) struct ecc_cpk_para *ecc_cpk;
__attribute__((section(".hci_controller_bss"))) struct ecc_ss_para *ecc_ss;

static HCI_CODE void btencry_task(void)
{
    int msg[16];
    for (;;) {
        if (os_taskq_pend(NULL, msg, 16) != 13) continue;
        switch (msg[0]) {
        case 0: function_E1((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                            (u8 *)(uintptr_t)msg[3], (u8 *)(uintptr_t)msg[4]); break;
        case 1: function_E21((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                             (u8 *)(uintptr_t)msg[3]); break;
        case 2: function_E22((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                             *(u8 *)(uintptr_t)msg[3], (u8 *)(uintptr_t)msg[4],
                             (u8 *)(uintptr_t)msg[5]); break;
        case 3: function_E3((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                            (u8 *)(uintptr_t)msg[3], *(u8 *)(uintptr_t)msg[4],
                            (u8 *)(uintptr_t)msg[5]); break;
        case 4: *(u32 *)(uintptr_t)msg[5] = g_function((u8 *)(uintptr_t)msg[1],
                        (u8 *)(uintptr_t)msg[2], (u8 *)(uintptr_t)msg[3],
                        (u8 *)(uintptr_t)msg[4]); break;
        case 5: f1_function((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                            (u8 *)(uintptr_t)msg[3], (u8 *)(uintptr_t)msg[4],
                            (u8 *)(uintptr_t)msg[5]); break;
        case 6: f2_function((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                            (u8 *)(uintptr_t)msg[3], (u8 *)(uintptr_t)msg[4],
                            (u8 *)(uintptr_t)msg[5], (u8 *)(uintptr_t)msg[6]); break;
        case 7: f3_function((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                            (u8 *)(uintptr_t)msg[3], (u8 *)(uintptr_t)msg[4],
                            (u8 *)(uintptr_t)msg[5], (u8 *)(uintptr_t)msg[6],
                            (u8 *)(uintptr_t)msg[7], (u8 *)(uintptr_t)msg[8]); break;
        case 8: uECC_compute_public_key((u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                                         (uECC_Curve)(uintptr_t)msg[3]); break;
        case 9: *(u8 *)(uintptr_t)msg[5] = (u8)uECC_shared_secret(
                    (u8 *)(uintptr_t)msg[1], (u8 *)(uintptr_t)msg[2],
                    (u8 *)(uintptr_t)msg[3], (uECC_Curve)(uintptr_t)msg[4]); break;
        default: break;
        }
        os_sem_set(btencry_sem, 0);
        os_sem_post(btencry_sem);
    }
}

HCI_CODE void btencry_task_init(void)
{
    if (!btencry_sem) btencry_sem = malloc(80);
    if (!btencry_sem) btctrler_assert_failed("btencry_sem", 0);
    os_sem_create(btencry_sem, 0);
    task_create((void (*)(void *))btencry_task, NULL, "btencry");
}

HCI_CODE int btencry_task_exit(void)
{
    task_kill("btencry");
    return 0;
}

HCI_CODE int btencry_msg_to_task(int cmd, int argc, ...)
{
    int argv[16];
    va_list ap;
    btencry_task_init();
    if ((u32)argc >= 16) btctrler_assert_failed("argc < ARRAY_SIZE(argv)", argc);
    va_start(ap, argc);
    for (int i = 0; i < argc; ++i) argv[i] = va_arg(ap, int);
    va_end(ap);

    int result;
    do {
        result = os_taskq_post_type("btencry", cmd, argc, argv);
        if (result != 21 || cpu_in_irq() || cpu_irq_disabled()) break;
        os_time_dly(2);
    } while (1);
    os_sem_pend(btencry_sem, 0);
    btencry_task_exit();
    return result;
}

HCI_CODE void btencry_function_E1(u8 *address, u8 *RAND, u8 *K, u8 *result)
{ btencry_msg_to_task(0, 4, (int)(uintptr_t)address, (int)(uintptr_t)RAND,
                      (int)(uintptr_t)K, (int)(uintptr_t)result); }
HCI_CODE void btencry_function_E21(u8 *address, u8 *RAND, u8 *result)
{ btencry_msg_to_task(1, 3, (int)(uintptr_t)address, (int)(uintptr_t)RAND,
                      (int)(uintptr_t)result); }
HCI_CODE void btencry_function_E22(u8 *pin, u8 *RAND, u8 L, u8 *address, u8 *result)
{ btencry_msg_to_task(2, 5, (int)(uintptr_t)pin, (int)(uintptr_t)RAND, L,
                      (int)(uintptr_t)address, (int)(uintptr_t)result); }
HCI_CODE void btencry_function_E3(u8 *K, u8 *RAND, u8 *COF, u8 Len, u8 *result)
{ btencry_msg_to_task(3, 5, (int)(uintptr_t)K, (int)(uintptr_t)RAND,
                      (int)(uintptr_t)COF, Len, (int)(uintptr_t)result); }
HCI_CODE u32 btencry_g_function(u8 *U, u8 *V, u8 *X, u8 *Y)
{
    u32 result;
    btencry_msg_to_task(4, 5, (int)(uintptr_t)U, (int)(uintptr_t)V,
                        (int)(uintptr_t)X, (int)(uintptr_t)Y, (int)(uintptr_t)&result);
    return result;
}
HCI_CODE void btencry_f1_function(u8 *U, u8 *V, u8 *X, u8 *Z, u8 *re)
{ btencry_msg_to_task(5, 5, (int)(uintptr_t)U, (int)(uintptr_t)V, (int)(uintptr_t)X,
                      (int)(uintptr_t)Z, (int)(uintptr_t)re); }
HCI_CODE void btencry_f2_function(u8 *W, u8 *N1, u8 *N2, u8 *A1, u8 *A2, u8 *re)
{ btencry_msg_to_task(6, 6, (int)(uintptr_t)W, (int)(uintptr_t)N1, (int)(uintptr_t)N2,
                      (int)(uintptr_t)A1, (int)(uintptr_t)A2, (int)(uintptr_t)re); }
HCI_CODE void btencry_f3_function(u8 *W, u8 *N1, u8 *N2, u8 *R, u8 *IOcap,
                         u8 *A1, u8 *A2, u8 *re)
{ btencry_msg_to_task(7, 8, (int)(uintptr_t)W, (int)(uintptr_t)N1,
                      (int)(uintptr_t)N2, (int)(uintptr_t)R, (int)(uintptr_t)IOcap,
                      (int)(uintptr_t)A1, (int)(uintptr_t)A2, (int)(uintptr_t)re); }
HCI_CODE int btencry_uECC_compute_public_key(const uint8_t *private_key, uint8_t *public_key,
                                    uECC_Curve curve)
{ btencry_msg_to_task(8, 3, (int)(uintptr_t)private_key,
                      (int)(uintptr_t)public_key, (int)(uintptr_t)curve); return 0; }
HCI_CODE int btencry_uECC_shared_secret(const uint8_t *public_key, const uint8_t *private_key,
                               uint8_t *secret, uECC_Curve curve, u8 *result)
{ btencry_msg_to_task(9, 5, (int)(uintptr_t)public_key, (int)(uintptr_t)private_key,
                      (int)(uintptr_t)secret, (int)(uintptr_t)curve,
                      (int)(uintptr_t)result); return 0; }

HCI_CODE int uECC_shared_secret_result(const uint8_t *public_key, const uint8_t *private_key,
                              uint8_t *secret, uECC_Curve curve, u8 *result)
{
    int value = uECC_shared_secret(public_key, private_key, secret, curve);
    *result = (u8)value;
    return value & 255;
}

HCI_CODE void function_E1_api(u8 *address, u8 *RAND, u8 *K, u8 *result)
{
    if (!(config_bt_function & 1)) { function_E1(address, RAND, K, result); return; }
    e1 = zalloc(54);
    memcpy(e1, address, 6); memcpy(e1->rand, RAND, 16); memcpy(e1->k, K, 16);
    btencry_msg_to_task(0, 4, (int)(uintptr_t)e1, (int)(uintptr_t)e1->rand,
                        (int)(uintptr_t)e1->k, (int)(uintptr_t)e1->re);
    memcpy(result, e1->re, 16); free(e1->addr);
}
HCI_CODE void function_E21_api(u8 *address, u8 *RAND, u8 *result)
{
    if (!(config_bt_function & 1)) { function_E21(address, RAND, result); return; }
    e21 = zalloc(38);
    memcpy(e21, address, 6); memcpy(e21->rand, RAND, 16);
    btencry_msg_to_task(1, 3, (int)(uintptr_t)e21, (int)(uintptr_t)e21->rand,
                        (int)(uintptr_t)e21->re);
    memcpy(result, e21->re, 16); free(e21->addr);
}
HCI_CODE void function_E22_api(u8 *pin, u8 *RAND, u8 L, u8 *address, u8 *result)
{
    if (!(config_bt_function & 1)) { function_E22(pin, RAND, L, address, result); return; }
    e22 = zalloc(55);
    memcpy(e22->pin, pin, L); memcpy(e22->rand, RAND, 16); e22->len = L;
    memcpy(e22->addr, address, 6);
    btencry_msg_to_task(2, 5, (int)(uintptr_t)e22, (int)(uintptr_t)e22->rand,
                        (int)(uintptr_t)&e22->len, (int)(uintptr_t)e22->addr,
                        (int)(uintptr_t)e22->re);
    memcpy(result, e22->re, 16); free(e22->pin);
}
HCI_CODE void function_E3_api(u8 *K, u8 *RAND, u8 *COF, u8 Len, u8 *result)
{
    if (!(config_bt_function & 1)) { function_E3(K, RAND, COF, Len, result); return; }
    e3 = zalloc(61);
    memcpy(e3->k, K, 16); memcpy(e3->rand, RAND, 16); memcpy(e3->cof, COF, 12);
    e3->len = Len;
    btencry_msg_to_task(3, 5, (int)(uintptr_t)e3, (int)(uintptr_t)e3->rand,
                        (int)(uintptr_t)e3->cof, (int)(uintptr_t)&e3->len,
                        (int)(uintptr_t)e3->re);
    memcpy(result, e3->re, 16); free(e3->k);
}
HCI_CODE u32 g_function_api(u8 *U, u8 *V, u8 *X, u8 *Y)
{
    u32 result = 0;
    if (!(config_bt_function & 1)) return g_function(U, V, X, Y);
    g = zalloc(128);
    memcpy(g->u, U, 24); memcpy((u8 *)g + 48, V, 24);
    memcpy((u8 *)g + 96, X, 16); memcpy((u8 *)g + 112, Y, 16);
    btencry_msg_to_task(4, 5, (int)(uintptr_t)g, (int)(uintptr_t)((u8 *)g + 48),
                        (int)(uintptr_t)((u8 *)g + 96), (int)(uintptr_t)((u8 *)g + 112),
                        (int)(uintptr_t)&result);
    free(g);
    return result;
}
HCI_CODE void f1_function_api(u8 *U, u8 *V, u8 *X, u8 *Z, u8 *re)
{
    if (!(config_bt_function & 1)) { f1_function(U, V, X, Z, re); return; }
    f1 = zalloc(129);
    memcpy(f1->u, U, 24); memcpy((u8 *)f1 + 48, V, 24); memcpy((u8 *)f1 + 96, X, 16);
    f1->z[0] = *Z;
    btencry_msg_to_task(5, 5, (int)(uintptr_t)f1, (int)(uintptr_t)((u8 *)f1 + 48),
                        (int)(uintptr_t)((u8 *)f1 + 96), (int)(uintptr_t)((u8 *)f1 + 112),
                        (int)(uintptr_t)((u8 *)f1 + 113));
    memcpy(re, f1->re, 16); free(f1->u);
}
HCI_CODE void f2_function_api(u8 *W, u8 *N1, u8 *N2, u8 *A1, u8 *A2, u8 *re)
{
    if (!(config_bt_function & 1)) { f2_function(W, N1, N2, A1, A2, re); return; }
    f2 = zalloc(84);
    memcpy(f2->w, W, 24); memcpy(f2->n1, N1, 16); memcpy(f2->n2, N2, 16);
    memcpy(f2->a1, A1, 6); memcpy((u8 *)f2 + 62, A2, 6);
    btencry_msg_to_task(6, 6, (int)(uintptr_t)f2, (int)(uintptr_t)((u8 *)f2 + 24),
                        (int)(uintptr_t)((u8 *)f2 + 40), (int)(uintptr_t)((u8 *)f2 + 56),
                        (int)(uintptr_t)((u8 *)f2 + 62), (int)(uintptr_t)((u8 *)f2 + 68));
    memcpy(re, f2->re, 16); free(f2->w);
}
HCI_CODE void f3_function_api(u8 *W, u8 *N1, u8 *N2, u8 *R, u8 *IOcap,
                     u8 *A1, u8 *A2, u8 *re)
{
    if (!(config_bt_function & 1)) { f3_function(W, N1, N2, R, IOcap, A1, A2, re); return; }
    f3 = zalloc(103);
    memcpy(f3->w, W, 24); memcpy(f3->n1, N1, 16); memcpy(f3->n2, N2, 16);
    memcpy(f3->r, R, 16); memcpy(f3->iocap, IOcap, 3);
    memcpy(f3->a1, A1, 6); memcpy(f3->a2, A2, 6);
    btencry_msg_to_task(7, 8, (int)(uintptr_t)f3, (int)(uintptr_t)((u8 *)f3 + 24),
                        (int)(uintptr_t)((u8 *)f3 + 40), (int)(uintptr_t)((u8 *)f3 + 56),
                        (int)(uintptr_t)((u8 *)f3 + 72), (int)(uintptr_t)((u8 *)f3 + 75),
                        (int)(uintptr_t)((u8 *)f3 + 81), (int)(uintptr_t)((u8 *)f3 + 87));
    memcpy(re, f3->re, 16); free(f3->w);
}
HCI_CODE int uECC_compute_public_key_api(const uint8_t *private_key, uint8_t *public_key,
                                uECC_Curve curve)
{
    if (config_bt_function & 1) {
        ecc_cpk = zalloc(72); memcpy(ecc_cpk->private_key, private_key, 24);
        btencry_msg_to_task(8, 3, (int)(uintptr_t)ecc_cpk,
                            (int)(uintptr_t)ecc_cpk->public_key, (int)(uintptr_t)curve);
        memcpy(public_key, ecc_cpk->public_key, 48); free(ecc_cpk->private_key);
    } else {
        uECC_compute_public_key(private_key, public_key, curve);
    }
    return 0;
}
HCI_CODE int uECC_shared_secret_api(const uint8_t *public_key, const uint8_t *private_key,
                           uint8_t *secret, uECC_Curve curve, u8 *result)
{
    if (config_bt_function & 1) {
        ecc_ss = zalloc(97); memcpy(ecc_ss->public_key, public_key, 48);
        memcpy(ecc_ss->private_key, private_key, 24);
        btencry_msg_to_task(9, 5, (int)(uintptr_t)ecc_ss,
                            (int)(uintptr_t)ecc_ss->private_key,
                            (int)(uintptr_t)ecc_ss->secret, (int)(uintptr_t)curve,
                            (int)(uintptr_t)&ecc_ss->re);
        memcpy(secret, ecc_ss->secret, 24); *result = ecc_ss->re; free(ecc_ss);
    } else {
        uECC_shared_secret_result(public_key, private_key, secret, curve, result);
    }
    return 0;
}

#ifdef UNITTEST
#include "test.h"

int config_bt_function;
int config_asser;
static int sem_creates, task_creates, task_kills, posts, pendings, delays;
static int irq, disabled, asserts, fail_post, post_results[4], post_result_count;
static int last_cmd, last_argc, last_argv[16], fail_alloc;
static void (*task_entry)(void *);
static jmp_buf worker_escape;
static int worker_mode, worker_pend_count, worker_msg[16], worker_results[3];
static int sem_sets, sem_posts, sem_order[4], sem_order_count;
static int dispatched, api_fill, direct_called, released;
static void *last_freed;
static u8 *dispatch_args[8];
static int dispatch_arg_count;
static u8 api_arena[2048];
static u8 api_inputs[256];
static size_t api_offset;
static u8 dispatch_bytes[3] = { 0xa7, 0x5c, 0x31 };
static u32 dispatch_word;
static u8 *dispatch_result;

void *test_alloc(size_t size)
{ if (fail_alloc) return NULL; CHECK(size == 80); return calloc(1, size); }
void test_release(void *ptr) { last_freed = ptr; ++released; }
void *zalloc(u32 size)
{
    size_t start = (api_offset + 3u) & ~3u;
    CHECK(start + size <= sizeof(api_arena));
    api_offset = start + size;
    memset(api_arena + start, 0, size);
    return api_arena + start;
}
int os_sem_create(OS_SEM *sem, int count)
{ (void)sem; CHECK(count == 0); ++sem_creates; return 0; }
int os_sem_pend(OS_SEM *sem, int timeout)
{ CHECK(sem == btencry_sem && timeout == 0); ++pendings; return 0; }
int os_sem_set(OS_SEM *sem, u16 count)
{ (void)sem; CHECK(count == 0); ++sem_sets; sem_order[sem_order_count++] = 1; return 0; }
int os_sem_post(OS_SEM *sem)
{ (void)sem; ++sem_posts; sem_order[sem_order_count++] = 2; return 0; }
int task_create(void (*entry)(void *), void *priv, const char *name)
{ CHECK(priv == NULL && !strcmp(name, "btencry")); ++task_creates; task_entry = entry; return 0; }
int task_kill(const char *name) { CHECK(!strcmp(name, "btencry")); ++task_kills; return 0; }
int os_taskq_pend(const char *name, int *msg, int len)
{
    (void)name; CHECK(len == 16);
    if (!worker_mode) return 0;
    if (worker_pend_count == 0) memcpy(msg, worker_msg, sizeof(worker_msg));
    if (worker_pend_count > 1) longjmp(worker_escape, 1);
    return worker_results[worker_pend_count++];
}
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    CHECK(!strcmp(name, "btencry")); ++posts; last_cmd = type; last_argc = argc;
    for (int i = 0; i < argc && i < 16; ++i) last_argv[i] = argv[i];
    if (api_fill) {
        if (type == 0) memset((void *)(uintptr_t)argv[3], 0xa1, 16);
        if (type == 2) memset((void *)(uintptr_t)argv[4], 0xa2, 16);
        if (type == 7) memset((void *)(uintptr_t)argv[7], 0xa3, 16);
        if (type == 8) memset((void *)(uintptr_t)argv[1], 0xa4, 48);
        if (type == 9) {
            memset((void *)(uintptr_t)argv[2], 0xa5, 24);
            *(u8 *)(uintptr_t)argv[4] = 0x5a;
        }
    }
    if (post_result_count) return post_results[--post_result_count];
    return fail_post;
}
void os_time_dly(int ticks) { CHECK(ticks == 2); ++delays; }
void btctrler_assert_failed(const char *what, int value)
{ CHECK(what); (void)value; ++asserts; }
void function_E1(u8 *a, u8 *r, u8 *k, u8 *out)
{ if (!worker_mode) ++direct_called; dispatched = 0; dispatch_result = out; dispatch_args[0] = a; dispatch_args[1] = r; dispatch_args[2] = k; dispatch_args[3] = out; dispatch_arg_count = 4; }
void function_E21(u8 *a, u8 *r, u8 *out)
{ if (!worker_mode) ++direct_called; dispatched = 1; dispatch_args[0] = a; dispatch_args[1] = r; dispatch_args[2] = out; dispatch_arg_count = 3; }
void function_E22(u8 *pin, u8 *r, u8 len, u8 *a, u8 *out)
{ if (!worker_mode) ++direct_called; dispatched = 2; dispatch_word = len; dispatch_args[0] = pin; dispatch_args[1] = r; dispatch_args[2] = a; dispatch_args[3] = out; dispatch_arg_count = 4; }
void function_E3(u8 *k, u8 *r, u8 *cof, u8 len, u8 *out)
{ if (!worker_mode) ++direct_called; dispatched = 3; dispatch_word = len; dispatch_args[0] = k; dispatch_args[1] = r; dispatch_args[2] = cof; dispatch_args[3] = out; dispatch_arg_count = 4; }
u32 g_function(u8 *u, u8 *v, u8 *x, u8 *y)
{ dispatched = 4; dispatch_args[0] = u; dispatch_args[1] = v; dispatch_args[2] = x; dispatch_args[3] = y; dispatch_arg_count = 4; return 0x89abcdef; }
int f1_function(u8 *u, u8 *v, u8 *x, u8 *z, u8 *out)
{ dispatched = 5; dispatch_args[0] = u; dispatch_args[1] = v; dispatch_args[2] = x; dispatch_args[3] = z; dispatch_args[4] = out; dispatch_arg_count = 5; return -5; }
int f2_function(u8 *w, u8 *n1, u8 *n2, u8 *a1, u8 *a2, u8 *out)
{ dispatched = 6; dispatch_args[0] = w; dispatch_args[1] = n1; dispatch_args[2] = n2; dispatch_args[3] = a1; dispatch_args[4] = a2; dispatch_args[5] = out; dispatch_arg_count = 6; return -6; }
int f3_function(u8 *w, u8 *n1, u8 *n2, u8 *r, u8 *io, u8 *a1, u8 *a2, u8 *out)
{ dispatched = 7; dispatch_args[0] = w; dispatch_args[1] = n1; dispatch_args[2] = n2; dispatch_args[3] = r; dispatch_args[4] = io; dispatch_args[5] = a1; dispatch_args[6] = a2; dispatch_args[7] = out; dispatch_arg_count = 8; return -7; }
int uECC_compute_public_key(const uint8_t *priv, uint8_t *pub, uECC_Curve curve)
{ dispatched = 8; dispatch_args[0] = (u8 *)priv; dispatch_args[1] = pub; dispatch_args[2] = (u8 *)curve; dispatch_arg_count = 3; return 0; }
int uECC_shared_secret(const uint8_t *pub, const uint8_t *priv, uint8_t *secret, uECC_Curve curve)
{ dispatched = 9; dispatch_args[0] = (u8 *)pub; dispatch_args[1] = (u8 *)priv; dispatch_args[2] = secret; dispatch_args[3] = (u8 *)curve; dispatch_arg_count = 4; memset(secret, 0x36, 24); return 0x12345678; }
int cpu_in_irq(void) { return irq; }
int cpu_irq_disabled(void) { return disabled; }

static void reset_test(void)
{
    free(btencry_sem); btencry_sem = NULL;
    sem_creates = task_creates = task_kills = posts = pendings = delays = 0;
    irq = disabled = asserts = fail_post = post_result_count = fail_alloc = 0;
    memset(last_argv, 0, sizeof(last_argv)); task_entry = NULL;
    worker_mode = worker_pend_count = sem_sets = sem_posts = sem_order_count = 0;
    dispatched = api_fill = direct_called = released = 0; last_freed = NULL;
    dispatch_arg_count = 0; memset(dispatch_args, 0, sizeof(dispatch_args));
    api_offset = 0; memset(api_arena, 0, sizeof(api_arena));
    memset(api_inputs, 0x6d, sizeof(api_inputs));
    memset(worker_msg, 0, sizeof(worker_msg)); memset(worker_results, 0, sizeof(worker_results));
    worker_results[0] = 13;
}

static void run_worker_message(int command)
{
    btencry_task_init();
    memset(worker_msg, 0, sizeof(worker_msg));
    worker_msg[0] = command;
    for (int i = 1; i < 9; ++i) worker_msg[i] = (int)(uintptr_t)&api_arena[i * 16];
    if (command == 2) worker_msg[3] = (int)(uintptr_t)&dispatch_bytes[0];
    if (command == 3) worker_msg[4] = (int)(uintptr_t)&dispatch_bytes[1];
    if (command == 4) worker_msg[5] = (int)(uintptr_t)&dispatch_word;
    if (command == 9) worker_msg[5] = (int)(uintptr_t)&dispatch_bytes[2];
    worker_pend_count = 0;
    worker_mode = 1;
    if (setjmp(worker_escape) == 0) task_entry(NULL);
    worker_mode = 0;
    CHECK(sem_sets == 1 && sem_posts == 1 && sem_order_count == 2);
    CHECK(sem_order[0] == 1 && sem_order[1] == 2);
}

int main(void)
{
    reset_test();
    btencry_task_init();
    CHECK(btencry_sem != NULL && sem_creates == 1 && task_creates == 1 && task_entry != NULL);
    btencry_task_init();
    CHECK(sem_creates == 2 && task_creates == 2); /* init repeats OS setup */

    reset_test();
    fail_post = 0;
    CHECK(btencry_msg_to_task(7, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14) == 0);
    CHECK(last_cmd == 7 && last_argc == 15 && last_argv[14] == 14);
    CHECK(posts == 1 && pendings == 1 && task_kills == 1 && delays == 0);

    reset_test();
    post_results[0] = 0; post_results[1] = 21; post_result_count = 2;
    CHECK(btencry_msg_to_task(3, 0) == 0);
    CHECK(posts == 2 && delays == 1 && pendings == 1 && task_kills == 1);

    reset_test();
    irq = 1; fail_post = 21;
    CHECK(btencry_msg_to_task(1, 0) == 21);
    CHECK(posts == 1 && delays == 0 && pendings == 1 && task_kills == 1);

    reset_test();
    fail_post = -7;
    CHECK(btencry_msg_to_task(2, 0) == -7);
    CHECK(posts == 1 && pendings == 1 && task_kills == 1);

    reset_test();
    fail_post = 0;
    CHECK(btencry_msg_to_task(9, 16, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15) == 0);
    CHECK(asserts == 1 && last_argc == 16 && last_argv[15] == 15);

    reset_test();
    disabled = 1; fail_post = 21;
    CHECK(btencry_msg_to_task(1, 0) == 21);
    CHECK(posts == 1 && delays == 0 && pendings == 1 && task_kills == 1);

    reset_test();
    fail_post = 0;
    CHECK(btencry_msg_to_task(1, -1) == 0);
    CHECK(asserts == 1 && last_argc == -1 && posts == 1 && pendings == 1);

    reset_test();
    run_worker_message(0);
    CHECK(dispatched == 0 && dispatch_result == (u8 *)(uintptr_t)worker_msg[4]);
    CHECK(dispatch_arg_count == 4 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
    CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
    CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[3]);
    CHECK(dispatch_args[3] == (u8 *)(uintptr_t)worker_msg[4]);
    for (int command = 1; command <= 9; ++command) {
        reset_test(); run_worker_message(command);
        CHECK(dispatched == command);
        if (command == 1) {
            CHECK(dispatch_arg_count == 3 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
            CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
            CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[3]);
        } else if (command == 2) {
            CHECK(dispatch_arg_count == 4 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
            CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
            CHECK(dispatch_word == dispatch_bytes[0]);
            CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[4]);
            CHECK(dispatch_args[3] == (u8 *)(uintptr_t)worker_msg[5]);
        } else if (command == 3) {
            CHECK(dispatch_arg_count == 4 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
            CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
            CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[3]);
            CHECK(dispatch_word == dispatch_bytes[1]);
            CHECK(dispatch_args[3] == (u8 *)(uintptr_t)worker_msg[5]);
        } else if (command == 4) {
            CHECK(dispatch_arg_count == 4 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
            CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
            CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[3]);
            CHECK(dispatch_args[3] == (u8 *)(uintptr_t)worker_msg[4]);
            CHECK(dispatch_word == 0x89abcdef);
        } else if (command >= 5 && command <= 7) {
            int count = command == 5 ? 5 : command == 6 ? 6 : 8;
            CHECK(dispatch_arg_count == count);
            for (int i = 0; i < count; ++i)
                CHECK(dispatch_args[i] == (u8 *)(uintptr_t)worker_msg[i + 1]);
        } else if (command == 8) {
            CHECK(dispatch_arg_count == 3 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
            CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
            CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[3]);
        } else if (command == 9) {
            CHECK(dispatch_arg_count == 4 && dispatch_args[0] == (u8 *)(uintptr_t)worker_msg[1]);
            CHECK(dispatch_args[1] == (u8 *)(uintptr_t)worker_msg[2]);
            CHECK(dispatch_args[2] == (u8 *)(uintptr_t)worker_msg[3]);
            CHECK(dispatch_args[3] == (u8 *)(uintptr_t)worker_msg[4]);
            CHECK(dispatch_bytes[2] == 0x78);
        }
    }
    reset_test();
    run_worker_message(2);
    dispatch_bytes[0] = 0xa7;
    /* The E22 length comes from the pointed byte, despite the direct wrapper
     * passing the scalar value in this same message slot. */
    CHECK(dispatch_word == 0xa7);

    reset_test();
    run_worker_message(3);
    dispatch_bytes[1] = 0x5c;
    CHECK(dispatch_word == 0x5c);

    reset_test();
    run_worker_message(4);
    CHECK(dispatch_word == 0x89abcdef);

    reset_test();
    run_worker_message(9);
    CHECK(dispatch_bytes[2] == 0x78);

    reset_test();
    run_worker_message(99);
    CHECK(dispatched == 0 && sem_sets == 1 && sem_posts == 1);

    reset_test();
    worker_results[0] = 12; worker_results[1] = 13;
    run_worker_message(99);
    CHECK(worker_pend_count == 2);

    reset_test();
    config_bt_function = 0;
    function_E1_api(api_inputs, api_inputs + 16, api_inputs + 32, api_inputs + 48);
    CHECK(direct_called == 1 && posts == 0);

    reset_test();
    config_bt_function = 1; api_fill = 1;
    function_E1_api(api_inputs, api_inputs + 16, api_inputs + 32, api_inputs + 48);
    CHECK(!memcmp(api_inputs + 48, (u8[16]){[0 ... 15] = 0xa1}, 16));
    CHECK(released == 1 && last_freed == e1->addr && last_cmd == 0 && last_argc == 4);

    reset_test();
    config_bt_function = 1; api_fill = 1;
    function_E22_api(api_inputs, api_inputs + 16, 4, api_inputs + 32, api_inputs + 48);
    CHECK(e22->pin[0] == api_inputs[0] && e22->pin[3] == api_inputs[3]);
    CHECK(e22->pin[4] == 0 && e22->len == 4 && e22->addr[0] == api_inputs[32]);
    CHECK(!memcmp(api_inputs + 48, (u8[16]){[0 ... 15] = 0xa2}, 16));
    CHECK(released == 1 && last_freed == e22->pin);

    reset_test();
    config_bt_function = 1; api_fill = 1;
    f3_function_api(api_inputs, api_inputs + 24, api_inputs + 40, api_inputs + 56,
                    api_inputs + 72, api_inputs + 75, api_inputs + 81, api_inputs + 103);
    CHECK(!memcmp(api_inputs + 103, (u8[16]){[0 ... 15] = 0xa3}, 16));
    CHECK(released == 1 && last_freed == f3->w);

    reset_test();
    config_bt_function = 1;
    for (int i = 0; i < 48; ++i) { api_inputs[i] = (u8)(i + 1); api_inputs[48 + i] = (u8)(0x80 + i); }
    f1_function_api(api_inputs, api_inputs + 48, api_inputs + 96, api_inputs + 112, api_inputs + 128);
    CHECK(!memcmp(f1->u, api_inputs, 24) && !memcmp(f1->v, api_inputs + 48, 24));
    CHECK(!memcmp(f1->u + 24, (u8[24]){0}, 24));
    CHECK(!memcmp(f1->v + 24, (u8[24]){0}, 24));
    CHECK(f1->x[0] == api_inputs[96] && f1->z[0] == api_inputs[112]);
    CHECK(released == 1 && last_freed == f1->u);

    reset_test();
    config_bt_function = 1; api_fill = 1;
    uECC_compute_public_key_api(api_inputs, api_inputs + 64, NULL);
    CHECK(!memcmp(api_inputs + 64, (u8[48]){[0 ... 47] = 0xa4}, 48));
    CHECK(released == 1 && last_freed == ecc_cpk->private_key);

    reset_test();
    config_bt_function = 1; api_fill = 1;
    uECC_shared_secret_api(api_inputs, api_inputs + 48, api_inputs + 80, NULL, api_inputs + 104);
    CHECK(!memcmp(api_inputs + 80, (u8[24]){[0 ... 23] = 0xa5}, 24));
    CHECK(api_inputs[104] == 0x5a);
    CHECK(released == 1 && last_freed == ecc_ss);

    reset_test();
    config_asser = 0; fail_alloc = 1;
    btencry_task_init();
    CHECK(asserts == 1 && sem_creates == 1 && task_creates == 1);
    config_asser = 0; fail_alloc = 0;
    return TEST_DONE("btctrler_btencry_task");
}
#endif
