/* SPDX-License-Identifier: MIT */
#include "modem.h"
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
#define CHANNEL 20
static const struct device *uart=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *radio=DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));
static const struct ieee802154_radio_api *api;
static struct net_if *iface;
static struct zm_modem modem;
static uint8_t mac_seq;
RING_BUF_DECLARE(serial_rx_queue,8192);
RING_BUF_DECLARE(serial_tx_queue,4096);
static atomic_t overrun;
struct rx_frame { uint8_t bytes[127]; uint8_t len; };
K_MSGQ_DEFINE(radio_rx,sizeof(struct rx_frame),8,4);

static void uart_isr(const struct device *dev,void *ctx) {
    ARG_UNUSED(ctx);
    for(;;) {
        uart_irq_update(dev);
        if(!uart_irq_is_pending(dev)) break;
        if(uart_irq_rx_ready(dev)) {
            uint8_t bytes[32]; int n;
            while((n=uart_fifo_read(dev,bytes,sizeof(bytes)))>0)
                if(ring_buf_put(&serial_rx_queue,bytes,n)!=(uint32_t)n) atomic_set(&overrun,1);
        }
        if(uart_irq_tx_ready(dev)) {
            uint8_t bytes[32]; uint32_t n=ring_buf_peek(&serial_tx_queue,bytes,sizeof(bytes));
            if(!n) uart_irq_tx_disable(dev);
            else { int sent=uart_fifo_fill(dev,bytes,n);
                if(sent>0) (void)ring_buf_get(&serial_tx_queue,bytes,sent); }
        }
    }
    if(uart_err_check(dev)>0) atomic_set(&overrun,1);
}
static bool serial_out(void *ctx,const uint8_t *p,size_t n) {
    ARG_UNUSED(ctx); unsigned key=irq_lock();
    if(ring_buf_space_get(&serial_tx_queue)<n) { irq_unlock(key); return false; }
    ring_buf_put(&serial_tx_queue,p,n); uart_irq_tx_enable(uart); irq_unlock(key); return true;
}
static enum net_verdict l2_recv(struct net_if *dev,struct net_pkt *pkt) {
    ARG_UNUSED(dev); struct rx_frame f;
    size_t n=net_pkt_get_len(pkt);
    if(n<=sizeof(f.bytes)) {
        net_pkt_cursor_init(pkt);
        if(!net_pkt_read(pkt,f.bytes,n)) { f.len=(uint8_t)n; (void)k_msgq_put(&radio_rx,&f,K_NO_WAIT); }
    }
    net_pkt_unref(pkt); return NET_OK;
}
static int l2_send(struct net_if *dev,struct net_pkt *pkt) {
    ARG_UNUSED(dev); ARG_UNUSED(pkt); return -ENOTSUP;
}
static int l2_enable(struct net_if *dev,bool state) { ARG_UNUSED(dev); ARG_UNUSED(state); return 0; }
static enum net_l2_flags l2_flags(struct net_if *dev) { ARG_UNUSED(dev); return NET_L2_MULTICAST; }
NET_L2_INIT(CUSTOM_IEEE802154_L2,l2_recv,l2_send,l2_enable,l2_flags);

static void radio_out(void *ctx,const uint8_t *p,size_t n) {
    ARG_UNUSED(ctx);
    uint8_t f[MAC_HEADER+ZM_FRAME];
    sys_put_le16(0xc841,f); f[2]=mac_seq++;
    sys_put_le16(PAN,f+3); sys_put_le16(0xffff,f+5);
    sys_put_le64(modem.number,f+7); memcpy(f+MAC_HEADER,p,n);
    struct net_pkt *pkt=net_pkt_alloc_with_buffer(iface,n+MAC_HEADER,NET_AF_UNSPEC,0,K_NO_WAIT);
    if(!pkt) return;
    if(!net_pkt_write(pkt,f,n+MAC_HEADER))
        (void)api->tx(radio,IEEE802154_TX_MODE_CSMA_CA,pkt,pkt->frags);
    net_pkt_unref(pkt);
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
    uint64_t number=0,nonce;
#if defined(CONFIG_SOC_CC2652P)
    /* Driver reads the factory IEEE address, with CCFG override if configured. */
    const struct net_linkaddr *addr=net_if_get_link_addr(iface);
    if(addr->len==8) number=sys_get_be64(addr->addr);
#else
    uint8_t id[8];
    if(hwinfo_get_device_id(id,sizeof(id))==sizeof(id)) number=sys_get_be64(id);
#endif
    if(!number || sys_csrand_get(&nonce,sizeof(nonce))) fatal("\r\nIDENTITY/ENTROPY ERROR\r\n");
    zm_init(&modem,number,nonce,(struct zm_io){NULL,radio_out,serial_out});
    struct ieee802154_filter filter={.pan_id=PAN};
    if(api->filter(radio,true,IEEE802154_FILTER_TYPE_PAN_ID,&filter)) fatal("\r\nPAN ERROR\r\n");
    filter.short_addr=0xfffe;
    if(api->filter(radio,true,IEEE802154_FILTER_TYPE_SHORT_ADDR,&filter)) fatal("\r\nADDRESS ERROR\r\n");
    if(api->set_channel(radio,CHANNEL) || api->set_txpower(radio,5) || api->start(radio))
        fatal("\r\nRADIO START ERROR\r\n");
    uart_irq_callback_user_data_set(uart,uart_isr,NULL); uart_irq_rx_enable(uart);
    uint8_t input[128]; size_t pending=0;
    for(;;) {
        zm_tick(&modem,k_uptime_get());
        if(atomic_cas(&overrun,1,0)) { zm_overrun(&modem); pending=0;
            unsigned key=irq_lock(); ring_buf_reset(&serial_rx_queue); irq_unlock(key); }
        struct rx_frame f;
        while(!k_msgq_get(&radio_rx,&f,K_NO_WAIT)) {
            if(f.len>=MAC_HEADER+ZM_HEADER && sys_get_le16(f.bytes)==0xc841 &&
               sys_get_le16(f.bytes+3)==PAN && sys_get_le16(f.bytes+5)==0xffff &&
               !memcmp(f.bytes+7,f.bytes+MAC_HEADER+4,8))
                zm_receive(&modem,f.bytes+MAC_HEADER,f.len-MAC_HEADER);
        }
        if(!pending) { unsigned key=irq_lock(); pending=ring_buf_get(&serial_rx_queue,input,sizeof(input)); irq_unlock(key); }
        if(pending) { size_t n=zm_input(&modem,input,pending); pending-=n; memmove(input,input+n,pending); }
        k_sleep(K_MSEC(1));
    }
    return 0;
}

