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
               ZM_PING, ZM_PONG, ZM_CLOSE, ZM_CLOSED, ZM_WAIT, ZM_PROBE, ZM_PROBE_ACK };
struct zm_link_ops { void *ctx; bool (*channel)(void *,unsigned); };
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
    uint32_t radio_errors;
    uint32_t radio_status;
    int16_t last_rssi;
    bool secure_identity;
    uint8_t identity[16], peer_identity[16];
    struct zm_link_ops link;
    bool advanced, caller, training, recovering, compression, adaptive, connect_pending;
    bool software_flow;
    unsigned channel, channel_index, profile, min_profile, max_profile, peer_max, peer_min;
    unsigned probes, probe_acks, last_probe_ack, successes, failures;
    uint64_t channel_at, probe_at, next_data, train_at, sample_at;
    uint32_t retrains, channel_changes, rtt_ms, trained_rate;
    uint32_t probe_seq, probe_base, probe_mask;
    uint32_t authenticated_frames, rejected_frames, compression_saved;
    unsigned secure_stage;
    uint32_t wire_rx, invalid_wire;
    unsigned wire_type, wire_length;
    enum zm_state state;
    bool online, echo, verbose, quiet, inflight, overflow, serial_failed;
    unsigned autoanswer, rings, dial_seconds, guard_ms, escape_count;
    uint8_t queue[ZM_QUEUE], pending[ZM_PAYLOAD];
    size_t qhead, qlen, pending_len;
    char command[128];
    size_t command_len;
};
void zm_init(struct zm_modem *, uint64_t number, uint64_t boot_nonce, struct zm_io);
void zm_set_identity(struct zm_modem *,const uint8_t identity[16]);
void zm_enable_link(struct zm_modem *,struct zm_link_ops);
void zm_tick(struct zm_modem *, uint64_t now);
void zm_receive(struct zm_modem *, const uint8_t *, size_t);
/* Returns bytes consumed; retain and retry the remainder for backpressure. */
size_t zm_input(struct zm_modem *, const uint8_t *, size_t);
/* Platform reports unrecoverable UART overrun: fail the call, never hide loss. */
void zm_overrun(struct zm_modem *);
#endif
