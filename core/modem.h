/* SPDX-License-Identifier: MIT */
#ifndef ZIGMODEM_H
#define ZIGMODEM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZM_PAYLOAD 64
#define ZM_HEADER 32
#define ZM_FRAME (ZM_HEADER + ZM_PAYLOAD)
#define ZM_QUEUE 2048
enum zm_state { ZM_IDLE, ZM_DIALING, ZM_RINGING, ZM_ANSWERING, ZM_CONNECTED, ZM_CLOSING };
enum zm_type { ZM_CALL=1, ZM_ACCEPT, ZM_CONFIRM, ZM_BUSY, ZM_DATA, ZM_ACK,
               ZM_PING, ZM_PONG, ZM_CLOSE, ZM_CLOSED };
/* Callbacks are synchronous and must not reenter the modem. Serial must accept
 * the entire buffer or none; radio may drop a frame (the protocol retries).
 * All API calls must be serialized by the platform. now is monotonic ms. */
struct zm_io {
    void *ctx;
    void (*radio)(void *, const uint8_t *, size_t);
    bool (*serial)(void *, const uint8_t *, size_t);
};
struct zm_modem {
    struct zm_io io;
    uint64_t number, peer, session, nonce;
    uint64_t now, started, last_control, last_rx, last_ping, last_tx;
    uint64_t last_input, escape_at;
    uint32_t tx_seq, rx_seq;
    uint32_t frames_tx, frames_rx, retries, overruns, bytes_rx, bytes_acked;
    uint32_t random_state, retry_ms, last_fault;
    enum zm_state state;
    bool online, echo, verbose, quiet, inflight, overflow, serial_failed;
    unsigned autoanswer, rings, dial_seconds, guard_ms, escape_count;
    uint8_t queue[ZM_QUEUE], pending[ZM_PAYLOAD];
    size_t qhead, qlen, pending_len;
    char command[128];
    size_t command_len;
};
void zm_init(struct zm_modem *, uint64_t number, uint64_t boot_nonce, struct zm_io);
void zm_tick(struct zm_modem *, uint64_t now);
void zm_receive(struct zm_modem *, const uint8_t *, size_t);
/* Returns bytes consumed; retain and retry the remainder for backpressure. */
size_t zm_input(struct zm_modem *, const uint8_t *, size_t);
/* Platform reports unrecoverable UART overrun: fail the call, never hide loss. */
void zm_overrun(struct zm_modem *);
#endif
