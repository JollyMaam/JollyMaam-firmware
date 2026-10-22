/* Public Classic HCI device settings, recovered from SDK lmp_hci.c. */
#include <string.h>
#include "btctrler/glue.h"

/* The SDK's local device defaults (lmp_hci.c).  The host overwrites name,
 * class and timeouts at power-on; drift and jitter (ppm, us) answer timing
 * accuracy requests. */
__attribute__((section(".classic_lmp_data"))) struct bredr_local_device gbredr_local_dev = {
    .drift = 250, .jitter = 10,
    .local_name = "JL-BR22",
    .priv_version = { 'b', 'r', '2', '2', 'x', 'x', 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xaa, 0xbb },
    .super_timeout_value = 8000, .page_timeout_value = 8000,
    .eir = { .sr = 1, .enable = 1, .type = 3 },
    .internal_addr_local = { 0xcb, 0x35, 0x3a, 0x53, 0x98, 0xd7 },
    .internal_addr_remote = { 0x80, 0xba, 0xa3, 0x28, 0x97, 0xad },
};

void lmp_hci_write_class_of_device(int class)
{ gbredr_local_dev.dev_class = class; }
void lmp_hci_write_page_timeout(u16 timeout)
{ gbredr_local_dev.page_timeout_value = timeout; }
void lmp_hci_write_super_timeout(u16 timeout)
{ gbredr_local_dev.super_timeout_value = timeout; }
void lmp_hci_write_connection_accept_timeout(u16 timeout)
{ gbredr_local_dev.connection_accept_timeout_value = timeout; }
int lmp_hci_read_pin_type(void) { return gbredr_local_dev.pin_code_type; }
int lmp_hci_write_pin_type(u8 type)
{ gbredr_local_dev.pin_code_type = type; return type; }
int lmp_hci_write_le_host_support(int features)
{ (void)features; return 0; } /* SDK no-op. */
void lmp_hci_host_num_of_completed_packets(u16 handle, u16 num_of_completed_packet)
{ lmp_hci_cmd_to_conn_for_handle(handle, 2, 0x0c35, num_of_completed_packet); }

void lmp_hci_write_local_name(const char *name)
{
    unsigned size = sizeof(gbredr_local_dev.local_name), len = 0;
    /* Fix: avoid the SDK's 16-bit strlen truncation and copying after a
     * failed assertion. A terminator must fit in the 32-byte name field. */
    if (name) while (len < size && name[len]) len++;
    if (!name || len == size) {
        btctrler_assert_failed("local name length", len);
        return;
    }
    memmove(gbredr_local_dev.local_name, name, len + 1);
    memset(gbredr_local_dev.local_name + len + 1, 0, size - len - 1);
}
void lmp_hci_write_local_address(const u8 *addr)
{
    memcpy(hci_param.addr, addr, sizeof(hci_param.addr));
    /* Preserve the SDK checksum's accumulation across repeated writes. */
    for (unsigned i = 0; i < sizeof(hci_param.addr); i++) hci_param.addr_sum += hci_param.addr[i];
}
void lmp_hci_read_local_address(u8 *addr)
{ if (addr) memcpy(addr, hci_param.addr, sizeof(hci_param.addr)); }
void lmp_hci_write_simple_pairing_mode(u8 enable)
{
    if (enable) { hci_param.features[6] |= 8; hci_param.ext_features[0] |= 1; }
    else { hci_param.features[6] &= ~8; hci_param.ext_features[0] &= ~1; }
}
void lmp_hci_set_role_switch_supported(bool enable)
{
    if (enable) hci_param.features[0] |= 0x20;
    else hci_param.features[0] &= ~0x20;
}
u8 lmp_hci_read_local_supported_features(int octet)
{
    /* Fix: SDK callers could index either side of the feature array. */
    if ((unsigned)octet >= sizeof(hci_param.features)) return 0;
    return hci_param.features[octet];
}
void lmp_hci_write_local_supported_features(u8 features, int octet)
{
    if ((unsigned)octet < sizeof(hci_param.features)) hci_param.features[octet] = features;
}

#ifdef UNITTEST
#include <limits.h>
#include <stdarg.h>
#include "test.h"
struct hci_parameter hci_param;
static int asserts, assert_value, queued;
void btctrler_assert_failed(const char *what, int value)
{ CHECK(what); asserts++; assert_value = value; }
int lmp_hci_cmd_to_conn_for_handle(u16 handle, int argc, ...)
{
    CHECK(handle == 0x123 && argc == 2);
    va_list ap; va_start(ap, argc);
    CHECK(va_arg(ap, int) == 0x0c35 && va_arg(ap, int) == 0xffff);
    va_end(ap); queued++; return -1;
}
int main(void)
{
    lmp_hci_write_class_of_device(0x12345678); CHECK(gbredr_local_dev.dev_class == 0x12345678);
    lmp_hci_write_page_timeout(0xffff); lmp_hci_write_super_timeout(0x8000);
    lmp_hci_write_connection_accept_timeout(0xabcd);
    CHECK(gbredr_local_dev.page_timeout_value == 0xffff && gbredr_local_dev.super_timeout_value == 0x8000 &&
          gbredr_local_dev.connection_accept_timeout_value == 0xabcd);
    gbredr_local_dev.authentication_enable = 1; gbredr_local_dev.inquiry_mode = 3;
    gbredr_local_dev.page_scan_type = gbredr_local_dev.inquiry_scan_type = 1;
    for (int type = 0; type < 256; type++) {
        CHECK(lmp_hci_write_pin_type(type) == type && lmp_hci_read_pin_type() == (type & 1));
        CHECK(gbredr_local_dev.authentication_enable && gbredr_local_dev.page_scan_type &&
              gbredr_local_dev.inquiry_scan_type && gbredr_local_dev.inquiry_mode == 3);
    }
    CHECK(lmp_hci_write_le_host_support(INT_MIN) == 0 && lmp_hci_write_le_host_support(INT_MAX) == 0);
    lmp_hci_host_num_of_completed_packets(0x123, 0xffff); CHECK(queued == 1);
    for (unsigned len = 0; len < 32; len++) {
        char name[32]; memset(name, 'A', len); name[len] = 0;
        memset(gbredr_local_dev.local_name, 0xa5, 32);
        lmp_hci_write_local_name(name);
        CHECK(!asserts && !memcmp(gbredr_local_dev.local_name, name, len + 1));
        for (unsigned i = len; i < 32; i++) CHECK(!gbredr_local_dev.local_name[i]);
    }
    lmp_hci_write_local_name((const char *)gbredr_local_dev.local_name);
    CHECK(gbredr_local_dev.local_name[0] == 'A' && gbredr_local_dev.local_name[30] == 'A' &&
          !gbredr_local_dev.local_name[31]);
    /* Exact-length unterminated storage and null input must leave state intact. */
    char unterminated[32]; memset(unterminated, 'B', 32);
    u8 saved[32]; memcpy(saved, gbredr_local_dev.local_name, 32);
    lmp_hci_write_local_name(unterminated);
    CHECK(asserts == 1 && assert_value == 32 && !memcmp(saved, gbredr_local_dev.local_name, 32));
    lmp_hci_write_local_name(0);
    CHECK(asserts == 2 && !assert_value && !memcmp(saved, gbredr_local_dev.local_name, 32));
    u8 addr[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}, read[6];
    hci_param.addr_sum = 0xf0;
    for (int pass = 0; pass < 2; pass++) {
        u8 sum = hci_param.addr_sum;
        for (unsigned i = 0; i < 6; i++) sum += addr[i];
        lmp_hci_write_local_address(addr); CHECK(hci_param.addr_sum == sum);
        lmp_hci_read_local_address(read); CHECK(!memcmp(addr, read, 6));
    }
    lmp_hci_read_local_address(0);
    for (int enable = 0; enable < 256; enable++) {
        memset(hci_param.features, 0xa5, sizeof(hci_param.features));
        memset(hci_param.ext_features, 0xa5, sizeof(hci_param.ext_features));
        lmp_hci_write_simple_pairing_mode(enable);
        CHECK(hci_param.features[6] == (enable ? 0xad : 0xa5) &&
              hci_param.ext_features[0] == (enable ? 0xa5 : 0xa4));
        for (unsigned i = 0; i < sizeof(hci_param.features); i++) if (i != 6) CHECK(hci_param.features[i] == 0xa5);
        for (unsigned i = 1; i < sizeof(hci_param.ext_features); i++) CHECK(hci_param.ext_features[i] == 0xa5);
    }
    for (int enable = 0; enable < 2; enable++) {
        hci_param.features[0] = 0xd5; lmp_hci_set_role_switch_supported(enable);
        CHECK(hci_param.features[0] == (enable ? 0xf5 : 0xd5));
    }
    for (int i = 0; i < 8; i++) {
        lmp_hci_write_local_supported_features(0x70 + i, i);
        CHECK(lmp_hci_read_local_supported_features(i) == 0x70 + i);
    }
    struct hci_parameter before = hci_param;
    const int invalid[] = {INT_MIN, -1, 8, 9, INT_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        CHECK(!lmp_hci_read_local_supported_features(invalid[i]));
        lmp_hci_write_local_supported_features(0xff, invalid[i]);
        CHECK(!memcmp(&hci_param, &before, sizeof(before)));
    }
    return TEST_DONE("btctrler_lmp_hci_settings");
}
#endif
