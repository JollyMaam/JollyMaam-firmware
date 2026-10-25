#ifndef ATT_TELINK_H
#define ATT_TELINK_H

/* The flash ATT server, independent of SDK types. Notifications and ATT
 * responses have separate bounded buffers; responses go first. */
#define TELINK_ATT_BUFFER 160
int plat_att_start(const struct profile *profile, u16 max_mtu);
void plat_att_connected(u16 handle);
void plat_att_encrypted(int encrypted);
void plat_att_receive(u8 *pdu, u16 len);
void plat_att_reject_large(u8 opcode, u16 handle);
void plat_att_poll(void);
int plat_att_notifying(u16 handle);
int plat_att_send(u16 handle, const u8 *data, u16 len);
int plat_att_message(u16 handle, const u8 *data, u16 len);
int plat_att_busy(void);
u16 plat_att_mtu(void);

/* SDK-side seams: copy a raw ATT PDU to the LL FIFO, or queue a host write
 * for the controller loop. No profile calls from a receive callback. */
int plat_att_transmit(const u8 *pdu, u16 len, int response);
int plat_att_write(u16 handle, const u8 *data, u16 len);

#endif
