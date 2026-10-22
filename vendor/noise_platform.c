/* SPDX-License-Identifier: MIT */
#include "internal.h"
NoiseCipherState *noise_aesgcm_new_ref(void) { return NULL; }
NoiseHashState *noise_blake2b_new(void) { return NULL; }
NoiseHashState *noise_sha256_new(void) { return NULL; }
NoiseHashState *noise_sha512_new(void) { return NULL; }
NoiseDHState *noise_curve448_new(void) { return NULL; }
NoiseDHState *noise_newhope_new(void) { return NULL; }
/* Datagram receive only. The application owns replay protection; a forged
 * high nonce must not poison the cipher state before tag verification. */
int zm_noise_decrypt_at(NoiseCipherState *state,uint64_t nonce,const uint8_t *ad,
                       size_t ad_len,NoiseBuffer *buffer) {
    if(!state || nonce==UINT64_MAX) return NOISE_ERROR_INVALID_NONCE;
    uint64_t saved=state->n; state->n=nonce;
    int result=noise_cipherstate_decrypt_with_ad(state,ad,ad_len,buffer);
    state->n=saved; return result;
}
#ifdef __ZEPHYR__
#include <zephyr/random/random.h>
#include <zephyr/kernel.h>
void noise_rand_bytes(void *bytes, size_t size) {
    if (sys_csrand_get(bytes,size)) k_panic();
}
#endif
