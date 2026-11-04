/* Export the real board's settings schema without the SDK or device I/O.
 * Unused settings operations are discarded by the host linker. */
#include "../../src/core/settings.c"
#include "board.inc"
#include "../../src/lib/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include "../../src/platform/telink/store_telink.h"

void util_put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }

static u8 schema[65536];
static size_t used;

static void append(const void *data, size_t size)
{
    if (size > sizeof schema - used) { fputs("settings schema too large\n", stderr); exit(1); }
    memcpy(schema + used, data, size);
    used += size;
}

static void word(u16 v)
{
    u8 bytes[2];
    util_put16(bytes, v);
    append(bytes, sizeof bytes);
}

/* All fields explicit is the largest persisted form, regardless of value.
 * Count every saved target and both halves of each remap record. */
static int storage_fits(void)
{
    u32 t, part, i, n, largest = 0, payload = 0, records = 0;
#ifdef SETTINGS_STORE_TELINK
    u32 live = TELINK_STORE_HEADER_SIZE;
#endif
#ifdef SETTINGS_STORE_JIELI_BYTES
    u32 jieli_live = 4;
#endif

    for (t = 0; t < TARGETS; t++) {
        const struct set_field *fields = fields_of(t, &n);
        if (n > 32) {
            fprintf(stderr, "settings target %u has %u fields, exceeds 32-bit field mask\n", t, n);
            return 0;
        }
        if (!stored(t)) continue;
        for (part = 0; part < parts_of(t); part++) {
            u32 bytes = 4;
            for (i = 0; i < n; i++) if (part_of(t, i) == part) bytes += 2u + fields[i].size;
            if (bytes > RECORD_MAX || bytes > 255) {
                fprintf(stderr, "settings target %u part %u needs %u bytes; record limit %u, format limit 255\n",
                        t, part, bytes, (unsigned)RECORD_MAX);
                return 0;
            }
#ifdef SETTINGS_STORE_TELINK
            if (bytes > TELINK_STORE_DATA_MAX || key_of(t, part) >= TELINK_STORE_RECEIVER_KEY) {
                fprintf(stderr, "settings target %u part %u exceeds Telink record/key limits\n", t, part);
                return 0;
            }
#endif
            if (bytes > largest) largest = bytes;
            payload += bytes;
#ifdef SETTINGS_STORE_TELINK
            live += TELINK_STORE_RECORD_SIZE(bytes);
#endif
#ifdef SETTINGS_STORE_JIELI_BYTES
            jieli_live += bytes + 4;
#endif
            records++;
        }
    }
    fprintf(stderr, "settings: %u records, %u payload bytes, largest %u/%u bytes\n",
            records, payload, largest, (unsigned)RECORD_MAX);
#ifdef SETTINGS_STORE_TELINK
    /* Reserve both optional records even when the build does not use them.
     * Compaction copies the old value before appending its replacement. */
    live += TELINK_STORE_RECORD_SIZE(TELINK_STORE_RECEIVER_SIZE) +
            TELINK_STORE_RECORD_SIZE(TELINK_STORE_LAST_SLOT_SIZE);
    if (largest < TELINK_STORE_RECEIVER_SIZE) largest = TELINK_STORE_RECEIVER_SIZE;
    if (largest < TELINK_STORE_LAST_SLOT_SIZE) largest = TELINK_STORE_LAST_SLOT_SIZE;
    live += TELINK_STORE_RECORD_SIZE(largest);
    fprintf(stderr, "settings: Telink live records plus overwrite reserve %u/%u flash bytes\n",
            live, (unsigned)TELINK_STORE_SECTOR_SIZE);
    if (live > TELINK_STORE_SECTOR_SIZE) {
        fputs("settings do not fit in one Telink compaction sector\n", stderr);
        return 0;
    }
#endif
#ifdef SETTINGS_STORE_JIELI_BYTES
    /* The reviewed SDK uses two equal areas: a 4-byte magic then unpadded
     * {4-byte item header, payload}. Compaction copies the old value before
     * its replacement is appended. An overflowing move erases both areas. */
    if (SETTINGS_STORE_JIELI_BYTES != 8192 && SETTINGS_STORE_JIELI_BYTES != 16384) {
        fputs("Jieli VM capacity model supports 8/16 KiB sector-mode regions only\n", stderr);
        return 0;
    }
    jieli_live += largest + 4 + SETTINGS_STORE_JIELI_RESERVE;
    fprintf(stderr, "settings: Jieli live records plus overwrite and %u-byte SDK reserve %u/%u flash bytes\n",
            (unsigned)SETTINGS_STORE_JIELI_RESERVE, jieli_live, (unsigned)(SETTINGS_STORE_JIELI_BYTES / 2));
    if (jieli_live > SETTINGS_STORE_JIELI_BYTES / 2) {
        fputs("settings do not fit in one Jieli VM compaction area\n", stderr);
        return 0;
    }
#endif
    return 1;
}

/* Every entry a known input and kind, no input twice. */
static int layout_ok(void)
{
    u64 seen = 0;
    u32 i;

    for (i = 0; i < board_layout_count; i++) {
        const struct board_control *c = &board_layout[i];
        u64 bit = 1ull << (c->input & 63);
        int known = c->input <= IN_R3 || (c->input >= IN_UP && c->input <= IN_R2) ||
                    c->input == IN_LSTICK || c->input == IN_RSTICK;
        if (!known || c->kind >= LAYOUT_KINDS || (seen & bit)) {
            fprintf(stderr, "board_layout entry %u: input %u kind %u is %s\n", i, c->input, c->kind,
                    !known ? "not an input" : c->kind >= LAYOUT_KINDS ? "not a kind" : "there twice");
            return 0;
        }
        seen |= bit;
    }
    return 1;
}

int main(int argc, char **argv)
{
    u8 field[4096], digest[32], info[JM_SCHEMA_INFO_SIZE] = {0};
    u32 n, count, t, i;
    FILE *out;

    if (argc != 3) { fputs("usage: settings-schema SCHEMA HEADER\n", stderr); return 1; }
    if (!storage_fits() || !layout_ok()) return 1;
    append("JMSC", 4);
    word(JM_SCHEMA_FORMAT);
    word(JM_PROTOCOL);
    word(0);                    /* filled with the field count below */
    for (count = 0; (n = set_describe(count, field, sizeof field)) != 0; count++) {
        word(n);
        append(field, n);
    }
    if (count != COUNT(slot_fields) + COUNT(palette_fields) + COUNT(cal_fields) + COUNT(remap_fields) ||
        count > 65535) return 1;
    util_put16(schema + 8, count);
    /* Factory values do not inherit the current user's base remap. Include
     * long defaults and writable targets, which FIELD alone cannot describe. */
    word(TARGETS);
    for (t = 0; t < TARGETS; t++) {
        const struct set_field *fields = fields_of(t, &n);
        u8 target[3] = {t, stored(t), n};
        append(target, sizeof target);
        for (i = 0; i < n; i++) {
            u8 item[2] = {fields[i].id, fields[i].size};
            own_default(t, &fields[i], field);
            append(item, sizeof item);
            append(field, fields[i].size);
        }
    }
    jm_sha256(schema, used, digest);
    info[0] = JM_SCHEMA_FORMAT;
    info[1] = JM_SCHEMA_HASH_SHA256_128;
    util_put16(info + 2, count);
    memcpy(info + JM_SCHEMA_DIGEST_OFF, digest, JM_SCHEMA_DIGEST_SIZE);
    out = fopen(argv[1], "wb");
    if (!out) return 1;
    if (fwrite(schema, 1, used, out) != used) { fclose(out); return 1; }
    if (fclose(out)) return 1;
    out = fopen(argv[2], "w");
    if (!out) return 1;
    fputs("/* Generated by the host settings schema exporter. */\n"
          "static const u8 settings_schema_info[JM_SCHEMA_INFO_SIZE] = {", out);
    for (i = 0; i < sizeof info; i++) fprintf(out, "%s0x%02x", i ? ", " : "", info[i]);
    fputs("};\n", out);
    if (ferror(out)) { fclose(out); return 1; }
    return fclose(out) != 0;
}
