/* SPDX-License-Identifier: MIT */
#ifndef ZIGMODEM_H
#define ZIGMODEM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZM_PAYLOAD 64
#define ZM_HEADER 40
#define ZM_FRAME (ZM_HEADER + ZM_PAYLOAD)
#define ZM_QUEUE 2048
#define ZM_WINDOW 4
#define ZM_RATE_COUNT 19
#define ZM_PROFILE_SIZE 40
struct zm_storage_ops {
    void *ctx;
    int (*read)(void *,unsigned,uint8_t *,size_t);
    bool (*write)(void *,unsigned,const uint8_t *,size_t);
};
struct zm_slot { uint8_t data[ZM_PAYLOAD]; size_t len; uint32_t seq; uint64_t sent; bool used; };
enum zm_state { ZM_IDLE, ZM_DIALING, ZM_RINGING, ZM_ANSWERING, ZM_CONNECTED, ZM_CLOSING };
enum zm_type { ZM_CALL=1, ZM_ACCEPT, ZM_CONFIRM, ZM_BUSY, ZM_DATA, ZM_ACK,
               ZM_PING, ZM_PONG, ZM_CLOSE, ZM_CLOSED, ZM_WAIT, ZM_PROBE, ZM_PROBE_ACK, ZM_SURVEY };
struct zm_link_ops {
    void *ctx;
    bool (*channel)(void *,unsigned);
    bool (*pin)(void *,const uint8_t[16]);
    bool (*lookup)(void *,unsigned,uint8_t[16]);
    int (*energy)(void *,unsigned);
};
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
    uint32_t radio_us, crypto_us, radio_bytes;
    uint32_t firmware_crc;
    bool firmware_crc_valid;
    int16_t last_rssi;
    bool secure_identity;
    uint8_t identity[16], peer_identity[16];
    struct zm_link_ops link;
    bool advanced, caller, training, recovering, compression, adaptive, connect_pending;
    bool software_flow;
    bool flow_tx, flow_rx, usr_commands;
    unsigned report_mode, result_level, boot_profile;
    unsigned usr_ceiling, usr_floor;
    struct zm_storage_ops storage;
    struct zm_slot tx_window[ZM_WINDOW], rx_window[ZM_WINDOW];
    uint32_t tx_base;
    bool ack_pending;
    unsigned ack_count;
    uint64_t ack_at;
    unsigned channel, channel_index, profile, min_profile, max_profile, peer_max, peer_min;
    unsigned probes, probe_acks, last_probe_ack, successes, failures;
    uint64_t channel_at, probe_at, next_data, train_at, sample_at;
    uint32_t retrains, channel_changes, rtt_ms, trained_rate;
    uint32_t probe_seq, probe_base, probe_mask;
    uint32_t authenticated_frames, rejected_frames, compression_saved;
    unsigned secure_stage;
    uint32_t wire_rx, invalid_wire;
    unsigned wire_type, wire_length;
    bool survey_enabled, survey_sampled;
    unsigned survey_state, survey_index, survey_origin, survey_round;
    uint32_t survey_id, surveys, survey_moves;
    uint64_t survey_due, survey_at, survey_start, survey_end, survey_tx;
    int survey_local, channel_energy[5];
    uint64_t channel_measured[5];
    enum zm_state state;
    bool online, echo, verbose, quiet, inflight, overflow, serial_failed;
    unsigned autoanswer, rings, dial_seconds, guard_ms, escape_count, escape_char;
    unsigned command_cr, response_lf, command_bs;
    unsigned inactivity_minutes;
    bool escape_hangup;
    uint64_t last_data_at;
    uint8_t queue[ZM_QUEUE], pending[ZM_PAYLOAD];
    size_t qhead, qlen, pending_len;
    char command[128];
    char last_command[128];
    size_t command_len;
};
void zm_init(struct zm_modem *, uint64_t number, uint64_t boot_nonce, struct zm_io);
void zm_set_identity(struct zm_modem *,const uint8_t identity[16]);
uint64_t zm_phone(const uint8_t identity[16]);
void zm_enable_link(struct zm_modem *,struct zm_link_ops);
void zm_set_storage(struct zm_modem *,struct zm_storage_ops);
bool zm_save_profile(struct zm_modem *,unsigned);
bool zm_load_profile(struct zm_modem *,unsigned);
bool zm_select_profile(struct zm_modem *,unsigned);
void zm_profile_encode(const struct zm_modem *,uint8_t[ZM_PROFILE_SIZE]);
bool zm_profile_decode(struct zm_modem *,const uint8_t[ZM_PROFILE_SIZE]);
bool zm_profile_valid(const uint8_t[ZM_PROFILE_SIZE]);
unsigned zm_rate(unsigned);
bool zm_inquiry(struct zm_modem *,unsigned);
void zm_show_profile(struct zm_modem *,const uint8_t[ZM_PROFILE_SIZE]);
void zm_emit(struct zm_modem *,const char *);
void zm_tick(struct zm_modem *, uint64_t now);
void zm_receive(struct zm_modem *, const uint8_t *, size_t);
/* Returns bytes consumed; retain and retry the remainder for backpressure. */
size_t zm_input(struct zm_modem *, const uint8_t *, size_t);
/* Platform reports unrecoverable UART overrun: fail the call, never hide loss. */
void zm_overrun(struct zm_modem *);
/* Authenticated channel management, shared with the portable tests. */
void zm_control(struct zm_modem *,unsigned,uint32_t,const uint8_t *,size_t);
void zm_retrain(struct zm_modem *);
bool zm_survey_tick(struct zm_modem *);
void zm_survey_receive(struct zm_modem *,uint32_t,const uint8_t *);
#endif
