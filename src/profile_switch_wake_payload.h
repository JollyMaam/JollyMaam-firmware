#ifndef CYCLONE2_WAKE_PAYLOAD_H
#define CYCLONE2_WAKE_PAYLOAD_H

/* Whitening must be pre-cancelled; BR23 seed semantics remain unverified. */

#include <stddef.h>
#include <stdint.h>

#define WAKE_BODY_MAX             339U

#define WAKE_WHITEN_SKIP_MULTISLOT 34

void wake_build_phase1(const uint8_t address[6], uint8_t *body, uint16_t length);

/* How many whitening states the burst sweep covers.  The baseband whitens
 * every packet, and the wake procedure wants the body on the air unwhitened,
 * so the body is pre-cancelled - but the sequence depends on the state the
 * baseband will use, which is not something this port has measured yet.
 * Rather than guess once, successive bursts walk the reference's 32 states
 * and src/profile_switch_wake_radio.c drives the baseband's whitening control to match
 * each one.  See docs/radio.md. */
#define WAKE_WHITEN_STATES         32U

uint8_t wake_whiten_seed(uint8_t index);

/* XOR the whitening sequence over `body`, starting `skip` bits in. */
void wake_whiten_cancel(uint8_t *body, uint16_t length, uint8_t seed, uint8_t skip);

#endif
