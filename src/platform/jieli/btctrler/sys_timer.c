/* Controller timer adapter. BR23 and BD19 have identical member IR here:
 * registration uses sys_timer_add and widens its u16 result to int; all
 * incoming int handles truncate to u16, while clock/period values stay u32. */
#include "btctrler/glue.h"

/* Runtime timer API absent from the recovered controller prototypes. */
u32 sys_timer_get_ms(void);
void sys_timer_re_run(u16 id);
void sys_timer_set_user_data(u16 id, void *context);
void *sys_timer_get_user_data(u16 id);

u32 sys_timer_get_time(void)
{
    return sys_timer_get_ms();
}

sys_timer sys_timer_register(u32 msec, void (*callback)(void *))
{
    return sys_timer_add(NULL, callback, msec);
}

sys_timer sys_timer_register_periodic(u32 msec, void (*callback)(void *))
{
    return sys_timer_add(NULL, callback, msec);
}

void sys_timer_remove(sys_timer timer)
{
    sys_timeout_del((u16)timer);
}

void sys_timer_reset(sys_timer timer)
{
    sys_timer_re_run((u16)timer);
}

void sys_timer_change_period(sys_timer timer, u32 msec)
{
    sys_timer_modify((u16)timer, msec);
}

void sys_timer_set_context(sys_timer timer, void *context)
{
    sys_timer_set_user_data((u16)timer, context);
}

void *sys_timer_get_context(sys_timer timer)
{
    return sys_timer_get_user_data((u16)timer);
}

#ifdef UNITTEST
#include "test.h"
static u32 clock_value, last_period;
static u16 last_id, add_result;
static void *last_context;
static void (*last_callback)(void *);
static int calls;
u32 sys_timer_get_ms(void) { ++calls; return clock_value; }
u16 sys_timer_add(void *priv, void (*func)(void *), u32 msec)
{
    ++calls; last_context = priv; last_callback = func; last_period = msec;
    return add_result;
}
void sys_timeout_del(u16 id) { ++calls; last_id = id; }
void sys_timer_re_run(u16 id) { ++calls; last_id = id; }
int sys_timer_modify(u16 id, u32 msec)
{
    ++calls; last_id = id; last_period = msec; return -1;
}
void sys_timer_set_user_data(u16 id, void *context)
{
    ++calls; last_id = id; last_context = context;
}
void *sys_timer_get_user_data(u16 id) { ++calls; last_id = id; return last_context; }
static void callback(void *context) { (void)context; }
int main(void)
{
    int context;
    /* IR returns the 32-bit clock unchanged, including wrap boundaries. */
    clock_value = 0; CHECK(sys_timer_get_time() == 0);
    clock_value = 0xffffffffu; CHECK(sys_timer_get_time() == 0xffffffffu);
    clock_value = 0; CHECK(sys_timer_get_time() == 0);
    /* Both wrappers pass null context and preserve all 32 period bits;
     * the i16 result is zero-extended even when bit 15 is set. */
    add_result = 0xffff;
    CHECK(sys_timer_register(0xffffffffu, callback) == 65535);
    CHECK(last_context == NULL && last_callback == callback && last_period == 0xffffffffu);
    add_result = 0;
    CHECK(sys_timer_register_periodic(0, NULL) == 0);
    CHECK(last_context == NULL && last_callback == NULL && last_period == 0);
    /* Every handle-taking wrapper truncates to i16 without validation. */
    sys_timer_remove(-1); CHECK(last_id == 65535);
    sys_timer_reset(65536); CHECK(last_id == 0);
    sys_timer_change_period(0x12345678, 0xffffffffu);
    CHECK(last_id == 0x5678 && last_period == 0xffffffffu);
    sys_timer_set_context(-65536, &context);
    CHECK(last_id == 0 && last_context == &context);
    CHECK(sys_timer_get_context(-1) == &context && last_id == 65535);
    sys_timer_set_context(0, NULL);
    CHECK(sys_timer_get_context(65536) == NULL && last_id == 0);
    CHECK(calls == 12);
    return TEST_DONE("btctrler_sys_timer");
}
#endif
