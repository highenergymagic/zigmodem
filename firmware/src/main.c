/* SPDX-License-Identifier: MIT */
#include "modem.h"
#include "secure.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#define MAC_HEADER 15
#define PAN 0x5a4d
#define CHANNEL 25
static const struct device *uart=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *radio=DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));
static const struct ieee802154_radio_api *api;
static struct net_if *iface;
static struct zm_modem modem;
static struct zs_link secure;
extern int identity_load(uint8_t key[32]);
extern bool identity_trust(void *,const uint8_t[16]);
extern bool identity_lookup(void *,unsigned,uint8_t[16]);
extern int settings_read(void *,unsigned,uint8_t *,size_t);
extern bool settings_write(void *,unsigned,const uint8_t *,size_t);
static uint8_t mac_seq;
RING_BUF_DECLARE(serial_rx_queue,8192);
RING_BUF_DECLARE(serial_tx_queue,4096);
static atomic_t overrun;
static atomic_t flow_enabled, host_paused, flow_pending;
static bool input_paused;
static unsigned tx_fail_streak;
static uint32_t firmware_checksum(void) {
    extern const uint8_t __rom_region_start[], __rom_region_end[];
    uintptr_t pos=(uintptr_t)__rom_region_start,end=(uintptr_t)__rom_region_end;
    uint32_t crc=0xffffffffu;
    for(;pos<end;pos++) {
        crc^=*(const volatile uint8_t *)pos;
        for(unsigned i=0;i<8;i++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}
K_SEM_DEFINE(energy_done,0,1);
static atomic_t energy_value;
static void energy_result(const struct device *dev,int16_t value) {
    ARG_UNUSED(dev); atomic_set(&energy_value,value); k_sem_give(&energy_done);
}
static int measure_energy(void *ctx,unsigned duration) {
    ARG_UNUSED(ctx);
    if(!api->ed_scan) return 127;
    k_sem_reset(&energy_done); atomic_set(&energy_value,127);
    int result=api->ed_scan(radio,(uint16_t)duration,energy_result);
    if(!result) result=k_sem_take(&energy_done,K_MSEC(100));
#if defined(CONFIG_SOC_CC2652P)
    extern int zigmodem_ti_resume_rx(const struct device *dev);
    if(zigmodem_ti_resume_rx(radio)) result=-1;
#endif
    int value=(int)atomic_get(&energy_value);
    return !result && value>-128 && value<0?value:127;
}
struct rx_frame { uint8_t bytes[127]; uint8_t len; int16_t rssi; };
K_MSGQ_DEFINE(radio_rx,sizeof(struct rx_frame),8,4);

static void serial_kick(void) {
#if !defined(CONFIG_UART_SILABS_USART_ASYNC)
    uart_irq_tx_enable(uart);
#endif
}
static void serial_receive(const uint8_t *bytes,size_t n) {
    for(size_t i=0;i<n;i++) {
        if(atomic_get(&flow_enabled) && (bytes[i]==0x11 || bytes[i]==0x13)) {
            atomic_set(&host_paused,bytes[i]==0x13);
            if(bytes[i]==0x11) serial_kick();
        } else if(ring_buf_put(&serial_rx_queue,bytes+i,1)!=1) atomic_set(&overrun,1);
    }
}
#if defined(CONFIG_UART_SILABS_USART_ASYNC)
static uint8_t dma_rx[2][512],dma_tx[256];
static unsigned dma_next;
static atomic_t dma_busy;
static void serial_event(const struct device *dev,struct uart_event *evt,void *ctx) {
    ARG_UNUSED(ctx);
    switch(evt->type) {
    case UART_RX_RDY: serial_receive(evt->data.rx.buf+evt->data.rx.offset,evt->data.rx.len); break;
    case UART_RX_BUF_REQUEST:
        if(uart_rx_buf_rsp(dev,dma_rx[dma_next],sizeof(dma_rx[0]))) atomic_set(&overrun,1);
        dma_next^=1; break;
    case UART_RX_STOPPED: atomic_set(&overrun,1); break;
    case UART_RX_DISABLED:
        dma_next=1;
        if(uart_rx_enable(dev,dma_rx[0],sizeof(dma_rx[0]),1000)) atomic_set(&overrun,1);
        break;
    case UART_TX_ABORTED: atomic_set(&overrun,1); atomic_clear(&dma_busy); break;
    case UART_TX_DONE: atomic_clear(&dma_busy); break;
    default: break;
    }
}
static void serial_pump(void) {
    if(!atomic_cas(&dma_busy,0,1)) return;
    uint8_t control=(uint8_t)atomic_get(&flow_pending); size_t n=0;
    if(control) { dma_tx[0]=control; n=1; }
    else if(!atomic_get(&host_paused)) n=ring_buf_peek(&serial_tx_queue,dma_tx,sizeof(dma_tx));
    if(!n) { atomic_clear(&dma_busy); return; }
    if(uart_tx(uart,dma_tx,n,SYS_FOREVER_US)) { atomic_clear(&dma_busy); atomic_set(&overrun,1); return; }
    if(control) atomic_cas(&flow_pending,control,0);
    else { uint8_t discard[sizeof(dma_tx)]; (void)ring_buf_get(&serial_tx_queue,discard,n); }
}
#else
static void uart_isr(const struct device *dev,void *ctx) {
    ARG_UNUSED(ctx);
    for(;;) {
        uart_irq_update(dev);
        if(!uart_irq_is_pending(dev)) break;
        if(uart_irq_rx_ready(dev)) {
            uint8_t bytes[32]; int n;
            while((n=uart_fifo_read(dev,bytes,sizeof(bytes)))>0) {
                serial_receive(bytes,(size_t)n);
            }
        }
        if(uart_irq_tx_ready(dev)) {
            uint8_t control=(uint8_t)atomic_get(&flow_pending);
            if(control) {
                if(uart_fifo_fill(dev,&control,1)==1) atomic_cas(&flow_pending,control,0);
                continue;
            }
            if(atomic_get(&host_paused)) { uart_irq_tx_disable(dev); continue; }
            uint8_t bytes[32]; uint32_t n=ring_buf_peek(&serial_tx_queue,bytes,sizeof(bytes));
            if(!n) uart_irq_tx_disable(dev);
            else { int sent=uart_fifo_fill(dev,bytes,n);
                if(sent>0) (void)ring_buf_get(&serial_tx_queue,bytes,sent); }
        }
    }
    if(uart_err_check(dev)>0) atomic_set(&overrun,1);
}
#endif
static bool serial_out(void *ctx,const uint8_t *p,size_t n) {
    ARG_UNUSED(ctx); unsigned key=irq_lock();
    if(ring_buf_space_get(&serial_tx_queue)<n) { irq_unlock(key); return false; }
    ring_buf_put(&serial_tx_queue,p,n); serial_kick(); irq_unlock(key); return true;
}
static enum net_verdict l2_recv(struct net_if *dev,struct net_pkt *pkt) {
    ARG_UNUSED(dev); struct rx_frame f;
    size_t n=net_pkt_get_len(pkt);
    if(n<=sizeof(f.bytes)) {
        net_pkt_cursor_init(pkt);
        if(!net_pkt_read(pkt,f.bytes,n)) { f.len=(uint8_t)n;
            f.rssi=net_pkt_ieee802154_rssi_dbm(pkt);
            (void)k_msgq_put(&radio_rx,&f,K_NO_WAIT); }
    }
    net_pkt_unref(pkt); return NET_OK;
}
static int l2_send(struct net_if *dev,struct net_pkt *pkt) {
    ARG_UNUSED(dev); ARG_UNUSED(pkt); return -ENOTSUP;
}
static int l2_enable(struct net_if *dev,bool state) { ARG_UNUSED(dev); ARG_UNUSED(state); return 0; }
static enum net_l2_flags l2_flags(struct net_if *dev) { ARG_UNUSED(dev); return NET_L2_MULTICAST; }
NET_L2_INIT(CUSTOM_IEEE802154_L2,l2_recv,l2_send,l2_enable,l2_flags);

static void wire_out(void *ctx,const uint8_t *p,size_t n) {
    ARG_UNUSED(ctx);
    uint8_t f[MAC_HEADER+ZS_WIRE_MAX];
    sys_put_le16(0xc841,f); f[2]=mac_seq++;
    sys_put_le16(PAN,f+3); sys_put_le16(0xffff,f+5);
    sys_put_le64(modem.number,f+7); memcpy(f+MAC_HEADER,p,n);
    struct net_pkt *pkt=net_pkt_alloc_with_buffer(iface,n+MAC_HEADER,NET_AF_UNSPEC,0,K_NO_WAIT);
    if(!pkt) { modem.radio_errors++; return; }
    if(!net_pkt_write(pkt,f,n+MAC_HEADER)) {
        uint32_t began=k_cycle_get_32();
        if(api->tx(radio,IEEE802154_TX_MODE_CSMA_CA,pkt,pkt->frags)) { modem.radio_errors++; tx_fail_streak++; }
        else tx_fail_streak=0;
        modem.radio_us+=(uint32_t)k_cyc_to_us_floor32(k_cycle_get_32()-began);
        modem.radio_bytes+=(uint32_t)(n+MAC_HEADER+2);
    }
    net_pkt_unref(pkt);
#if defined(CONFIG_SOC_CC2652P)
    extern uint32_t zigmodem_ti_status(const struct device *dev);
    modem.radio_status=zigmodem_ti_status(radio);
#endif
}
static void radio_out(void *ctx,const uint8_t *p,size_t n) {
    ARG_UNUSED(ctx); uint32_t began=k_cycle_get_32(),radio_before=modem.radio_us;
    zs_send(&secure,p,n,modem.peer_identity);
    uint32_t elapsed=(uint32_t)k_cyc_to_us_floor32(k_cycle_get_32()-began);
    if(elapsed>=modem.radio_us-radio_before) modem.crypto_us+=elapsed-(modem.radio_us-radio_before);
}
static void plain_in(void *ctx,const uint8_t *p,size_t n) {
    ARG_UNUSED(ctx);
    if(n>=ZM_HEADER && p[3]==ZM_CALL) memcpy(modem.peer_identity,secure.remote_id,16);
    zm_receive(&modem,p,n);
}
static bool accept_call(void *ctx) { ARG_UNUSED(ctx); return modem.state==ZM_IDLE; }
static bool change_channel(void *ctx,unsigned channel) {
    ARG_UNUSED(ctx);
    /* TI's same-channel setter does not restart a stopped receiver. */
    if(channel==modem.channel) return true;
    /* Only anchor once a peer has responded. Elliptic-curve work on the
     * Cortex-M can span several scheduler ticks; don't hop mid-handshake. */
    if(tx_fail_streak<4 && ((!secure.initiator && secure.hs) || secure.stage==3 ||
       (secure.stage==1 && secure.assembly_mask))) return false;
    /* EFR32 applies current_channel only when RX starts. TI start is a no-op,
     * while its set_channel restarts RX. Stop/set/start supports both. */
    if(api->stop(radio)) return false;
    int result=api->set_channel(radio,channel);
    int restarted=api->start(radio);
    if(result==0 && restarted==0) { tx_fail_streak=0; return true; }
    return false;
}
static void fatal(const char *s) {
    while(*s) uart_poll_out(uart,*s++);
    for(;;) k_sleep(K_SECONDS(1));
}
int main(void) {
    if(!device_is_ready(uart)) return 0;
    if(!device_is_ready(radio)) fatal("\r\nRADIO INIT ERROR\r\n");
    api=(const struct ieee802154_radio_api *)radio->api;
    iface=net_if_lookup_by_dev(radio);
    if(!iface) fatal("\r\nRADIO INTERFACE ERROR\r\n");
    uint64_t nonce; uint8_t key[32];
    if(identity_load(key) || !zs_init(&secure,key,(struct zs_io){NULL,wire_out,plain_in,accept_call,identity_trust}) ||
       sys_csrand_get(&nonce,sizeof(nonce))) fatal("\r\nIDENTITY/ENTROPY ERROR\r\n");
    noise_clean(key,sizeof(key));
    zm_init(&modem,secure.number,nonce,(struct zm_io){NULL,radio_out,serial_out});
    modem.firmware_crc=firmware_checksum(); modem.firmware_crc_valid=true;
    zm_set_identity(&modem,secure.identity);
    struct ieee802154_filter filter={.pan_id=PAN};
    if(api->filter(radio,true,IEEE802154_FILTER_TYPE_PAN_ID,&filter)) fatal("\r\nPAN ERROR\r\n");
    filter.short_addr=0xfffe;
    if(api->filter(radio,true,IEEE802154_FILTER_TYPE_SHORT_ADDR,&filter)) fatal("\r\nADDRESS ERROR\r\n");
    if(api->set_channel(radio,CHANNEL) || api->set_txpower(radio,5) || api->start(radio))
        fatal("\r\nRADIO START ERROR\r\n");
    zm_enable_link(&modem,(struct zm_link_ops){NULL,change_channel,identity_trust,identity_lookup,measure_energy});
    zm_set_storage(&modem,(struct zm_storage_ops){NULL,settings_read,settings_write});
#if defined(CONFIG_UART_SILABS_USART_ASYNC)
    dma_next=1;
    if(uart_callback_set(uart,serial_event,NULL) || uart_rx_enable(uart,dma_rx[0],sizeof(dma_rx[0]),1000)) fatal("\r\nUART DMA ERROR\r\n");
#else
    uart_irq_callback_user_data_set(uart,uart_isr,NULL); uart_irq_rx_enable(uart);
#endif
    uint8_t input[128]; size_t pending=0;
    for(;;) {
        bool enabled=modem.flow_rx;
        atomic_set(&flow_enabled,enabled);
        if(!enabled && atomic_cas(&host_paused,1,0)) serial_kick();
        size_t queued=ring_buf_size_get(&serial_rx_queue)+modem.qlen;
        bool pause=modem.flow_tx && (input_paused?queued>1024:queued>4096);
        if(pause!=input_paused) {
            input_paused=pause; atomic_set(&flow_pending,pause?0x13:0x11); serial_kick();
        }
        secure.compression=modem.compression;
        modem.authenticated_frames=secure.authenticated;
        modem.rejected_frames=secure.rejected;
        modem.compression_saved=secure.compression_saved;
        modem.secure_stage=secure.stage;
        modem.wire_rx=secure.wire_rx; modem.invalid_wire=secure.invalid_wire;
        modem.wire_type=secure.last_type; modem.wire_length=secure.last_length;
        zs_tick(&secure,k_uptime_get());
        modem.now=k_uptime_get();
        if(atomic_cas(&overrun,1,0)) { zm_overrun(&modem); pending=0;
            unsigned key=irq_lock(); ring_buf_reset(&serial_rx_queue); irq_unlock(key); }
        struct rx_frame f;
        while(!k_msgq_get(&radio_rx,&f,K_NO_WAIT)) {
            if(f.len>=MAC_HEADER+4 && sys_get_le16(f.bytes)==0xc841 &&
               sys_get_le16(f.bytes+3)==PAN && sys_get_le16(f.bytes+5)==0xffff &&
               ((f.bytes[MAC_HEADER+3]>=4 && sys_get_le64(f.bytes+7)==secure.peer) ||
                (f.len>=MAC_HEADER+44 && !memcmp(f.bytes+7,f.bytes+MAC_HEADER+4,8)))) {
                modem.last_rssi=f.rssi;
                zs_receive(&secure,f.bytes+MAC_HEADER,f.len-MAC_HEADER);
            }
        }
        if(!pending) { unsigned key=irq_lock(); pending=ring_buf_get(&serial_rx_queue,input,sizeof(input)); irq_unlock(key); }
        if(pending) { size_t n=zm_input(&modem,input,pending); pending-=n; memmove(input,input+n,pending); }
        zm_tick(&modem,k_uptime_get());
#if defined(CONFIG_UART_SILABS_USART_ASYNC)
        serial_pump();
#endif
        k_sleep(K_MSEC(1));
    }
    return 0;
}
