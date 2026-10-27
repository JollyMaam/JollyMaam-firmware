#ifndef UPDATER_H
#define UPDATER_H

#include "pad.h"

/* Between updater.c (the protocol, the loop) and the chip's side
 * (updater_jieli.c, updater_telink.c): where the app lives and how its
 * flash is written.  Offsets are into the app's region: its header at 0,
 * the app at MAIN_HEAD_SIZE (its entry). */

#define MAIN_MAGIC      0x4e49414du     /* "MAIN", the header's commit word */
#define MAIN_HEAD_SIZE  0x120u
#define MAIN_VERSION    1
#define SECTOR          0x1000u

enum { ERR_NONE, ERR_SIZE, ERR_ORDER, ERR_NOT_BEGUN, ERR_VERIFY, ERR_INCOMPLETE, ERR_HEADER,
       ERR_CRC, ERR_KEY, ERR_BOARD, ERR_LAYOUT };

u32 upd_region(void);            /* the header's flash offset */
u32 upd_room(void);              /* bytes for header and app */
u32 upd_entry(void);             /* the app's entry, as the header must name it */
u32 upd_read32(u32 offset);      /* as written (deciphered) */
void upd_read(u32 offset, u8 *out, u32 n);
/* Before the first erase of an update; ERR_NONE or why not. */
u8 upd_prepare(void);
/* After the header's sector is erased: anything that needs it (the
 * BR23's chip key); ERR_NONE or why not. */
u8 upd_erased(void);
void upd_erase(u32 offset);      /* the sector at offset */
/* Programs n bytes at offset, all but the first skip, and reads them back. */
int upd_program(u32 offset, const u8 *data, u32 n, u32 skip);
void upd_committed(void);        /* after the commit word */
void upd_started(void);          /* the loop starts: the chip's own log lines */
u32 upd_extra(void);             /* UPDATE_INFO's word at 20: the chip key on the BR23 */
u32 upd_data_max(void);          /* the most UPDATE_DATA bytes a transport takes */

/* The LE side (updater_ble.c, updater_ble_telink.c). */
void updater_ble_start(void);
int updater_ble_connected(void);

/* updater.c: a host's LE frame (u8 sub-command, u32 argument, data): the
 * reply's length in reply (u8 sub-command, then the answer), 0: none. */
u32 updater_frame(const u8 *frame, u32 len, u8 *reply, u32 max);

#endif
