/* Cooperative controller threads, as in the SDK. Name identity uses CRC only;
 * create leaves priority untouched, and timed suspend does not consume a resume.
 * The SDK's undefined timeout accumulator is fixed: suspend receives the
 * smallest thread timeout, or the largest byte timeout for an empty list. */
#include "btctrler/glue.h"

_Static_assert(offsetof(struct thread, entry) == 0, "thread list prefix");
#ifndef UNITTEST
_Static_assert(offsetof(struct thread, owner) == 8, "owner");
_Static_assert(offsetof(struct thread, name) == 12, "name");
_Static_assert(offsetof(struct thread, fun) == 16, "fun");
_Static_assert(offsetof(struct thread, priority) == 20, "priority");
_Static_assert(offsetof(struct thread, resume_cnt) == 21, "resume_cnt");
_Static_assert(offsetof(struct thread, suspend_timeout) == 22, "suspend_timeout");
_Static_assert(offsetof(struct thread_owner, head) == 0, "head");
_Static_assert(offsetof(struct thread_owner, create) == 8, "create");
_Static_assert(offsetof(struct thread_owner, delete) == 12, "delete");
_Static_assert(offsetof(struct thread_owner, suspend) == 16, "suspend");
_Static_assert(offsetof(struct thread_owner, resume) == 20, "resume");
#endif

extern BTCTRLER_CONFIG u8 log_tag_const_i_Thread;
void log_print(int level, const char *tag, const char *format, ...);

static void thread_schedule(struct thread_owner *owner)
{
    struct list_head *head = &owner->head;
    u8 logging = log_tag_const_i_Thread;
    for (struct list_head *node = head->next, *next; node != head; node = next) {
        struct thread *p = (struct thread *)node;
        next = node->next; /* callbacks may delete their current thread */
        if (p->resume_cnt) {
            if (logging)
                log_print(2, NULL, "[Info]: [Thread]thread_schedule \"%s\"\r\n", p->name);
            p->fun(p);
        }
    }
    u8 suspend_timeout = 255;
    for (struct list_head *node = head->next; node != head; node = node->next) {
        struct thread *p = (struct thread *)node;
        if (p->resume_cnt && !p->suspend_timeout) return;
        if (p->resume_cnt)
            btctrler_assert_failed("p->resume_cnt == 0", p->resume_cnt);
        if (suspend_timeout > p->suspend_timeout)
            suspend_timeout = p->suspend_timeout;
    }
    owner->suspend(0, (u8)suspend_timeout);
}

int thread_owner_init(struct thread_owner *owner)
{
    owner->head.next = owner->head.prev = &owner->head;
    return 0;
}

int thread_create(struct thread *th, char *name, void (*fun)(struct thread *),
                  struct thread_owner *owner)
{
    struct list_head *head = &owner->head;
    u32 crc = crc_get_32bit(name);
    u8 logging = log_tag_const_i_Thread;
    for (struct list_head *node = head->next; node != head; node = node->next) {
        struct thread *p = (struct thread *)node;
        if (logging)
            log_print(2, NULL, "[Info]: [Thread]thread_query \"%s\"/ \"%s\"\r\n", p->name, name);
        if (crc == crc_get_32bit(p->name)) return 0;
    }
    th->fun = fun;
    th->resume_cnt = 1;
    th->suspend_timeout = 0;
    th->name = name;
    th->owner = owner;
    if (head->next == head) {
        if (logging)
            log_print(2, NULL, "[Info]: [Thread]thread_first_create \"%s\" %d\r\n", name, 0);
        int err = th->owner->create(thread_schedule, 0);
        if (err) return err;
    } else if (logging) {
        log_print(2, NULL, "[Info]: [Thread]thread_create 0x%x /\"%s\"\r\n", th, name);
    }
    struct list_head *prev = head->prev;
    head->prev = &th->entry;
    th->entry.next = head;
    th->entry.prev = prev;
    prev->next = &th->entry;
    return 0;
}

void thread_suspend(struct thread *th, int timeout)
{
    if (log_tag_const_i_Thread)
        log_print(2, NULL, "[Info]: [Thread]thread_suspend \"%s\"\r\n", th->name);
    local_irq_disable();
    th->suspend_timeout = (u8)timeout;
    if (!timeout && th->resume_cnt) --th->resume_cnt;
    local_irq_enable();
}

void thread_resume(struct thread *th)
{
    if (log_tag_const_i_Thread)
        log_print(2, NULL, "[Info]: [Thread]thread_resume \"%s\"\r\n", th->name);
    local_irq_disable();
    th->suspend_timeout = 0;
    if (th->resume_cnt != 255) ++th->resume_cnt;
    local_irq_enable();
    th->owner->resume();
}

int thread_delete(struct thread *th)
{
    struct thread_owner *owner = th->owner;
    th->entry.next->prev = th->entry.prev;
    th->entry.prev->next = th->entry.next;
    th->entry.next = th->entry.prev = &th->entry;
    return owner->head.next == &owner->head ? owner->delete(th->priority) : 0;
}

void thread_run(struct thread_owner *owner) { thread_schedule(owner); }

#ifdef UNITTEST
#include "test.h"
u8 log_tag_const_i_Thread;
static int irq, creates, deletes, resumes, suspends, create_err, runs;
static u8 deleted_priority, suspended_timeout;
static void (*scheduler)(struct thread_owner *);
void log_print(int level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; }
u32 crc_get_32bit(const char *src) { return (u8)*src; }
void local_irq_disable(void) { CHECK(!irq); irq = 1; }
void local_irq_enable(void) { CHECK(irq); irq = 0; }
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; CHECK(0); }
static int create(void (*fn)(struct thread_owner *), u8 priority)
{ CHECK(priority == 0); scheduler = fn; creates++; return create_err; }
static int delete(u8 priority)
{ deleted_priority = priority; deletes++; return -7; }
static int suspend(u8 priority, u8 timeout)
{ CHECK(priority == 0); suspended_timeout = timeout; suspends++; return 0; }
static int resume(void) { CHECK(!irq); resumes++; return 0; }
static void run(struct thread *th) { (void)th; runs++; }
static void remove_self(struct thread *th) { runs++; thread_delete(th); }
int main(void)
{
    struct thread_owner owner = { .create = create, .delete = delete,
        .suspend = suspend, .resume = resume };
    struct thread a = { .priority = 23 }, b = { .priority = 42 }, duplicate = { .priority = 99 };
    CHECK(thread_owner_init(&owner) == 0);
    CHECK(owner.head.next == &owner.head && owner.head.prev == &owner.head);
    create_err = -3;
    CHECK(thread_create(&a, "a", run, &owner) == -3);
    CHECK(a.resume_cnt == 1 && a.suspend_timeout == 0 && a.priority == 23);
    CHECK(owner.head.next == &owner.head);
    create_err = 0;
    CHECK(thread_create(&a, "a", run, &owner) == 0 && creates == 2);
    CHECK(thread_create(&b, "b", run, &owner) == 0 && creates == 2);
    CHECK(owner.head.next == &a.entry && owner.head.prev == &b.entry);
    /* The IR compares CRCs, not strings: this stub deliberately collides. */
    CHECK(thread_create(&duplicate, "another", run, &owner) == 0);
    CHECK(duplicate.owner == NULL && duplicate.priority == 99);
    thread_suspend(&a, 256); /* truncates timeout, but full-width nonzero avoids decrement */
    CHECK(a.suspend_timeout == 0 && a.resume_cnt == 1);
    thread_suspend(&a, -1);
    CHECK(a.suspend_timeout == 255 && a.resume_cnt == 1);
    thread_suspend(&a, 0);
    thread_suspend(&a, 0);
    CHECK(a.resume_cnt == 0 && a.suspend_timeout == 0);
    thread_resume(&a);
    CHECK(a.resume_cnt == 1 && resumes == 1);
    a.resume_cnt = 254;
    thread_resume(&a); thread_resume(&a);
    CHECK(a.resume_cnt == 255 && resumes == 3);
    /* A runnable zero-timeout thread prevents the owner from suspending. */
    thread_run(&owner);
    CHECK(runs == 2 && suspends == 0);
    a.fun = remove_self;
    scheduler(&owner);
    CHECK(runs == 4 && deletes == 0 && owner.head.next == &b.entry);
    CHECK(a.entry.next == &a.entry && a.entry.prev == &a.entry);
    CHECK(thread_delete(&b) == -7 && deletes == 1 && deleted_priority == 42);
    CHECK(owner.head.next == &owner.head && owner.head.prev == &owner.head);
    thread_run(&owner);
    CHECK(suspends == 1 && suspended_timeout == 255);
    CHECK(thread_create(&a, "a", run, &owner) == 0);
    CHECK(thread_create(&b, "b", run, &owner) == 0);
    a.resume_cnt = b.resume_cnt = 0;
    a.suspend_timeout = 200; b.suspend_timeout = 7;
    thread_run(&owner);
    CHECK(suspends == 2 && suspended_timeout == 7 && runs == 4);
    a.suspend_timeout = 3; b.suspend_timeout = 200;
    thread_run(&owner);
    CHECK(suspends == 3 && suspended_timeout == 3);
    a.suspend_timeout = b.suspend_timeout = 255;
    thread_run(&owner);
    CHECK(suspends == 4 && suspended_timeout == 255);
    b.suspend_timeout = 0;
    thread_run(&owner);
    CHECK(suspends == 5 && suspended_timeout == 0);
    /* The final callback may remove the last thread before the minimum pass. */
    CHECK(thread_delete(&b) == 0);
    a.fun = remove_self; a.resume_cnt = 1;
    thread_run(&owner);
    CHECK(suspends == 6 && suspended_timeout == 255 && runs == 5);
    return TEST_DONE("btctrler_thread");
}
#endif
