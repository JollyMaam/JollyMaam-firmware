/* The SDK's link_task.c: the Classic link scheduler.  Every link (ACL,
 * inquiry/page, TWS, LE-in-Classic, idle) is a link_task on task_head; one
 * slot timer (task_timer) gives the current task run_slot slots, then
 * link_task_schedule picks the next: among runnable tasks (state bit 1
 * clear) that have runs left in this round (run_cnt < run_num), the highest
 * cur_priority, first on the list on a tie.  A task that runs loses one
 * priority and goes to the tail; the others creep back up.  When nobody has
 * runs left the round restarts; when nobody is runnable the scheduler idles
 * for max_sleep (or 8) slots.  link_task_adjust hands out the slot budgets
 * by link type.  The idle task (type 1) arms powerdown_timer, which asks the
 * baseband to sleep (bredr_power_off).
 *
 * Task types seen here: 1 idle, 2/128 TWS, 8/512 inquiry/page, 16 LE, 32 ACL,
 * 256 TWS ACL.  sta_attribute: 1 use default_run_slot, 2 audio (larger
 * slots), 4 don't schedule on add.
 *
 * Kept as the SDK has it: link_task_adjust collects ACL tasks into a
 * two-entry array without a bound; __link_task_del unlinks without
 * re-initialising the node, link_task_close_all and the scheduler re-initialise
 * it; nested interrupt disables as written; link_task_set_sleep_slot prints
 * with printf. */
#include "btctrler/glue.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

extern BTCTRLER_CONFIG int config_btctler_dual_a2dp;

#define TIMER_NONE 0xff

static bool schedule_suspend;
static int schedule_period;
static s16 max_sleep;
static struct list_head task_head = { &task_head, &task_head };
static struct link_task *curr_task;
static u8 task_timer = TIMER_NONE;
static u16 idle_period_slot = 736;
static u8 powerdown_timer = TIMER_NONE;
static struct link_idle_task idle_task;
static bool idle_task_running;

static int idle_resume(struct link_task *task);
static int idle_suspend(struct link_task *task);
const struct link_task_ops idle_task_ops = { idle_resume, idle_suspend };

#define TASK_OF(node) ((struct link_task *)((u8 *)(node) - offsetof(struct link_task, entry)))
#define FOR_EACH_TASK(t) \
    for (struct list_head *n_ = task_head.next; n_ != &task_head && ((t) = TASK_OF(n_), 1); n_ = n_->next)

_Static_assert(offsetof(struct link_task, entry) == 16, "SDK link_task layout");

#ifdef CONFIG_CPU_BD19
/* The BD19 object exports i8, while the recovered shared-header prototype is
 * bool/i1. Bind the source implementation to the SDK symbol with its i8 ABI. */
u8 link_task_idle_task_enable_detect_ir(void)
    __asm__("link_task_idle_task_enable_detect");
#define link_task_idle_task_enable_detect link_task_idle_task_enable_detect_ir
#define LINK_TASK_IDLE_RESULT u8
#else
#define LINK_TASK_IDLE_RESULT bool
#endif

/* Unlink and point the node at itself. */
static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static void list_add_tail(struct list_head *new)
{
    struct list_head *last = task_head.prev;
    task_head.prev = new;
    new->next = &task_head;
    new->prev = last;
    last->next = new;
}

static int list_empty(void)
{
    return task_head.next == &task_head;
}

__attribute__((weak)) u8 is_dac_power_off(void)
{
    return 1;
}

__attribute__((weak)) u16 get_run_slot_whit_inquiry(void)
{
    return 110;
}

/* Make new the running task.  0 if the current one refused to stop (it
 * asked for that many more slots, which the timer now gives it). */
static u8 link_task_switch(struct link_task *new)
{
    struct link_task *cur = curr_task;
    if (cur == new) return 1;
    if (cur) {
        int more = cur->ops->suspend(cur);
        if (more) {
            slot_timer_reset(task_timer, more);
            return 0;
        }
    }
    if (new) new->ops->resume(new);
    curr_task = new;
    return 1;
}

void link_task_close_all(void)
{
    local_irq_disable();
    schedule_suspend = 0;
    schedule_period = 0;
    max_sleep = 0;
    link_task_idle_disable();
    for (struct list_head *n = task_head.next, *next; n != &task_head; n = next) {
        next = n->next;
        list_del(n);
        if (curr_task == TASK_OF(n)) {
            TASK_OF(n)->ops->suspend(TASK_OF(n));
            curr_task = 0;
        }
    }
    if (task_timer != TIMER_NONE) {
        slot_timer_put(task_timer);
        task_timer = TIMER_NONE;
    }
    local_irq_enable();
}

struct link_task *get_curr_task(void)
{
    return curr_task;
}

/* The slot timer's callback (task is then 0), and the way to run a given
 * task now. */
void link_task_schedule(struct link_task *task)
{
    for (;;) {
        if (task_timer == TIMER_NONE) task_timer = slot_timer_get();
        u8 timer = task_timer;
        struct link_task *sel = 0, *p;

        if (task) {
            if (curr_task == task) {
                if (!schedule_suspend) slot_timer_set(timer, (void (*)(void *))link_task_schedule, 0, task->run_slot);
                return;
            }
            sel = task;
        } else {
            FOR_EACH_TASK(p)
                if (!(p->state & 2) && p->run_cnt < p->run_num && (!sel || sel->cur_priority < p->cur_priority))
                    sel = p;
        }
        if (sel) {
            if (link_task_switch(sel) != 1) return;
            list_del(&sel->entry);
            sel->cur_priority--;
            sel->run_cnt++;
            FOR_EACH_TASK(p)
                if (p->cur_priority < p->priority) p->cur_priority++;
            list_add_tail(&sel->entry);
            if (!schedule_suspend) slot_timer_set(task_timer, (void (*)(void *))link_task_schedule, 0, sel->run_slot);
            return;
        }
        if (list_empty()) return;

        /* New round. */
        u8 run_en = 0;
        FOR_EACH_TASK(p) {
            p->run_cnt = 0;
            p->cur_priority = p->priority;
            if (!(p->state & 2)) run_en = 1;
        }
        if (max_sleep) {
            link_task_switch(0);
            slot_timer_reset(task_timer, max_sleep);
            return;
        }
        if (!run_en) {
            link_task_switch(0);
            slot_timer_reset(task_timer, 8);
            return;
        }
        task = 0;
    }
}

void link_tws_task_adjust(u8 slot)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p)
        if (p->type == 256) p->run_slot = slot;
    local_irq_enable();
}

/* Slot budgets for the tasks after new joins (or a task changes). */
static void link_task_adjust(struct link_task *new)
{
    struct link_task *conn[2], *p;
    int other_num = 0, conn_num = 0, tws_num = 0;

    FOR_EACH_TASK(p) {
        switch (p->type) {
        case 32:  conn[conn_num++] = p; break;
        case 256: conn[conn_num++] = p; tws_num++; break;
        case 1:   break;
        case 128: tws_num++; other_num++; break;
        default:  other_num++; break;
        }
    }
    u16 type = new->type;
    if (type == 128 || type == 256) tws_num++;

    if (conn_num == 0) {
        switch (type) {
        case 32: case 256:
            new->run_slot = 72;
            if (other_num) return;
            break;
        case 16: new->run_slot = 64; break;
        case 1:  break;
        default: new->run_slot = 32; break;
        }
        max_sleep = 0;
        return;
    }

    if (tws_num) {
        if (conn_num != 1) return;
        switch (type) {
        case 32: case 256:
            if (conn[0] == new) {
                if (new->sta_attribute == 2) {
                    new->default_run_slot = 350;
                    new->run_slot = 350;
                } else if (new->sta_attribute == 1 && new->default_run_slot) {
                    new->run_slot = new->default_run_slot;
                }
                return;
            }
            if (type == 256) {
                new->run_slot = 32;
                new->default_run_slot = 32;
                conn[0]->run_num = 1;
                conn[0]->run_slot = 64;
                conn[0]->default_run_slot = 64;
            } else {
                conn[0]->run_num = 1;
                conn[0]->run_slot = 32;
                conn[0]->default_run_slot = 32;
                new->run_slot = 64;
                new->default_run_slot = 64;
            }
            max_sleep = 0;
            return;
        case 16: new->run_slot = 64; break;
        case 1:  break;
        default: new->run_slot = 32; break;
        }
        struct link_task *c = conn[0];
        c->run_num = (u8)(other_num + 1);
        bool audio = c->sta_attribute == 2;
        if (type == 16) {
            c->default_run_slot = audio ? 350 : 16;
            c->run_slot = audio ? 128 : 16;
            max_sleep = 0;
        } else if (audio) {
            c->default_run_slot = 32;
            c->run_slot = 128;
            max_sleep = 0;
        } else {
            c->run_slot = 32;
            c->default_run_slot = 32;
            max_sleep = c->type == 256 ? 64 : 0;
        }
        return;
    }

    if (conn_num == 1) {
        switch (type) {
        case 32:
            if (conn[0] == new) {
                if (new->sta_attribute == 2) {
                    new->default_run_slot = 1600;
                    new->run_slot = 1600;
                } else if (new->sta_attribute == 1 && new->default_run_slot) {
                    new->run_slot = new->default_run_slot;
                }
            } else {
                if (conn[0]->sta_attribute == 2) {
                    conn[0]->default_run_slot = 48;
                    conn[0]->run_slot = 128;
                } else {
                    conn[0]->run_num = 1;
                    conn[0]->run_slot = 48;
                }
                new->run_slot = 48;
            }
            max_sleep = 0;
            return;
        case 8: case 512:
            conn[0]->run_slot = get_run_slot_whit_inquiry();
            break;
        case 16:
            new->run_slot = 64;
            conn[0]->run_slot = conn[0]->sta_attribute == 2 ? 128 : 48;
            break;
        case 1:
            break;
        case 2: case 128:
            new->run_slot = 32;
            break;
        default:
            conn[0]->run_slot = 1600;
            new->run_slot = 32;
            break;
        }
        conn[0]->run_num = (u8)(other_num + 1);
        max_sleep = 0;
        return;
    }

    if (conn_num == 2) {
        if (new->sta_attribute == 2) {
            new->default_run_slot = 48;
            new->run_slot = 128;
            if (config_btctler_dual_a2dp) conn[0]->run_slot = 128;
        } else if (new->sta_attribute == 1 && new->default_run_slot) {
            new->run_slot = new->default_run_slot;
        }
    }
}

void link_task_reset_slot(struct link_task *task, u8 sta_attribute)
{
    if (!task) return;
    local_irq_disable();
    if (sta_attribute) task->sta_attribute = sta_attribute;
    link_task_adjust(task);
    local_irq_enable();
}

static void __link_task_add(struct link_task *new)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p) {
        if (p == new) {
            local_irq_enable();
            return;
        }
    }
    new->run_cnt = 0;
    new->cur_priority = new->priority;
    new->state &= ~2;
    link_task_adjust(new);
    list_add_tail(&new->entry);
    if (task_timer == TIMER_NONE) {
        task_timer = slot_timer_get();
        link_task_schedule(0);
    } else if ((new->type == 32 || new->type == 256) && !(new->sta_attribute & 4)) {
        link_task_schedule(new);
    }
    local_irq_enable();
}

void link_task_add(struct link_task *new)
{
    local_irq_disable();
    __link_task_add(new);
    local_irq_enable();
}

static void __link_task_del(struct link_task *task)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p) {
        if (p == task) {
            struct list_head *prev = task->entry.prev, *next = task->entry.next;
            next->prev = prev;
            prev->next = next;
            break;
        }
    }
    switch (task->type) {
    case 32: case 256: case 8: case 512:
        FOR_EACH_TASK(p) {
            if (p->type == 32 || p->type == 256) {
                link_task_adjust(p);
                break;
            }
        }
        break;
    }
    if (curr_task == task) {
        task->ops->suspend(task);
        curr_task = 0;
        if (!list_empty() && !schedule_suspend) link_task_schedule(0);
    }
    if (list_empty() && task_timer != TIMER_NONE) {
        slot_timer_put(task_timer);
        task_timer = TIMER_NONE;
    }
    local_irq_enable();
}

void link_task_del(struct link_task *task)
{
    local_irq_disable();
    __link_task_del(task);
    local_irq_enable();
}

/* run_en 0 keeps the task out of scheduling (state bit 1). */
void link_task_run_set(struct link_task *task, u8 run_en)
{
    local_irq_disable();
    if (run_en) task->state &= ~2;
    else task->state |= 2;
    local_irq_enable();
}

void link_task_run(struct link_task *task)
{
    local_irq_disable();
    link_task_schedule(task);
    local_irq_enable();
}

void link_task_run_slots(struct link_task *task, int slots)
{
    local_irq_disable();
    u16 saved = task->run_slot;
    task->run_slot = (u16)slots;
    link_task_schedule(task);
    task->run_slot = saved;
    local_irq_enable();
}

void link_task_try_run_slots(struct link_task *task, int slots)
{
    if ((int)link_task_remain_slot(task) < slots) link_task_run_slots(task, slots);
}

u32 link_task_remain_slot(struct link_task *task)
{
    if (curr_task != task) return 0;
    return (u32)slot_timer_remain(task_timer);
}

void link_other_task_run_slots(struct link_task *task, int slots)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p) {
        if (p != task && p->type == 32) {
            link_task_schedule(p);
            slot_timer_reset(task_timer, slots);
        }
    }
    local_irq_enable();
}

void link_task_set_slots(struct link_task *task, int slots)
{
    task->run_slot = (u16)slots;
    slot_timer_reset(task_timer, slots);
}

void link_task_reset_sbc_slots(int slots)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p)
        if (p->type == 32 && p->sta_attribute == 2) link_task_set_slots(p, slots);
    local_irq_enable();
}

void link_task_schedule_reset(void)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p) {
        p->run_cnt = 0;
        p->cur_priority = p->priority;
    }
    link_task_switch(0);
    local_irq_enable();
}

void link_task_set_period(int period)
{
    local_irq_disable();
    schedule_period = period;
    local_irq_enable();
}

void link_task_schedule_suspend(void)
{
    schedule_suspend = 1;
    if (task_timer != TIMER_NONE) slot_timer_disable(task_timer);
}

void link_task_schedule_resume(void)
{
    if (schedule_suspend) {
        schedule_suspend = 0;
        link_task_schedule(curr_task);
    }
}

void link_task_set_sleep_slot(int slot)
{
    max_sleep = (s16)slot;
    printf("link_task_sleep: %d\n", slot);
}

/* 1 if a sleep length is set or more than one task is listed. */
int link_task_schedule_active(void)
{
    int active = 1;
    local_irq_disable();
    if (!max_sleep) {
        int count = 0;
        active = 0;
        for (struct list_head *n = task_head.next; n != &task_head; n = n->next)
            if (++count > 1) {
                active = 1;
                break;
            }
    }
    local_irq_enable();
    return active;
}

void set_idle_period_slot(u16 slot)
{
    local_irq_disable();
    idle_period_slot = (u16)(slot - 64);
    local_irq_enable();
}

/* The idle task's turn: arm the power-down timer (8 slots before the
 * scheduling period ends, or the idle period less 14) and run until then. */
static int idle_resume(struct link_task *task)
{
    idle_task_running = 1;
    if (schedule_period) {
        int left = schedule_period - (int)bredr_read_slot_clk();
        if (left > 12) {
            if (powerdown_timer == TIMER_NONE) powerdown_timer = slot_timer_get();
#ifdef CONFIG_CPU_BD19
            slot_timer_set(powerdown_timer, (void (*)(void *))powerdown_entry, (void *)(intptr_t)(left - 4), 4);
#else
            slot_timer_set(powerdown_timer, (void (*)(void *))powerdown_entry, (void *)(intptr_t)(left - 8), 8);
#endif
        }
        task->run_slot = (u16)(left + 32);
    } else {
        u16 period = idle_period_slot;
        task->run_slot = period;
        if (powerdown_timer == TIMER_NONE) powerdown_timer = slot_timer_get();
#ifdef CONFIG_CPU_BD19
        slot_timer_set(powerdown_timer, (void (*)(void *))powerdown_entry, (void *)(intptr_t)(period - 10), 4);
#else
        slot_timer_set(powerdown_timer, (void (*)(void *))powerdown_entry, (void *)(intptr_t)(period - 14), 8);
#endif
    }
    return 0;
}

static int idle_suspend(struct link_task *task)
{
    (void)task;
    local_irq_disable();
    if (powerdown_timer != TIMER_NONE) {
        slot_timer_put(powerdown_timer);
        powerdown_timer = TIMER_NONE;
    }
    local_irq_enable();
    bredr_power_on();
    idle_task_running = 0;
    return 0;
}

/* The power-down timer's callback: sleep for slot slots if idling is on. */
void powerdown_entry(s32 slot)
{
    local_irq_disable();
    if (powerdown_timer != TIMER_NONE) {
        slot_timer_put(powerdown_timer);
        powerdown_timer = TIMER_NONE;
    }
    local_irq_enable();
    if (idle_task.state) bredr_power_off(slot);
}

void idle_reset(int slot)
{
    if (powerdown_timer != TIMER_NONE) slot_timer_put(powerdown_timer);
    powerdown_timer = slot_timer_get();
#ifdef CONFIG_CPU_BD19
    if (slot > 4) slot_timer_set(powerdown_timer, (void (*)(void *))powerdown_entry, (void *)(intptr_t)(slot - 4), 4);
#else
    if (slot > 8) slot_timer_set(powerdown_timer, (void (*)(void *))powerdown_entry, (void *)(intptr_t)(slot - 8), 8);
#endif
    bredr_power_off(0);
}

u8 get_idle_task_state(void)
{
    return idle_task_running;
}

/* True if no listed task has a type that keeps the radio busy (0xfe58). */
LINK_TASK_IDLE_RESULT link_task_idle_task_enable_detect(void)
{
    u16 types = 0;
    struct link_task *p;
    FOR_EACH_TASK(p) types |= p->type;
    return (types & 0xfe58) == 0;
}

/* Add the idle task unless an LE task (type 16) is listed. */
u32 link_task_idle_enable(void)
{
    struct link_task *p;
    local_irq_disable();
    FOR_EACH_TASK(p) {
        if (p->type & 16) {
            local_irq_enable();
            return 0;
        }
    }
    if (!idle_task.state) {
        idle_task.state = 1;
        max_sleep = 0;
        idle_task.task.type = 1;
        idle_task.task.run_slot = idle_period_slot;
        idle_task.task.run_num = 1;
        idle_task.task.priority = 0;
        idle_task.task.ops = &idle_task_ops;
        idle_task.task.sta_attribute = 1;
        __link_task_add(&idle_task.task);
    }
    local_irq_enable();
    return 0;
}

u32 link_task_idle_disable(void)
{
    local_irq_disable();
    if (idle_task.state & 1) {
        idle_task.state = 0;
        schedule_period = 0;
        __link_task_del(&idle_task.task);
    }
    local_irq_enable();
    return 0;
}

void link_task_idle_run(int slot)
{
    local_irq_disable();
    link_task_switch(0);
    slot_timer_reset(task_timer, slot);
    local_irq_enable();
}

#ifdef UNITTEST
#include "test.h"
#include <string.h>
int config_btctler_dual_a2dp;
static int irq_depth, timers_got, timer_put, timer_set_slot, timer_reset_slot, timer_disabled, off_slot = -1, power_on;
/* The fake allocator issues fresh IDs across all cases instead of reusing
 * released timers; capture every ID and fail before exhausting the model. */
static void (*handlers[64])(void *);
static void *privs[64];
static u32 clk;
u8 slot_timer_get(void) { CHECK(timers_got < (int)(sizeof handlers / sizeof handlers[0])); return (u8)timers_got++; }
static int pd_slot; static void *pd_priv;
void slot_timer_set(u8 i, void (*h)(void *), void *priv, int slot)
{
    CHECK(i < sizeof handlers / sizeof handlers[0]);
    handlers[i] = h; privs[i] = priv;
    if (h == (void (*)(void *))powerdown_entry) { pd_priv = priv; pd_slot = slot; } else timer_set_slot = slot;
}
void slot_timer_reset(u8 i, int slot) { (void)i; timer_reset_slot = slot; }
void slot_timer_put(u8 i) { (void)i; timer_put++; }
void slot_timer_disable(u8 i) { (void)i; timer_disabled++; }
int slot_timer_remain(u8 i) { (void)i; return 40; }
void local_irq_disable(void) { irq_depth++; }
void local_irq_enable(void) { irq_depth--; }
u32 bredr_read_slot_clk(void) { return clk; }
void bredr_power_on(void) { power_on++; }
void bredr_power_off(int slot) { off_slot = slot; }

static int resumed[3], suspended[3], refuse;
static int t_resume(struct link_task *t) { resumed[t->priority]++; return 0; }
static int t_suspend(struct link_task *t) { suspended[t->priority]++; return refuse; }
static const struct link_task_ops ops = { t_resume, t_suspend };

int main(void)
{
    struct link_task acl, le;
    memset(&acl, 0, sizeof(acl));
    memset(&le, 0, sizeof(le));
    acl.type = 32; acl.priority = 1; acl.run_num = 1; acl.ops = &ops;
    le.type = 16; le.priority = 2; le.run_num = 1; le.ops = &ops;

    /* First task: a timer, budget 72 (alone), runs at once. */
    link_task_add(&acl);
    CHECK(irq_depth == 0 && timers_got == 1 && task_timer == 0);
    CHECK(acl.run_slot == 72 && max_sleep == 0 && curr_task == &acl && resumed[1] == 1);
    CHECK(timer_set_slot == 72 && handlers[0] == (void (*)(void *))link_task_schedule && privs[0] == 0);
    CHECK(acl.run_cnt == 1 && acl.cur_priority == 0);

    /* Its slots are up and it has no runs left: new round, runs again
     * without a suspend/resume pair. */
    link_task_schedule(0);
    CHECK(curr_task == &acl && resumed[1] == 1 && suspended[1] == 0 && acl.run_cnt == 1);

    /* An LE task joins: LE gets 64 slots, the ACL 48 and one run per round
     * per other task.  LE is not scheduled on add. */
    link_task_add(&le);
    CHECK(le.run_slot == 64 && acl.run_slot == 48 && acl.run_num == 1 && curr_task == &acl);
    link_task_schedule(0);                       /* LE has runs left and higher priority */
    CHECK(curr_task == &le && suspended[1] == 1 && resumed[2] == 1 && timer_set_slot == 64);
    link_task_schedule(0);                       /* both spent: new round, LE (priority 2) first */
    CHECK(curr_task == &le && le.run_cnt == 1 && acl.run_cnt == 0);
    link_task_schedule(0);
    CHECK(curr_task == &acl && timer_set_slot == 48);

    /* A current task may refuse to stop: it keeps the radio for that many slots. */
    refuse = 5;
    link_task_schedule(&le);
    CHECK(curr_task == &acl && timer_reset_slot == 5);
    refuse = 0;

    /* Not runnable (state bit 1): skipped; nobody runnable idles 8 slots. */
    link_task_run_set(&le, 0);
    link_task_run_set(&acl, 0);
    CHECK((le.state & 2) && (acl.state & 2));
    link_task_schedule(0);
    CHECK(curr_task == 0 && timer_reset_slot == 8);
    link_task_run_set(&le, 1);
    link_task_run_set(&acl, 1);

    /* run_slots: a one-off budget, restored afterwards. */
    link_task_run_slots(&acl, 20);
    CHECK(curr_task == &acl && timer_set_slot == 20 && acl.run_slot == 48);
    CHECK(link_task_remain_slot(&acl) == 40 && link_task_remain_slot(&le) == 0);

    /* Deleting the current task suspends it and schedules the rest. */
    link_task_del(&acl);
    CHECK(curr_task == &le && irq_depth == 0);
    link_task_del(&le);
    CHECK(curr_task == 0 && list_empty() && timer_put == 1 && task_timer == TIMER_NONE);

    /* Idle: enabled only without LE. The BD19 and BR23 power-down timers use
     * different lead times and callback periods in their SDK members. */
    set_idle_period_slot(800);
    CHECK(idle_period_slot == 736);
    CHECK(link_task_idle_task_enable_detect() == 1);
    link_task_add(&le);
    CHECK(link_task_idle_task_enable_detect() == 0);
    link_task_idle_enable();
    CHECK(idle_task.state == 0);
    link_task_del(&le);
    link_task_idle_enable();
    CHECK(idle_task.state == 1 && idle_task.task.run_slot == 736 && curr_task == &idle_task.task);
    CHECK(get_idle_task_state() == 1 && timer_set_slot == 736);
#ifdef CONFIG_CPU_BD19
    CHECK((intptr_t)pd_priv == 726 && pd_slot == 4);
#else
    CHECK((intptr_t)pd_priv == 722 && pd_slot == 8);
#endif
    powerdown_entry((s32)(intptr_t)pd_priv);
#ifdef CONFIG_CPU_BD19
    CHECK(off_slot == 726 && powerdown_timer == TIMER_NONE);
#else
    CHECK(off_slot == 722 && powerdown_timer == TIMER_NONE);
#endif

    /* With a scheduling period, preserve the SDK-specific power-down lead. */
    link_task_set_period(1000);
    clk = 900;
    idle_resume(&idle_task.task);
#ifdef CONFIG_CPU_BD19
    CHECK((intptr_t)pd_priv == 96 && pd_slot == 4 && idle_task.task.run_slot == 132);
#else
    CHECK((intptr_t)pd_priv == 92 && idle_task.task.run_slot == 132);
#endif
    pd_priv = (void *)1; pd_slot = -1;
    clk = 988;                                  /* 12 left: strict threshold, no re-arm */
    idle_resume(&idle_task.task);
    CHECK(pd_priv == (void *)1 && idle_task.task.run_slot == 44);
    clk = 987;                                  /* 13 left: re-arm at chip-specific lead */
    idle_resume(&idle_task.task);
#ifdef CONFIG_CPU_BD19
    CHECK((intptr_t)pd_priv == 9 && pd_slot == 4 && idle_task.task.run_slot == 45);
#else
    CHECK((intptr_t)pd_priv == 5 && pd_slot == 8 && idle_task.task.run_slot == 45);
#endif
    clk = 995;                                   /* 5 left: no power-down timer */
    pd_priv = (void *)1;
    idle_resume(&idle_task.task);
    CHECK(pd_priv == (void *)1 && idle_task.task.run_slot == 37);

    link_task_set_sleep_slot(0);
    CHECK(link_task_schedule_active() == 0);
    link_task_idle_disable();
    CHECK(idle_task.state == 0 && schedule_period == 0 && power_on == 1 && curr_task == 0);
    CHECK(get_idle_task_state() == 0 && list_empty());

    /* idle_reset uses the same chip-specific callback threshold/lead. */
    off_slot = -1; pd_priv = (void *)99; pd_slot = -99;
    idle_reset(4);
    CHECK(pd_priv == (void *)99 && pd_slot == -99 && off_slot == 0);
    idle_reset(5);
#ifdef CONFIG_CPU_BD19
    CHECK((intptr_t)pd_priv == 1 && pd_slot == 4);
#else
    CHECK(pd_priv == (void *)99 && pd_slot == -99);
#endif
    idle_reset(13);
#ifdef CONFIG_CPU_BD19
    CHECK((intptr_t)pd_priv == 9 && pd_slot == 4);
#else
    CHECK((intptr_t)pd_priv == 5 && pd_slot == 8);
#endif
    powerdown_entry(9); /* idle task is disabled, so this only releases timer */
    CHECK(powerdown_timer == TIMER_NONE && off_slot == 0);

    link_task_add(&acl);
    link_task_add(&le);
    CHECK(link_task_schedule_active() == 1);
    link_task_schedule_suspend();
    CHECK(schedule_suspend && timer_disabled == 1);
    link_task_schedule_resume();
    CHECK(!schedule_suspend);
    link_task_close_all();
    CHECK(list_empty() && curr_task == 0 && task_timer == TIMER_NONE && irq_depth == 0);
    CHECK(acl.entry.next == &acl.entry && le.entry.next == &le.entry);
    return TEST_DONE("btctrler_link_task");
}
#endif
