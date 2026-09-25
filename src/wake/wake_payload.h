#ifndef CYCLONE2_WAKE_PAYLOAD_H
#define CYCLONE2_WAKE_PAYLOAD_H

/*
 * The two payload bodies the wake sequence transmits, ported from
 * /shared/github.com-608-NS1-Wakeup-Demo/src/wake/wake_payload.c.
 *
 * Phase 1 repeats the wake-up code, rotated, as the body of a multi-slot
 * packet.  Phase 3 is the pad's own address followed by six zero bytes.
 *
 * Neither body is whitened on the air, but the baseband whitens whatever it
 * is handed, so the body is XORed with the whitening sequence before it is
 * passed down.  The sequence is seeded from the piconet clock, which means
 * the pre-cancelled body depends on the slot it will be sent in.
 */

#include <stddef.h>
#include <stdint.h>

/* Longest body a single wake burst can carry: a DH5 payload (339 bytes). */
#define WAKE_BODY_MAX             339U

/* Phase 3's body: our own address, then six zero bytes. */
#define WAKE_PHASE3_BODY_LENGTH    12U

/* Bits to skip before the body's first bit, from the reference.  The packet
 * header is 18 bits and the payload header is 8 bits for DM1 and 16 for DH3
 * and DH5 - the two numbers also cover the next payload header's position. */
#define WAKE_WHITEN_SKIP_DM1       26
#define WAKE_WHITEN_SKIP_MULTISLOT 34

/* Phase 1: `length` bytes of the rotated wake-up code, repeated. */
void wake_build_phase1(const uint8_t address[6], uint8_t *body, uint16_t length);

/* Phase 3: `address` least significant byte first, then six zero bytes.
 * `address` here is the *pad's* address - it is what the console learns to
 * expect the link from. */
void wake_build_phase3(const uint8_t address[6], uint8_t body[WAKE_PHASE3_BODY_LENGTH]);

/* How many whitening states the burst sweep covers.  The baseband whitens
 * every packet, and the wake procedure wants the body on the air unwhitened,
 * so the body is pre-cancelled - but the sequence depends on the state the
 * baseband will use, which is not something this port has measured yet.
 * Rather than guess once, successive bursts walk the reference's 32 states
 * and src/wake/wake_radio.c drives the baseband's whitening control to match
 * each one.  See docs/ns1-wakeup.md. */
#define WAKE_WHITEN_STATES         32U

/* The seed the reference uses for state `index` (0..31). */
uint8_t wake_whiten_seed(uint8_t index);

/* XOR the whitening sequence over `body`, starting `skip` bits in. */
void wake_whiten_cancel(uint8_t *body, uint16_t length, uint8_t seed, uint8_t skip);

#endif
