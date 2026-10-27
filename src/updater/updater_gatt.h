#ifndef UPDATER_GATT_H
#define UPDATER_GATT_H

/* The updater's GATT service (docs/firmware/updater.md), for both chips'
 * LE sides: BTstack's database format (the BR23's BTstack, the B80's
 * att_db.c).  REQUEST takes a frame (updater_frame()); REPLY notifies the
 * answer.  Per entry its size, flags, handle, UUID (128-bit with flag
 * 0x0200) and value; flags 0x0100: dynamic. */

#define UPD_LE16(v)  (u8)(v), (u8)((v) >> 8)
/* c255xxxx-3e2d-66b1-8a4f-d5037c2b419e, least significant byte first. */
#define UPD_UUID(n)  0x9e, 0x41, 0x2b, 0x7c, 0x03, 0xd5, 0x4f, 0x8a, 0xb1, 0x66, 0x2d, 0x3e, n, 0x00, 0x55, 0xc2
#define UPD_SERVICE(h)                   UPD_LE16(24), UPD_LE16(0x0002), UPD_LE16(h), UPD_LE16(0x2800), UPD_UUID(0)
#define UPD_CHARACTERISTIC(h, props, n)  UPD_LE16(27), UPD_LE16(0x0002), UPD_LE16(h), UPD_LE16(0x2803), props, UPD_LE16((h) + 1), UPD_UUID(n)
#define UPD_VALUE(h, flags, n)           UPD_LE16(22), UPD_LE16(0x0300 | (flags)), UPD_LE16(h), UPD_UUID(n)
#define UPD_CCC(h)                       UPD_LE16(8), UPD_LE16(0x010e), UPD_LE16(h), UPD_LE16(0x2902)
enum { UPD_WRITE_COMMAND = 0x04, UPD_WRITE = 0x08, UPD_NOTIFY = 0x10 };
enum { UPD_REQUEST = 0x0003, UPD_REPLY = 0x0005 };

static const u8 updater_att_db[] = {
    1,
    UPD_SERVICE(0x0001),
    UPD_CHARACTERISTIC(UPD_REQUEST - 1, UPD_WRITE_COMMAND | UPD_WRITE, 1),
    UPD_VALUE(UPD_REQUEST, UPD_WRITE_COMMAND | UPD_WRITE, 1),
    UPD_CHARACTERISTIC(UPD_REPLY - 1, UPD_NOTIFY, 2),
    UPD_VALUE(UPD_REPLY, 0, 2),
    UPD_CCC(UPD_REPLY + 1),
    0, 0,
};

/* Advertising: flags and the service; scan response: "<board> Update". */
static const u8 updater_adv[] = { 2, 0x01, 0x06, 17, 0x07, UPD_UUID(0) };

static u8 updater_scan_response(u8 out[31])
{
    u32 n = strlen(board_name);

    if (n > 22) n = 22;
    out[0] = (u8)(n + 8);
    out[1] = 0x09;               /* complete local name */
    memcpy(out + 2, board_name, n);
    memcpy(out + 2 + n, " Update", 7);
    return (u8)(n + 9);
}

/* The LE address: the flash UID's, apart from the app's slot addresses. */
static void updater_address(u8 a[6])
{
    const u8 *uid = plat_unique_id();
    static const u8 none[6] = { 0x55, 0x50, 0x44, 0x54, 0x00, 0xc2 };
    u32 i;

    memcpy(a, none, 6);
    if (uid) for (i = 0; i < 6; i++) a[i] = uid[i] ^ uid[i + 6] ^ uid[i + 10] ^ 0x5a;
}

#endif
