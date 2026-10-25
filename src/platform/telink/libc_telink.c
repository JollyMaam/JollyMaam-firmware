#include <stdarg.h>
#include "sdk.h"

/* What the app needs of the C library that the Telink toolchain has none of
 * (the SDK's common/string.c has the string functions): vsnprintf and
 * snprintf for the log, with the conversions the app uses: d i u x X c s p
 * and %%, flags 0 and -, a width, a precision for s, and l (ignored: long
 * is 32 bits); and strncpy, in place of the SDK's (--wrap=strncpy), which
 * copies n bytes from the source whatever the string's length. */

struct out { char *p; size_t left; int n; };

static void put(struct out *o, char c)
{
    if (o->left > 1) {
        *o->p++ = c;
        o->left--;
    }
    o->n++;
}

static void field(struct out *o, const char *s, int len, int width, int left, char fill)
{
    int pad = width > len ? width - len : 0;

    if (!left)
        while (pad-- > 0) put(o, fill);
    while (len-- > 0) put(o, *s++);
    if (left)
        while (pad-- > 0) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    char digits[12];

    for (; *fmt; fmt++) {
        int left = 0, width = 0, precision = -1, len = 0;
        char fill = ' ';
        u32 v, base = 10;
        const char *s;

        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        for (; *fmt == '-' || *fmt == '0'; fmt++) {
            if (*fmt == '-') left = 1;
            else fill = '0';
        }
        for (; *fmt >= '0' && *fmt <= '9'; fmt++) width = width * 10 + *fmt - '0';
        if (*fmt == '.') {
            precision = 0;
            for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++) precision = precision * 10 + *fmt - '0';
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            s32 d = va_arg(ap, s32);

            if (d < 0) {
                if (fill == '0') {
                    put(&o, '-');
                    width--;
                } else {
                    digits[0] = '-';
                    len = 1;
                }
                v = (u32)-d;
            } else {
                v = (u32)d;
            }
            goto number;
        }
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            /* fall through */
        case 'x': case 'X':
            base = 16;
            /* fall through */
        case 'u':
            v = va_arg(ap, u32);
        number: {
            char rev[11];
            int k = 0;

            do {
                u32 r = v % base;

                rev[k++] = (char)(r < 10 ? '0' + r : (*fmt == 'X' ? 'A' : 'a') + r - 10);
                v /= base;
            } while (v);
            while (k) digits[len++] = rev[--k];
            field(&o, digits, len, width, left, left ? ' ' : fill);
            break;
        }
        case 'c':
            digits[0] = (char)va_arg(ap, int);
            field(&o, digits, 1, width, left, ' ');
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (s[len] && (precision < 0 || len < precision)) len++;
            field(&o, s, len, width, left, ' ');
            break;
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:                         /* not one of ours: shown as written */
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }
    if (size) *o.p = 0;
    return o.n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

char *__wrap_strncpy(char *dst, const char *src, unsigned int n)
{
    char *d = dst;

    for (; n && *src; n--) *d++ = *src++;
    for (; n; n--) *d++ = 0;
    return dst;
}
