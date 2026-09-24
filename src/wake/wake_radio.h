#ifndef CYCLONE2_WAKE_RADIO_H
#define CYCLONE2_WAKE_RADIO_H

/*
 * The baseband side of the wake sequence.
 *
 * Nothing in the standard HCI command set can put a non-standard access code
 * on the air, so this drives the controller library's baseband register
 * block directly.  The block is `struct ctrl_frame`, one per rented link, and
 * it holds the LAP and the 34-bit BCH parity as two independent fields - the
 * same two fields the ESP32 reference reaches through its connectionless
 * broadcast control structure.  Writing the parity from
 * wake_parity_inverted() is what makes the wake-up access code.
 *
 * Everything called here is a symbol the vendor controller archive already
 * exports, reached the same way src/bt/btstack_jl_sdk.c reaches
 * lmp_set_sniff_establish_by_remote().  The layout of the register block and
 * of struct link_task is not available in any header, so it was read out of
 * the archive's DWARF; see docs/ns1-wakeup.md.
 *
 * Status: implemented for phase 1 (the repeated wake-up code).  Phases 2 and
 * 3 need a receive path and precise in-slot scheduling that have not been
 * written yet, and none of this has been run on hardware.
 */

#include "generic/typedef.h"

struct wake_radio_config {
    u8  address[6];      /* the console, least significant byte first */
    u8  parity[5];       /* wake_parity_inverted() of the same address */
    u8  channel;         /* 0..78 */
    u16 interval_slots;  /* burst period, in 625 us slots */
    u8  llid;            /* payload header LLID */
    u8  flow;            /* payload header FLOW */
    u8  edr;             /* 0 = basic rate */
    u8  whiten_seed;     /* written to the whitening control; the body byte
                          * that reaches this function must already be
                          * pre-cancelled with the same seed */
};

/* Rent a baseband frame, point it at the wake-up access code and start
 * repeating `body` on `cfg->channel` every `cfg->interval_slots`.  Returns 0
 * on success.  `body` is copied. */
int wake_radio_start(const struct wake_radio_config *config,
                     const u8 *body, u16 length);

/* Stop the bursts and give the frame back.  Safe to call when idle. */
void wake_radio_stop(void);

int wake_radio_active(void);

#endif
