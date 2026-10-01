#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pad.h"

#define LOG_LINES 64
#define LOG_TEXT  124
#define LOG_MAGIC 0x474f4c43

static struct {
    u32 magic;
    u32 head;
    struct { u32 seq; char text[LOG_TEXT]; } line[LOG_LINES];
} ring RETAINED;

void util_log_init(void)
{
    if (ring.magic == LOG_MAGIC) return;
    memset(&ring, 0, sizeof(ring));
    ring.magic = LOG_MAGIC;
}

void util_log(const char *fmt, ...)
{
    char text[LOG_TEXT];
    va_list ap;
    u32 i;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    plat_lock();
    i = ring.head++ % LOG_LINES;
    ring.line[i].seq = ring.head;
    memcpy(ring.line[i].text, text, sizeof(text));
    plat_unlock();
}

/* Lines from sequence number *seq on, "text\n" each, as many as fit.  *seq
 * becomes the number of the first line returned (later than asked if the
 * older ones were overwritten); the return is the byte count, and the next
 * read starts at *seq plus the lines returned, which the caller counts. */
u32 util_log_read(u32 *seq, char *out, u32 max)
{
    u32 oldest = ring.head > LOG_LINES ? ring.head - LOG_LINES + 1 : 1;
    u32 s = *seq < oldest ? oldest : *seq;
    u32 used = 0;

    *seq = s;
    for (; s <= ring.head; s++) {
        const char *t = ring.line[(s - 1) % LOG_LINES].text;
        u32 n = strlen(t);

        if (used + n + 1 > max) break;
        memcpy(out + used, t, n);
        used += n;
        out[used++] = '\n';
    }
    return used;
}

void util_put16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

void util_put32(u8 *p, u32 v)
{
    util_put16(p, (u16)v);
    util_put16(p + 2, (u16)(v >> 16));
}

u32 util_get32(const u8 *p)
{
    return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

#ifdef UNITTEST
void plat_lock(void) {}
void plat_unlock(void) {}

int main(void)
{
    char buf[4096];
    u32 seq = 1, n, i;

    util_log_init();
    CHECK(util_log_read(&seq, buf, sizeof(buf)) == 0);
    util_log("hello %d", 1);
    util_log("world");
    n = util_log_read(&seq, buf, sizeof(buf));
    CHECK(seq == 1 && n == 14 && !memcmp(buf, "hello 1\nworld\n", 14));
    seq = 2;
    n = util_log_read(&seq, buf, sizeof(buf));
    CHECK(seq == 2 && n == 6);
    for (i = 0; i < 100; i++) util_log("line %u", i);
    seq = 1;
    n = util_log_read(&seq, buf, sizeof(buf));
    CHECK(seq == 102 - LOG_LINES + 1);
    CHECK(!memcmp(buf, "line 36\n", 8));
    seq = 1;
    n = util_log_read(&seq, buf, 20);
    CHECK(n == 16);
    util_log_init();
    seq = 102;
    CHECK(util_log_read(&seq, buf, sizeof(buf)) == 8);
    return TEST_DONE("utils");
}
#endif
