/* Owned lmp_escape3.c: Jieli's vendor LMP commands (escape 3).
 *
 * The SDK served a factory test box over the air: test mode, oscillator
 * trim (saved to flash), chip and flash identification, a chip-key CRC,
 * and starting the firmware update loader.  Any device that pages the pad
 * could send them, before authentication.  A game controller in use needs
 * none of them: every request is refused as unsupported, the SDK's answer
 * to an opcode it does not know.  Answers to escape-3 requests (we send
 * none) are only recorded, as before, never answered. */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

void lmp_escape3_command_process(struct lmp_connection *conn, u8 opcode, u8 *opcode1, u8 *rx)
{
    if (opcode == ACCEPTED_EXT || opcode == NOT_ACCEPTED_EXT) {
        *opcode1 = rx[1];
        return;
    }
    lmp_response(conn, "b05", PDU(ESCAPE_3), NOT_ACCEPTED_EXT, ESCAPE_3, opcode, 0x1a);
}
