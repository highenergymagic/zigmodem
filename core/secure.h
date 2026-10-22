/* SPDX-License-Identifier: MIT */
#ifndef ZM_SECURE_H
#define ZM_SECURE_H
#include "modem.h"
#include <noise/protocol.h>
#define ZS_WIRE_MAX 108
#define ZS_RECORD_MAX 160
struct zs_io {
    void *ctx;
    void (*wire)(void *,const uint8_t *,size_t);
    void (*plain)(void *,const uint8_t *,size_t);
    bool (*accept)(void *);
    bool (*trust)(void *,const uint8_t[16]);
};
struct zs_link {
    struct zs_io io;
    uint8_t private_key[32], identity[16], expected[16], remote_id[16];
    uint64_t number, peer, session, now, last_send, started, tx_counter, rx_top, rx_bits;
    NoiseHandshakeState *hs;
    NoiseCipherState *tx, *rx;
    bool initiator, ready;
    bool compression;
    uint32_t compression_saved;
    unsigned stage, cached_type;
    uint8_t cached[ZS_RECORD_MAX], pending[ZM_FRAME], assembly[ZS_RECORD_MAX], assembly_header[40];
    size_t cached_len, pending_len, assembly_len;
    unsigned assembly_mask;
    uint32_t rejected, authenticated;
    uint32_t wire_rx, invalid_wire;
    unsigned last_type, last_length;
};
bool zs_identity(const uint8_t private_key[32], uint8_t id[16]);
bool zs_init(struct zs_link *,const uint8_t private_key[32],struct zs_io);
void zs_reset(struct zs_link *);
void zs_send(struct zs_link *,const uint8_t *,size_t,const uint8_t expected[16]);
void zs_receive(struct zs_link *,const uint8_t *,size_t);
void zs_tick(struct zs_link *,uint64_t now);
#endif
