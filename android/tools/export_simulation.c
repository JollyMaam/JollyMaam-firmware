/* Export actual settings metadata/defaults without a controller or SDK.
 * Reuse settings.c's host-test storage/platform stubs, but not its test main. */
#define UNITTEST
#define main settings_test_main
#include "../../src/core/settings.c"
#undef main

static void hex_json(const u8 *data, u32 size)
{
    putchar('"');
    for (u32 i = 0; i < size; i++) printf("%02x", data[i]);
    putchar('"');
}

int main(void)
{
    u8 data[512];
    u32 n, size;

    set_init();
    printf("{\n  \"format\": 1,\n  \"fields\": [\n");
    for (n = 0; (size = set_describe(n, data, sizeof(data))); n++) {
        if (n) printf(",\n");
        printf("    "); hex_json(data, size);
    }
    printf("\n  ],\n  \"defaults\": [\n");
    for (n = 0; n < TARGETS; n++) {
        if (n) printf(",\n");
        printf("    "); hex_json(data, set_read_defaults((u8)n, data, sizeof(data)));
    }
    printf("\n  ],\n  \"values\": [\n");
    for (n = 0; n < TARGETS; n++) {
        if (n) printf(",\n");
        printf("    "); hex_json(data, set_read((u8)n, data, sizeof(data)));
    }
    printf("\n  ]\n}\n");
    return 0;
}
