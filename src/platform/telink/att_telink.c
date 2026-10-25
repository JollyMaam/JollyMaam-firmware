#include <string.h>
#include "pad.h"
#include "ble/att_db.h"
#include "bluetooth_gatt.h"
#include "att_telink.h"

#define CCC_MAX 8
#define MESSAGE_MAX 144         /* Switch 2's largest command response */

static struct {
    const struct profile *profile;
    att_connection_t connection;
    u16 ccc_handle[CCC_MAX];
    u8 ccc_value[CCC_MAX], ccc_count;
    u8 response[TELINK_ATT_MTU];
    u8 message[MESSAGE_MAX + 3];
    u16 response_len, message_len;
    u8 command;
} a;

static u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static int ccc(u16 handle)
{
    u32 i;
    for (i = 0; i < a.ccc_count; i++) if (a.ccc_handle[i] == handle) return (int)i;
    return -1;
}

static u16 read_value(u16 connection, u16 handle, u16 offset, u8 *out, u16 max)
{
    u8 value[32];
    int index = ccc(handle), n;

    (void)connection;
    if (index >= 0) {
        value[0] = a.ccc_value[index]; value[1] = 0;
        n = 2;
    } else {
        n = a.profile->read ? a.profile->read(BT_WRITE_ATT, handle, 0, value, sizeof(value)) : 0;
        if (n < 0) n = 0;
    }
    if (!out) return (u16)n;
    if (offset >= n) return 0;
    if (max > n - offset) max = (u16)(n - offset);
    memcpy(out, value + offset, max);
    return max;
}

static int write_value(u16 connection, u16 handle, u16 mode, u16 offset, u8 *data, u16 len)
{
    int index = ccc(handle);

    (void)connection;
    if (mode != ATT_TRANSACTION_MODE_NONE) return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    if (offset) return ATT_ERROR_INVALID_OFFSET;
    if (index >= 0) {
        if (len != 2) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        if (data[1] || data[0] > 3) return ATT_ERROR_VALUE_NOT_ALLOWED;
        a.ccc_value[index] = data[0];
        return 0;
    }
    if (a.message_len && a.command) return ATT_ERROR_INSUFFICIENT_RESOURCES;
    if (len > BT_WRITE_MAX) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
    return plat_att_write(handle, data, len) ? 0 : ATT_ERROR_INSUFFICIENT_RESOURCES;
}

int plat_att_start(const struct profile *profile)
{
    const u8 *entry = profile->device->att_db + 1;

    memset(&a, 0, sizeof(a));
    a.profile = profile;
    for (; get16(entry); entry += get16(entry)) {
        if (get16(entry) < 8) return 0;
        if (!(get16(entry + 2) & ATT_PROPERTY_UUID128) && get16(entry + 6) == 0x2902) {
            if (a.ccc_count == CCC_MAX) return 0;
            a.ccc_handle[a.ccc_count++] = get16(entry + 4);
        }
    }
    att_set_db(profile->device->att_db);
    att_set_read_callback(read_value);
    att_set_write_callback(write_value);
    return 1;
}

void plat_att_connected(u16 handle)
{
    memset(&a.connection, 0, sizeof(a.connection));
    a.connection.con_handle = handle;
    a.connection.mtu = 23;
    a.connection.max_mtu = TELINK_ATT_MTU;
    memset(a.ccc_value, 0, sizeof(a.ccc_value));
    a.response_len = a.message_len = 0;
}

void plat_att_encrypted(int encrypted)
{
    a.connection.encryption_key_size = encrypted ? 16 : 0;
}

void plat_att_receive(u8 *pdu, u16 len)
{
    if (!len || len > a.connection.mtu || a.response_len) return;
    a.response_len = att_handle_request(&a.connection, pdu, len, a.response);
}

void plat_att_poll(void)
{
    if (a.response_len && plat_att_transmit(a.response, a.response_len, 1)) a.response_len = 0;
    if (!a.response_len && a.message_len && plat_att_transmit(a.message, a.message_len, a.command)) a.message_len = 0;
}

int plat_att_notifying(u16 handle)
{
    int index = ccc((u16)(handle + 1));
    return index >= 0 && (a.ccc_value[index] & 1);
}

static int send(u16 handle, const u8 *data, u16 len, int command)
{
    if ((a.message_len && (!command || a.command)) || !plat_att_notifying(handle) || len > MESSAGE_MAX || len + 3 > a.connection.mtu) return 0;
    a.message[0] = ATT_HANDLE_VALUE_NOTIFICATION;
    a.message[1] = (u8)handle;
    a.message[2] = (u8)(handle >> 8);
    memcpy(a.message + 3, data, len);
    a.message_len = (u16)(len + 3);
    a.command = (u8)command;
    return 1;
}

int plat_att_send(u16 handle, const u8 *data, u16 len) { return send(handle, data, len, 0); }
int plat_att_message(u16 handle, const u8 *data, u16 len) { return send(handle, data, len, 1); }

int plat_att_busy(void) { return a.response_len || a.message_len; }
u16 plat_att_mtu(void) { return a.connection.mtu; }

#ifdef UNITTEST
/* The real upstream interpreter is linked by the dedicated host test. */
static u8 transmitted[2][TELINK_ATT_MTU];
static u16 transmitted_len[2];
static u8 sent, can_send = 1, write_ready = 1;
static u16 written_handle, written_len;
static u8 written[BT_WRITE_MAX];

int plat_att_transmit(const u8 *data, u16 len, int response)
{
    (void)response;
    if (!can_send) return 0;
    CHECK(sent < 2);
    if (sent < 2) { memcpy(transmitted[sent], data, len); transmitted_len[sent++] = len; }
    return 1;
}
int plat_att_write(u16 handle, const u8 *data, u16 len)
{
    if (!write_ready) return 0;
    written_handle = handle; written_len = len; memcpy(written, data, len);
    return 1;
}

/* Sparse handles, one static value, one dynamic value and one CCC. */
static const u8 db[] = {
    1,
    10,0, 2,0, 1,0, 0,0x28, 0x12,0x18,
    11,0, 2,0, 3,0, 0,0x2a, 'p','a','d',
    8,0, 0x0a,1, 7,0, 1,0x2a,
    10,0, 0x0e,1, 8,0, 2,0x29, 0,0,
    0,0,
};
static int test_read(u8 kind, u16 id, u16 off, u8 *out, u16 max)
{ (void)kind; (void)off; (void)max; if (id != 7) return -1; memcpy(out, "dynamic", 7); return 7; }
static const struct bt_device device = { .att_db = db, .input_handle = 7 };
static const struct profile profile = { .device = &device, .read = test_read };

static void receive(const u8 *data, u16 len)
{
    u8 pdu[TELINK_ATT_MTU];
    memcpy(pdu, data, len); sent = 0;
    plat_att_receive(pdu, len); plat_att_poll();
}
int main(void)
{
    static const u8 mtu[] = {2, 0xff, 1}, read_static[] = {10,3,0}, read_dynamic[] = {10,7,0};
    static const u8 blob[] = {12,7,0,3,0}, subscribe[] = {18,8,0,1,0}, command[] = {18,7,0,4,5,6};
    static const u8 bad[] = {18,8,0,1}, queued[] = {9,8,7};
    CHECK(plat_att_start(&profile)); plat_att_connected(64);
    receive(mtu, sizeof(mtu)); CHECK(sent == 1 && transmitted[0][0] == 3 && plat_att_mtu() == 185);
    receive(read_static, sizeof(read_static)); CHECK(sent == 1 && transmitted_len[0] == 4 && !memcmp(transmitted[0]+1,"pad",3));
    receive(read_dynamic, sizeof(read_dynamic)); CHECK(sent == 1 && transmitted_len[0] == 8 && !memcmp(transmitted[0]+1,"dynamic",7));
    receive(blob, sizeof(blob)); CHECK(sent == 1 && transmitted_len[0] == 5 && !memcmp(transmitted[0]+1,"amic",4));
    receive(bad, sizeof(bad)); CHECK(sent == 1 && transmitted[0][0] == 1 && !plat_att_notifying(7));
    receive(subscribe, sizeof(subscribe)); CHECK(plat_att_notifying(7));
    can_send = 0; CHECK(plat_att_send(7,queued,sizeof(queued)));
    receive(read_static, sizeof(read_static)); CHECK(!sent && plat_att_busy());
    can_send = 1; plat_att_poll();
    CHECK(sent == 2 && transmitted[0][0] == 11 && transmitted[1][0] == 27);
    receive(command,sizeof(command)); CHECK(written_handle == 7 && written_len == 3 && written[0] == 4);
    write_ready = 0; receive(command,sizeof(command)); CHECK(transmitted[0][0] == 1);
    /* A command reply replaces queued input, survives a busy FIFO, and
     * prevents another host write from overwriting its single buffer. */
    sent = 0; can_send = 0;
    CHECK(plat_att_send(7, queued, sizeof(queued)));
    CHECK(plat_att_message(7, (const u8 *)"reply", 5));
    CHECK(!plat_att_send(7, queued, sizeof(queued)));
    CHECK(!plat_att_message(7, queued, sizeof(queued)));
    receive(command, sizeof(command)); CHECK(!sent);
    can_send = 1; plat_att_poll();
    CHECK(sent == 2 && transmitted[0][0] == 1 && transmitted[0][4] == ATT_ERROR_INSUFFICIENT_RESOURCES);
    CHECK(transmitted_len[1] == 8 && !memcmp(transmitted[1] + 3, "reply", 5));
    plat_att_connected(64); CHECK(!plat_att_notifying(7) && plat_att_mtu() == 23);
    return TEST_DONE("Telink flash ATT");
}
#endif
