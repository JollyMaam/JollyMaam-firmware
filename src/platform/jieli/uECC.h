/* micro-ecc 1.x, as the SDK's crypto_toolbox library builds it (uECC_new.c):
 * the declarations btstack_hci_jieli.c uses.  BTstack's bundled uECC.h is the
 * older API without a curve argument; the library would read garbage. */
#ifndef UECC_H
#define UECC_H

#include <stdint.h>

#define uECC_SUPPORTS_secp256r1 1

typedef int (*uECC_RNG_Function)(uint8_t *dest, unsigned size);
struct uECC_Curve_t;
typedef const struct uECC_Curve_t *uECC_Curve;

uECC_Curve uECC_secp256r1(void);
void uECC_set_rng(uECC_RNG_Function rng_function);
int uECC_make_key(uint8_t *public_key, uint8_t *private_key, uECC_Curve curve);
int uECC_shared_secret(const uint8_t *public_key, const uint8_t *private_key,
                       uint8_t *secret, uECC_Curve curve);
int uECC_valid_public_key(const uint8_t *public_key, uECC_Curve curve);

#endif
