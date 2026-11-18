/* Export setting names and choices without board filtering.
 * Presentation text is maintained in the host catalog. */
#include <stdio.h>
#define SLOT_SWITCH2_WAKE 1
#include "../src/core/settings.c"

const struct board_slots board_slots = {0};
const struct board_led_brightness board_led_brightness = {0};
const struct board_rumble board_rumble = {0};
const struct set_remap board_remap_base = {0};

static void string(const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    putchar('"');
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void table(const char *kind, unsigned number, const struct set_field *fields, unsigned count, int *comma)
{
    unsigned i, j;
    for (i = 0; i < count; i++) {
        const struct set_field *f = fields + i;
        if ((*comma)++) puts(",");
        printf("  {\"kind\":%u,\"id\":%u,\"group\":", number, f->id);
        string(kind);
        printf(",\"name\":"); string(f->name);
        printf(",\"type\":%u,\"size\":%u,\"max\":%u,\"dev\":%s,\"default\":%u",
               f->type, f->size, f->max, f->flags & F_DEV ? "true" : "false", f->def);
        printf(",\"options\":[");
        for (j = 0; f->type == T_ENUM && f->names[j]; j++) {
            if (j) putchar(',');
            string(f->names[j]);
        }
        printf("]}");
    }
}

int main(void)
{
    int comma = 0;
    puts("[");
    table("slot", 0, slot_fields, COUNT(slot_fields), &comma);
    table("palette", 1, palette_fields, COUNT(palette_fields), &comma);
    table("calibration", 2, cal_fields, COUNT(cal_fields), &comma);
    table("remap", 3, remap_fields, COUNT(remap_fields), &comma);
    table("macros", 4, macro_fields, COUNT(macro_fields), &comma);
    puts("\n]");
    return 0;
}
