/* The SDK's small utility functions (btcontroller_util.c, a stripped copy of
 * BTstack's btstack_util): little-endian accessors, byte reversal, the
 * address-to-text helper, the RFCOMM-style CRC-8 and a few helpers.  Only the
 * functions something else still calls are carried over.  Kept as the SDK has
 * them: btctler_bd_addr_to_str returns one shared static buffer and its last
 * ':' is overwritten by the terminator; btctler_crc8_check returns 0 for a
 * match (like BTstack's btstack_crc8_check: crc8 ^ check_sum == 0xff is the
 * same as BTstack's table lookup comparing against 0xcf); the CRC-8 is not
 * complemented; btctler_get_rand_from_assign_range returns max when max <=
 * min and takes the remainder unsigned. BR23 keeps its recovered subset; the
 * CONFIG_CPU_BD19 block below supplies the rest of the BD19 object member. */
#ifdef CONFIG_CPU_BD19
#include <string.h>
#endif
#include "btctrler/glue.h"

/* The 07.10 / RFCOMM FCS table: CRC-8, polynomial x^8 + x^2 + x + 1, reflected. */
static const u8 crc8table[256] = {
    0x00, 0x91, 0xe3, 0x72, 0x07, 0x96, 0xe4, 0x75, 0x0e, 0x9f, 0xed, 0x7c, 0x09, 0x98, 0xea, 0x7b,
    0x1c, 0x8d, 0xff, 0x6e, 0x1b, 0x8a, 0xf8, 0x69, 0x12, 0x83, 0xf1, 0x60, 0x15, 0x84, 0xf6, 0x67,
    0x38, 0xa9, 0xdb, 0x4a, 0x3f, 0xae, 0xdc, 0x4d, 0x36, 0xa7, 0xd5, 0x44, 0x31, 0xa0, 0xd2, 0x43,
    0x24, 0xb5, 0xc7, 0x56, 0x23, 0xb2, 0xc0, 0x51, 0x2a, 0xbb, 0xc9, 0x58, 0x2d, 0xbc, 0xce, 0x5f,
    0x70, 0xe1, 0x93, 0x02, 0x77, 0xe6, 0x94, 0x05, 0x7e, 0xef, 0x9d, 0x0c, 0x79, 0xe8, 0x9a, 0x0b,
    0x6c, 0xfd, 0x8f, 0x1e, 0x6b, 0xfa, 0x88, 0x19, 0x62, 0xf3, 0x81, 0x10, 0x65, 0xf4, 0x86, 0x17,
    0x48, 0xd9, 0xab, 0x3a, 0x4f, 0xde, 0xac, 0x3d, 0x46, 0xd7, 0xa5, 0x34, 0x41, 0xd0, 0xa2, 0x33,
    0x54, 0xc5, 0xb7, 0x26, 0x53, 0xc2, 0xb0, 0x21, 0x5a, 0xcb, 0xb9, 0x28, 0x5d, 0xcc, 0xbe, 0x2f,
    0xe0, 0x71, 0x03, 0x92, 0xe7, 0x76, 0x04, 0x95, 0xee, 0x7f, 0x0d, 0x9c, 0xe9, 0x78, 0x0a, 0x9b,
    0xfc, 0x6d, 0x1f, 0x8e, 0xfb, 0x6a, 0x18, 0x89, 0xf2, 0x63, 0x11, 0x80, 0xf5, 0x64, 0x16, 0x87,
    0xd8, 0x49, 0x3b, 0xaa, 0xdf, 0x4e, 0x3c, 0xad, 0xd6, 0x47, 0x35, 0xa4, 0xd1, 0x40, 0x32, 0xa3,
    0xc4, 0x55, 0x27, 0xb6, 0xc3, 0x52, 0x20, 0xb1, 0xca, 0x5b, 0x29, 0xb8, 0xcd, 0x5c, 0x2e, 0xbf,
    0x90, 0x01, 0x73, 0xe2, 0x97, 0x06, 0x74, 0xe5, 0x9e, 0x0f, 0x7d, 0xec, 0x99, 0x08, 0x7a, 0xeb,
    0x8c, 0x1d, 0x6f, 0xfe, 0x8b, 0x1a, 0x68, 0xf9, 0x82, 0x13, 0x61, 0xf0, 0x85, 0x14, 0x66, 0xf7,
    0xa8, 0x39, 0x4b, 0xda, 0xaf, 0x3e, 0x4c, 0xdd, 0xa6, 0x37, 0x45, 0xd4, 0xa1, 0x30, 0x42, 0xd3,
    0xb4, 0x25, 0x57, 0xc6, 0xb3, 0x22, 0x50, 0xc1, 0xba, 0x2b, 0x59, 0xc8, 0xbd, 0x2c, 0x5e, 0xcf,
};

/* These APIs are present in the complete BD19 utility member but were not
 * needed by the recovered BR23 source set. */
#ifdef CONFIG_CPU_BD19
const uint8_t btctler_bluetooth_base_uuid[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb,
};
static char uuid128_to_str_buffer[37];
static const uint32_t crc32Table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};
#endif

static char bd_addr_to_str_buffer[18];

uint16_t btctler_little_endian_read_16(const uint8_t *buffer, int pos)
{
    return (uint16_t)(buffer[pos + 1] << 8 | buffer[pos]);
}

uint32_t btctler_little_endian_read_24(const uint8_t *buffer, int pos)
{
    return (uint32_t)buffer[pos + 1] << 8 | buffer[pos] | (uint32_t)buffer[pos + 2] << 16;
}

uint32_t btctler_little_endian_read_32(const uint8_t *buffer, int pos)
{
    return (uint32_t)buffer[pos + 1] << 8 | buffer[pos] | (uint32_t)buffer[pos + 2] << 16 | (uint32_t)buffer[pos + 3] << 24;
}

void btctler_little_endian_store_16(uint8_t *buffer, uint16_t pos, uint16_t value)
{
#ifdef CONFIG_CPU_BD19
    uint16_t p = pos;
    buffer[p] = (uint8_t)value; p++;
    buffer[p] = (uint8_t)(value >> 8);
#else
    buffer[pos] = (uint8_t)value;
    buffer[pos + 1] = (uint8_t)(value >> 8);
#endif
}

void btctler_reverse_bytes(const uint8_t *src, uint8_t *dst, int len)
{
    for (int i = 0; i < len; i++) dst[len - 1 - i] = src[i];
}

void btctler_reverse_128(const uint8_t *src, uint8_t *dst)
{
    btctler_reverse_bytes(src, dst, 16);
}

char btctler_char_for_nibble(int nibble)
{
    if (nibble < 10) return (char)(nibble + 48);
    if (nibble - 10 < 6) return (char)(nibble + 55);
    return 63;
}

static char btctler_char_for_high_nibble(int value) { return btctler_char_for_nibble((value >> 4) & 15); }
static char btctler_char_for_low_nibble(int value) { return btctler_char_for_nibble(value & 15); }

/* "AA:BB:CC:DD:EE:FF" of a 6-byte address stored least significant byte
 * first, in a buffer shared by all callers. */
char *btctler_bd_addr_to_str(const uint8_t *addr)
{
    char *p = bd_addr_to_str_buffer;
    for (int i = 6; i > 0; i--) {
        int value = addr[i - 1];
        *p = btctler_char_for_high_nibble(value);
        p[1] = btctler_char_for_low_nibble(value);
        p[2] = ':';
        p += 3;
    }
    bd_addr_to_str_buffer[17] = 0;
    return bd_addr_to_str_buffer;
}

static uint8_t crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0xff;
    for (uint16_t i = 0; i != len; i++) crc = crc8table[data[i] ^ crc];
    return crc;
}

uint8_t btctler_crc8(uint8_t *data, uint16_t len)
{
    return crc8(data, len);
}

uint8_t btctler_crc8_check(uint8_t *data, uint16_t len, uint8_t check_sum)
{
    return (uint8_t)((crc8(data, len) ^ check_sum) != 0xff);
}

uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min, uint32_t max)
{
    if (max > min) return rand % (max - min) + min;
    return max;
}

/* 1 if any byte is non-zero, 0 if all are zero. */
uint8_t btctler_memcmp_zero(const void *target, uint32_t size)
{
    const uint8_t *p = target;
    while (size != 0) {
        size--;
        if (*p++ != 0) return 1;
    }
    return 0;
}

#ifdef CONFIG_CPU_BD19
int btctler_bd_addr_cmp(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6);
}

void btctler_bd_addr_copy(uint8_t *dest, const uint8_t *src)
{
    __builtin_memcpy(dest, src, 6);
}

uint32_t btctler_big_endian_read_16(const uint8_t *buffer, int pos)
{
    return ((uint32_t)buffer[pos] << 8) | buffer[pos + 1];
}

uint32_t btctler_big_endian_read_24(const uint8_t *buffer, int pos)
{
    return ((uint32_t)buffer[pos] << 16) |
        ((uint32_t)buffer[pos + 1] << 8) | buffer[pos + 2];
}

uint32_t btctler_big_endian_read_32(const uint8_t *buffer, int pos)
{
    return ((uint32_t)buffer[pos] << 24) |
        ((uint32_t)buffer[pos + 1] << 16) |
        ((uint32_t)buffer[pos + 2] << 8) | buffer[pos + 3];
}

void btctler_little_endian_store_24(uint8_t *buffer, uint16_t pos, uint32_t value)
{
    uint16_t p = pos;
    buffer[p] = (uint8_t)value; p++;
    buffer[p] = (uint8_t)(value >> 8); p++;
    buffer[p] = (uint8_t)(value >> 16);
}

void btctler_little_endian_store_32(uint8_t *buffer, uint16_t pos, uint32_t value)
{
    uint16_t p = pos;
    buffer[p] = (uint8_t)value; p++;
    buffer[p] = (uint8_t)(value >> 8); p++;
    buffer[p] = (uint8_t)(value >> 16); p++;
    buffer[p] = (uint8_t)(value >> 24);
}

void btctler_big_endian_store_16(uint8_t *buffer, uint16_t pos, uint16_t value)
{
    uint16_t p = pos;
    buffer[p] = (uint8_t)(value >> 8); p++;
    buffer[p] = (uint8_t)value;
}

void btctler_big_endian_store_24(uint8_t *buffer, uint16_t pos, uint32_t value)
{
    uint16_t p = pos;
    buffer[p] = (uint8_t)(value >> 16); p++;
    buffer[p] = (uint8_t)(value >> 8); p++;
    buffer[p] = (uint8_t)value;
}

void btctler_big_endian_store_32(uint8_t *buffer, uint16_t pos, uint32_t value)
{
    uint16_t p = pos;
    buffer[p] = (uint8_t)(value >> 24); p++;
    buffer[p] = (uint8_t)(value >> 16); p++;
    buffer[p] = (uint8_t)(value >> 8); p++;
    buffer[p] = (uint8_t)value;
}

void btctler_reverse_24(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 3); }
void btctler_reverse_32(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 4); }
void btctler_reverse_48(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 6); }
void btctler_reverse_56(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 7); }
void btctler_reverse_64(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 8); }
void btctler_reverse_256(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 32); }
void btctler_reverse_bd_addr(const uint8_t *src, uint8_t *dst) { btctler_reverse_bytes(src, dst, 6); }

uint32_t btctler_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
uint32_t btctler_max(uint32_t a, uint32_t b) { return a > b ? a : b; }

int btctler_nibble_for_char(char c)
{
    uint8_t byte = (uint8_t)c;
    uint8_t digit = (uint8_t)(byte - (uint8_t)'0');
    if (digit < 10) return digit;
    digit = (uint8_t)(byte - (uint8_t)'a');
    if (digit < 6) return digit + 10;
    digit = (uint8_t)(byte - (uint8_t)'A');
    if (digit < 6) return digit + 10;
    return -1;
}

void btctler_printf_hexdump(const void *data, int size)
{
    (void)data; (void)size;
}

void btctler_log_info_hexdump(const void *data, int size)
{
    (void)data; (void)size;
}

void btctler_log_info_key(const char *name, uint8_t *key)
{
    (void)name; (void)key;
}

void btctler_uuid_add_bluetooth_prefix(uint8_t *uuid, uint32_t shortUUID)
{
    __builtin_memcpy(uuid, btctler_bluetooth_base_uuid, 16);
    uuid[0] = (uint8_t)(shortUUID >> 24);
    uuid[1] = (uint8_t)(shortUUID >> 16);
    uuid[2] = (uint8_t)(shortUUID >> 8);
    uuid[3] = (uint8_t)shortUUID;
}

int btctler_uuid_has_bluetooth_prefix(const uint8_t *uuid128)
{
    return memcmp(uuid128 + 4, btctler_bluetooth_base_uuid + 4, 12) == 0;
}

char *btctler_uuid128_to_str(const uint8_t *uuid)
{
    int out = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t byte = uuid[i];
        uuid128_to_str_buffer[out++] = btctler_char_for_nibble(byte >> 4);
        uuid128_to_str_buffer[out++] = btctler_char_for_nibble(byte & 15);
        if ((1u << i) & 680u) uuid128_to_str_buffer[out++] = '-';
    }
    return uuid128_to_str_buffer;
}

int btctler_sscanf_bd_addr(const char *addr_string, uint8_t *addr)
{
    uint8_t parsed[6];
    const uint8_t *p = (const uint8_t *)addr_string;
    for (int i = 0; i < 6; i++) {
        int high = btctler_nibble_for_char((char)*p);
        if (high < 0) return 0;
        int low = btctler_nibble_for_char((char)p[1]);
        int value = low | (high << 4);
        if (value < 0) return 0;
        parsed[i] = (uint8_t)value;
        if (i == 5) {
            __builtin_memcpy(addr, parsed, sizeof(parsed));
            return 1;
        }
        if (p[2] != ':' && p[2] != '-' && p[2] != ' ') return 0;
        p += 3;
    }
    return 0;
}

uint32_t btctler_atoi(const char *str)
{
    uint32_t value = 0;
    while (1) {
        uint8_t digit = (uint8_t)(*str - '0');
        if (digit > 9) break;
        value = value * 10 + digit;
        str++;
    }
    return value;
}

int btctler_string_len_for_uint32(uint32_t value)
{
    if (value < 10u) return 1;
    if (value < 100u) return 2;
    if (value < 1000u) return 3;
    if (value < 10000u) return 4;
    if (value < 100000u) return 5;
    if (value < 1000000u) return 6;
    if (value < 10000000u) return 7;
    if (value < 100000000u) return 8;
    return value < 1000000000u ? 9 : 10;
}

int btctler_count_set_bits_uint32(uint32_t value)
{
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0f0f0f0fu;
    value += value >> 8;
    value += value >> 16;
    return (int)(value & 0x3fu);
}

uint8_t btctler_crc8_calc(uint8_t *data, uint16_t len)
{
    return (uint8_t)(crc8(data, len) ^ 0xffu);
}

uint32_t btctler_CalcCrc32(uint32_t crc, uint32_t len, const uint8_t *buffer)
{
    while (len != 0) {
        uint32_t index = (crc ^ *buffer++) & 0xffu;
        crc = crc32Table[index] ^ (crc >> 8);
        len--;
    }
    return crc ^ 0xffffffffu;
}
#endif

#ifdef UNITTEST
#include <string.h>
#include "test.h"
int main(void)
{
    static const uint8_t b[8] = { 0x11, 0x22, 0x83, 0xf4, 0x05, 0x66, 0x77, 0x88 };
    /* little-endian reads at an offset */
    CHECK(btctler_little_endian_read_16(b, 0) == 0x2211);
    CHECK(btctler_little_endian_read_16(b, 2) == 0xf483);
    CHECK(btctler_little_endian_read_24(b, 1) == 0xf48322);
    CHECK(btctler_little_endian_read_32(b, 0) == 0xf4832211u);
    CHECK(btctler_little_endian_read_32(b, 3) == 0x776605f4u);
    CHECK(btctler_little_endian_read_32(b, 4) == 0x88776605u);
    uint8_t out[6] = { 0xee, 0xee, 0xee, 0xee, 0xee, 0xee };
    btctler_little_endian_store_16(out, 1, 0xa1b2);
    CHECK(out[0] == 0xee && out[1] == 0xb2 && out[2] == 0xa1 && out[3] == 0xee);
    btctler_little_endian_store_16(out, 0, 0x0102);
    CHECK(out[0] == 2 && out[1] == 1);
    /* reverse */
    uint8_t rev[16], src[16];
    for (int i = 0; i < 16; i++) src[i] = i;
    btctler_reverse_128(src, rev);
    for (int i = 0; i < 16; i++) CHECK(rev[i] == 15 - i);
    memset(rev, 0xee, 16);
    btctler_reverse_bytes(src, rev, 3);
    CHECK(rev[0] == 2 && rev[1] == 1 && rev[2] == 0 && rev[3] == 0xee);
    btctler_reverse_bytes(src, rev, 0); btctler_reverse_bytes(src, rev, -4);
    CHECK(rev[0] == 2 && rev[3] == 0xee);
    btctler_reverse_bytes(src, rev, 1); CHECK(rev[0] == 0);
    /* nibbles: digits, upper-case letters, '?' beyond 15 (and below 0 the digit rule applies) */
    CHECK(btctler_char_for_nibble(0) == '0' && btctler_char_for_nibble(9) == '9');
    CHECK(btctler_char_for_nibble(10) == 'A' && btctler_char_for_nibble(15) == 'F');
    CHECK(btctler_char_for_nibble(16) == '?' && btctler_char_for_nibble(255) == '?');
    CHECK(btctler_char_for_nibble(-1) == '/');
    /* address text: bytes printed from the last to the first */
    {
        static const uint8_t a[6] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab };
        char *s = btctler_bd_addr_to_str(a);
        CHECK(!strcmp(s, "AB:89:67:45:23:01") && strlen(s) == 17);
        static const uint8_t z[6] = { 0 };
        char *t = btctler_bd_addr_to_str(z);
        CHECK(t == s && !strcmp(s, "00:00:00:00:00:00"));      /* one shared buffer */
        static const uint8_t f[6] = { 0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa };
        CHECK(!strcmp(btctler_bd_addr_to_str(f), "FA:FB:FC:FD:FE:FF"));
    }
    /* CRC-8: the 07.10 frames, F9 03 3F 01 1C F9 (SABM, DLCI 0) and F9 03 73 01 D7 F9 (UA), FCS = 0xff - crc */
    {
        uint8_t sabm[3] = { 0x03, 0x3f, 0x01 }, ua[3] = { 0x03, 0x73, 0x01 };
        CHECK(btctler_crc8(sabm, 3) == (uint8_t)(0xff - 0x1c));
        CHECK(btctler_crc8(ua, 3) == (uint8_t)(0xff - 0xd7));
        CHECK(btctler_crc8_check(sabm, 3, 0x1c) == 0);
        CHECK(btctler_crc8_check(ua, 3, 0xd7) == 0);
        CHECK(btctler_crc8_check(sabm, 3, 0xd7) == 1 && btctler_crc8_check(sabm, 3, 0x1d) == 1);
        CHECK(btctler_crc8(sabm, 0) == 0xff);
        /* one byte i from the initial 0xff goes through table[i ^ 0xff] */
        for (int i = 0; i < 256; i++) {
            uint8_t one = (uint8_t)i;
            CHECK(btctler_crc8(&one, 1) == crc8table[i ^ 0xff]);
        }
        /* the table against the bitwise definition: MSB-first CRC-8 (x^8+x^2+x+1) over
         * bit-reversed input and output */
        for (int i = 0; i < 256; i++) {
            int in = 0, c;
            for (int k = 0; k < 8; k++) in |= ((i >> k) & 1) << (7 - k);
            c = in;
            for (int k = 0; k < 8; k++) c = c & 0x80 ? ((c << 1) ^ 0x07) & 0xff : (c << 1) & 0xff;
            int r = 0;
            for (int k = 0; k < 8; k++) r |= ((c >> k) & 1) << (7 - k);
            CHECK(crc8table[i] == r);
        }
    }
    /* range */
    CHECK(btctler_get_rand_from_assign_range(100, 10, 20) == 10);
    CHECK(btctler_get_rand_from_assign_range(107, 10, 20) == 17);
    CHECK(btctler_get_rand_from_assign_range(0xffffffffu, 5, 8) == 0xffffffffu % 3 + 5);
    CHECK(btctler_get_rand_from_assign_range(7, 20, 20) == 20);
    CHECK(btctler_get_rand_from_assign_range(7, 30, 20) == 20);
    CHECK(btctler_get_rand_from_assign_range(7, 0, 1) == 0);
    /* memcmp_zero */
    {
        uint8_t z[5] = { 0, 0, 0, 0, 0 };
        CHECK(btctler_memcmp_zero(z, 5) == 0 && btctler_memcmp_zero(z, 0) == 0);
        z[4] = 1; CHECK(btctler_memcmp_zero(z, 5) == 1 && btctler_memcmp_zero(z, 4) == 0);
        z[0] = 0x80; CHECK(btctler_memcmp_zero(z, 1) == 1);
    }
#ifdef CONFIG_CPU_BD19
    /* BD19 exports the full utility member. Values and wrap cases follow the
     * original i16 offsets, unsigned comparisons and table-based CRC IR. */
    {
        static const uint8_t a[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
        static const uint8_t b[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xac};
        uint8_t copy[6] = {0};
        CHECK(btctler_bd_addr_cmp(a, a) == 0);
        CHECK(btctler_bd_addr_cmp(a, b) != 0);
        btctler_bd_addr_copy(copy, a);
        CHECK(memcmp(copy, a, sizeof(a)) == 0);
    }
    {
        static const uint8_t data[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        CHECK(btctler_big_endian_read_16(data, 1) == 0x2233);
        CHECK(btctler_big_endian_read_24(data, 2) == 0x334455);
        CHECK(btctler_big_endian_read_32(data, 3) == 0x44556677);
        uint8_t out[8]; memset(out, 0xee, sizeof(out));
        btctler_little_endian_store_24(out, 1, 0xa1b2c3);
        CHECK(out[0] == 0xee && out[1] == 0xc3 && out[2] == 0xb2 && out[3] == 0xa1 && out[4] == 0xee);
        btctler_little_endian_store_32(out, 1, 0x12345678);
        CHECK(out[1] == 0x78 && out[2] == 0x56 && out[3] == 0x34 && out[4] == 0x12);
        btctler_big_endian_store_16(out, 1, 0xa1b2);
        CHECK(out[1] == 0xa1 && out[2] == 0xb2);
        btctler_big_endian_store_24(out, 1, 0xa1b2c3);
        CHECK(out[1] == 0xa1 && out[2] == 0xb2 && out[3] == 0xc3);
        btctler_big_endian_store_32(out, 1, 0x12345678);
        CHECK(out[1] == 0x12 && out[2] == 0x34 && out[3] == 0x56 && out[4] == 0x78);
    }
    {
        static uint8_t wrap[65536];
        btctler_little_endian_store_16(wrap, 0xffff, 0x1234);
        CHECK(wrap[0xffff] == 0x34 && wrap[0] == 0x12);
        memset(wrap, 0, sizeof(wrap));
        btctler_little_endian_store_24(wrap, 0xffff, 0x112233);
        CHECK(wrap[0xffff] == 0x33 && wrap[0] == 0x22 && wrap[1] == 0x11);
        memset(wrap, 0, sizeof(wrap));
        btctler_big_endian_store_32(wrap, 0xffff, 0x12345678);
        CHECK(wrap[0xffff] == 0x12 && wrap[0] == 0x34 && wrap[1] == 0x56 && wrap[2] == 0x78);
    }
    {
        uint8_t src[32], rev[32];
        for (int i = 0; i < 32; i++) src[i] = (uint8_t)i;
        memset(rev, 0xee, sizeof(rev));
        btctler_reverse_24(src, rev); CHECK(rev[0] == 2 && rev[2] == 0 && rev[3] == 0xee);
        btctler_reverse_32(src, rev); CHECK(rev[0] == 3 && rev[3] == 0);
        btctler_reverse_48(src, rev); CHECK(rev[0] == 5 && rev[5] == 0);
        btctler_reverse_56(src, rev); CHECK(rev[0] == 6 && rev[6] == 0);
        btctler_reverse_64(src, rev); CHECK(rev[0] == 7 && rev[7] == 0);
        btctler_reverse_256(src, rev); /* fixed-width call reads 32 bytes */
        /* Repeat at the supported 32-byte boundary with a sufficiently sized source/destination. */
        uint8_t wide_src[32], wide_dst[32];
        for (int i = 0; i < 32; i++) wide_src[i] = (uint8_t)i;
        btctler_reverse_256(wide_src, wide_dst);
        CHECK(wide_dst[0] == 31 && wide_dst[31] == 0);
        btctler_reverse_bd_addr(src, rev); CHECK(rev[0] == 5 && rev[5] == 0);
    }
    CHECK(btctler_min(0xffffffffu, 0x80000000u) == 0x80000000u);
    CHECK(btctler_max(0xffffffffu, 0x80000000u) == 0xffffffffu);
    CHECK(btctler_nibble_for_char('0') == 0 && btctler_nibble_for_char('9') == 9);
    CHECK(btctler_nibble_for_char('a') == 10 && btctler_nibble_for_char('F') == 15);
    CHECK(btctler_nibble_for_char('g') == -1 && btctler_nibble_for_char((char)0x80) == -1);
    {
        uint8_t uuid[16];
        static const uint8_t expected_tail[12] = {
            0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
        };
        btctler_uuid_add_bluetooth_prefix(uuid, 0x11223344);
        CHECK(uuid[0] == 0x11 && uuid[1] == 0x22 && uuid[2] == 0x33 && uuid[3] == 0x44);
        CHECK(memcmp(uuid + 4, expected_tail, sizeof(expected_tail)) == 0);
        CHECK(btctler_uuid_has_bluetooth_prefix(uuid));
        uuid[0] ^= 0xff; CHECK(btctler_uuid_has_bluetooth_prefix(uuid));
        uuid[15] ^= 1; CHECK(!btctler_uuid_has_bluetooth_prefix(uuid));
        static const uint8_t text_uuid[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };
        char *text = btctler_uuid128_to_str(text_uuid);
        CHECK(!strcmp(text, "00112233-4455-6677-8899-AABBCCDDEEFF") && strlen(text) == 36);
        CHECK(btctler_uuid128_to_str(text_uuid) == text); /* one shared output buffer */
    }
    {
        uint8_t addr[6] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
        CHECK(btctler_sscanf_bd_addr("01:23-45 67:89-ab", addr) == 1);
        static const uint8_t expected[6] = {1, 0x23, 0x45, 0x67, 0x89, 0xab};
        CHECK(memcmp(addr, expected, sizeof(addr)) == 0);
        memset(addr, 0xaa, sizeof(addr));
        CHECK(btctler_sscanf_bd_addr("01:23:45:67:89:ag", addr) == 0);
        CHECK(addr[0] == 0xaa && addr[5] == 0xaa); /* failed parse commits no partial bytes */
        char overlap[] = "01:23:45:67:89:ab";
        CHECK(btctler_sscanf_bd_addr(overlap, (uint8_t *)overlap) == 1);
        CHECK((uint8_t)overlap[0] == 1 && (uint8_t)overlap[5] == 0xab);
    }
    CHECK(btctler_atoi("123tail") == 123);
    CHECK(btctler_atoi("-123") == 0 && btctler_atoi("") == 0);
    CHECK(btctler_atoi("4294967296") == 0); /* i32 accumulator wraps */
    CHECK(btctler_string_len_for_uint32(0) == 1 && btctler_string_len_for_uint32(9) == 1);
    CHECK(btctler_string_len_for_uint32(10) == 2 && btctler_string_len_for_uint32(999999999) == 9);
    CHECK(btctler_string_len_for_uint32(1000000000) == 10 && btctler_string_len_for_uint32(0xffffffffu) == 10);
    CHECK(btctler_count_set_bits_uint32(0) == 0 && btctler_count_set_bits_uint32(0xffffffffu) == 32);
    CHECK(btctler_count_set_bits_uint32(0x80000001u) == 2);
    for (int i = 0; i < 32; i++) CHECK(btctler_count_set_bits_uint32(1u << i) == 1);
    {
        static const uint8_t digits[] = "123456789";
        static const uint8_t sabm[] = {0x03, 0x3f, 0x01};
        CHECK(btctler_CalcCrc32(0xffffffffu, 9, digits) == 0xcbf43926u);
        CHECK(btctler_CalcCrc32(0x12345678u, 0, digits) == (0x12345678u ^ 0xffffffffu));
        CHECK(btctler_crc8_calc((uint8_t *)sabm, sizeof(sabm)) == 0x1c);
    }
#endif
    return TEST_DONE("btctrler_btcontroller_util");
}
#endif
