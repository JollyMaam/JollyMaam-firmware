#ifndef CYCLONE2_UX_H
#define CYCLONE2_UX_H

#include "generic/typedef.h"
#include "controller_state.h"
#include "input.h"

#define CYCLONE2_UX_NO_SLOT 0xffU

/* One control poll: follow Home and what is held with it, and run the
 * gestures in ux.c's table.  `kind` is the controller's state now. */
void cyclone2_ux_poll(u32 now, const struct cyclone2_input_report *in,
                      enum cyclone2_controller_kind kind);
/* Keep from the host what a gesture holds, and add a Home tap it passes on. */
void cyclone2_ux_filter(struct cyclone2_input_report *report, u32 now);
/* Whatever is held now starts nothing: wait until it is all let go. */
void cyclone2_ux_block(void);
/* At start-up: a Home press (with slot key `early_slot`, or none) that woke
 * the pad and was let go before the first poll still counts. */
void cyclone2_ux_boot(int early_home, u8 early_slot,
                      const struct cyclone2_input_report *in,
                      enum cyclone2_controller_kind kind);

#endif
