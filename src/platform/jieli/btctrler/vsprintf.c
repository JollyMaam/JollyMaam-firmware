/* The SDK's binary packet serializer (vsprintf.c).  The format string is not
 * printf: each letter takes the next variadic argument and stores it as
 * little-endian bytes.  __vsprintf_len returns the size __vsprintf writes.
 *
 *   1 2 3 4   int, low 1..4 bytes          b NN  NN ints, low byte each
 *   H         int, low 2 bytes (as '2')    c NN  NN bytes from a pointer
 *   C NNN     NNN bytes from a pointer     S     NUL-terminated string
 *   A         6 bytes from a pointer       L     int: sets the 'B' length only
 *   l         int: one byte, sets 'B' length    B     'B' length bytes from a pointer
 *
 * NN and NNN are decimal digits in the format string, not validated.  SDK
 * quirks kept: 'b' in __vsprintf_len sets the length to NN instead of adding
 * to it; __vsprintf_len has no 'S' case (it neither counts nor consumes the
 * argument); 'B' before any 'L'/'l' copies an indeterminate length in the SDK
 * (here 0); unknown letters are skipped; __vsprintf_len(NULL) is 0 but
 * __vsprintf(NULL format) is not checked. */
#include <string.h>
#include "btctrler/glue.h"

int __vsprintf_len(const char *format, va_list argptr)
{
    va_list args; va_copy(args, argptr);
    int len = 0;
    if (!format) return 0;
    for (; *format; ++format) {
        switch (*format) {
        case '1': case '2': case '3': case '4':
            len = len + *format - '0';
            (void)va_arg(args, int);
            break;
        case 'b': {
            int count = format[1] * 10 + format[2] - 528;
            format += 2;
            len = count;
            while (count != 0) { --count; (void)va_arg(args, int); }
            break;
        }
        case 'c':
            len = len + format[1] * 10 + format[2] - 528;
            format += 2;
            (void)va_arg(args, int);
            break;
        case 'C':
            len = len + format[1] * 100 + format[2] * 10 + format[3] - 5328;
            format += 3;
            (void)va_arg(args, int);
            break;
        case 'A': len += 6; (void)va_arg(args, int); break;
        case 'H': len += 2; (void)va_arg(args, int); break;
        case 'L': len += va_arg(args, int); break;
        case 'B': (void)va_arg(args, int); break;
        case 'l': len = len + 1 + va_arg(args, int); break;
        default: break;
        }
    }
    va_end(args);
    return len;
}

int __vsprintf(u8 *data, const char *format, va_list argptr)
{
    va_list args; va_copy(args, argptr);
    u8 *out = data;
    int copy_len = 0;
    for (; *format; ++format) {
        switch (*format) {
        case '1': case '2': {
            int v = va_arg(args, int);
            *out++ = (u8)v;
            if (*format == '2') *out++ = (u8)(v >> 8);
            break;
        }
        case '3': case '4': {
            int v = va_arg(args, int);
            out[0] = (u8)v; out[1] = (u8)(v >> 8); out[2] = (u8)(v >> 16);
            out += 3;
            if (*format == '4') *out++ = (u8)(v >> 24);
            break;
        }
        case 'b': {
            int count = format[1] * 10 + format[2] - 528;
            format += 2;
            while (count != 0) { --count; *out++ = (u8)va_arg(args, int); }
            break;
        }
        case 'c': {
            int count = format[1] * 10 + format[2] - 528;
            format += 2;
            memcpy(out, (const void *)va_arg(args, void *), count);
            out += count;
            break;
        }
        case 'C': {
            int count = format[1] * 100 + format[2] * 10 + format[3] - 5328;
            format += 3;
            memcpy(out, (const void *)va_arg(args, void *), count);
            out += count;
            break;
        }
        case 'S': {
            const char *s = va_arg(args, const char *);
            strcpy((char *)out, s);
            out += strlen(s);
            break;
        }
        case 'A':
            memcpy(out, (const void *)va_arg(args, void *), 6);
            out += 6;
            break;
        case 'H': {
            int v = va_arg(args, int);
            out[0] = (u8)v; out[1] = (u8)(v >> 8);
            out += 2;
            break;
        }
        case 'L': copy_len = va_arg(args, int); break;
        case 'B':
            memcpy(out, (const void *)va_arg(args, void *), copy_len);
            out += copy_len;
            break;
        case 'l':
            copy_len = va_arg(args, int);
            *out++ = (u8)copy_len;
            break;
        default: break;
        }
    }
    va_end(args);
    return (int)(out - data);
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
static int run(u8 *data, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int n = __vsprintf(data, format, ap);
    va_end(ap);
    return n;
}
static int measure(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int n = __vsprintf_len(format, ap);
    va_end(ap);
    return n;
}
int main(void)
{
    u8 d[64];
    static const u8 src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    /* Little-endian integers, 1..4 bytes; higher bytes are dropped. */
    memset(d, 0xee, sizeof(d));
    CHECK(run(d, "1", 0x1234) == 1 && d[0] == 0x34 && d[1] == 0xee);
    CHECK(run(d, "2", 0x1234) == 2 && d[0] == 0x34 && d[1] == 0x12 && d[2] == 0xee);
    CHECK(run(d, "3", 0x12345678) == 3 && d[0] == 0x78 && d[1] == 0x56 && d[2] == 0x34 && d[3] == 0xee);
    CHECK(run(d, "4", (int)0x89abcdef) == 4 && d[0] == 0xef && d[1] == 0xcd && d[2] == 0xab && d[3] == 0x89);
    CHECK(run(d, "H", 0xa1b2) == 2 && d[0] == 0xb2 && d[1] == 0xa1);
    CHECK(run(d, "", 5) == 0);
    /* Several fields in sequence, unknown letters skipped. */
    CHECK(run(d, "1?2", 9, 0x0102) == 3 && d[0] == 9 && d[1] == 2 && d[2] == 1);
    /* 'b': NN bytes, one int each. */
    memset(d, 0xee, sizeof(d));
    CHECK(run(d, "b031", 0x101, 0x202, 0x303, 7) == 4 && !memcmp(d, (u8[]){ 1, 2, 3, 7 }, 4));
    CHECK(run(d, "b001", 5) == 1 && d[0] == 5);
    /* 'c' / 'C': fixed counts copied from a pointer. */
    CHECK(run(d, "c031", src, 9) == 4 && !memcmp(d, (u8[]){ 1, 2, 3, 9 }, 4));
    CHECK(run(d, "C0081", src, 9) == 9 && !memcmp(d, (u8[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9 }, 9));
    CHECK(run(d, "C0041", src, 9) == 5 && !memcmp(d, (u8[]){ 1, 2, 3, 4, 9 }, 5));
    CHECK(run(d, "A1", src, 9) == 7 && !memcmp(d, (u8[]){ 1, 2, 3, 4, 5, 6, 9 }, 7));
    /* 'S': string bytes without the NUL counted (it is written, though). */
    memset(d, 0xee, sizeof(d));
    CHECK(run(d, "S1", "abc", 7) == 4 && !memcmp(d, "abc\007", 4));
    /* 'L' sets the length for 'B'; 'l' also writes it as a byte. */
    CHECK(run(d, "LB", 3, src) == 3 && !memcmp(d, (u8[]){ 1, 2, 3 }, 3));
    CHECK(run(d, "lB", 2, src) == 3 && !memcmp(d, (u8[]){ 2, 1, 2 }, 3));
    CHECK(run(d, "LBB", 2, src, src + 4) == 4 && !memcmp(d, (u8[]){ 1, 2, 5, 6 }, 4));

    /* __vsprintf_len. */
    CHECK(measure(NULL) == 0);
    CHECK(measure("") == 0);
    CHECK(measure("1234H", 0, 0, 0, 0, 0) == 12);
    CHECK(measure("A", src) == 6);
    CHECK(measure("c05", src) == 5);
    CHECK(measure("C012", src) == 12);
    CHECK(measure("1c05", 0, src) == 6);
    CHECK(measure("L", 10) == 10);
    CHECK(measure("1L", 0, 10) == 11);
    CHECK(measure("l", 10) == 11);
    CHECK(measure("1lB", 0, 10, src) == 12);
    /* SDK quirk: 'b' sets the length instead of adding. */
    CHECK(measure("1b02", 0, 0, 0) == 2);
    CHECK(measure("b021", 0, 0, 0) == 3);
    /* SDK quirk: 'S' is not counted (and its argument is not consumed). */
    CHECK(measure("S", "abc") == 0);
    return TEST_DONE("btctrler_vsprintf");
}
#endif
