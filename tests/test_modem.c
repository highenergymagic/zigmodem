/* SPDX-License-Identifier: MIT */
#include "modem.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct endpoint { struct zm_modem m; uint8_t output[262144]; size_t len; bool blocked; };
struct packet { uint8_t data[ZM_FRAME]; size_t len; struct endpoint *dst; };
static struct endpoint a,b,c;
static struct packet packets[4096];
static size_t head,tail;
static uint64_t clock_ms;
static unsigned sent,drop_every,duplicate_every,drop_type;
static bool disconnected;
static bool serial_out(void *ctx,const uint8_t *p,size_t n) {
    struct endpoint *e=ctx;
    if(e->blocked) return false;
    assert(e->len+n<sizeof(e->output)); memcpy(e->output+e->len,p,n); e->len+=n;
    e->output[e->len]=0; return true;
}
static void radio_out(void *ctx,const uint8_t *p,size_t n) {
    struct endpoint *src=ctx;
    sent++;
    if(disconnected || (drop_every && sent%drop_every==0)) return;
    if(drop_type==p[3]) { drop_type=0; return; }
    struct endpoint *all[]={&a,&b,&c};
    for(unsigned i=0;i<3;i++) if(all[i]!=src) {
        unsigned copies=duplicate_every && sent%duplicate_every==0?2:1;
        while(copies--) {
            assert(tail-head<4096); struct packet *q=&packets[tail++%4096];
            q->dst=all[i]; q->len=n; memcpy(q->data,p,n);
        }
    }
}
static void drain(void) {
    unsigned limit=0;
    while(head<tail) {
        struct packet p=packets[head++%4096];
        zm_receive(&p.dst->m,p.data,p.len); assert(++limit<10000);
    }
}
static void advance(unsigned ms) {
    for(unsigned i=0;i<ms;i+=10) {
        clock_ms+=10;
        zm_tick(&a.m,clock_ms); zm_tick(&b.m,clock_ms); zm_tick(&c.m,clock_ms); drain();
    }
}
static void cmd(struct endpoint *e,const char *s) {
    assert(zm_input(&e->m,(const uint8_t *)s,strlen(s))==strlen(s)); drain();
}
static void setup(void) {
    memset(&a,0,sizeof(a)); memset(&b,0,sizeof(b)); memset(&c,0,sizeof(c));
    head=tail=0; clock_ms=0; sent=drop_every=duplicate_every=drop_type=0; disconnected=false;
    zm_init(&a.m,123,1000,(struct zm_io){&a,radio_out,serial_out});
    zm_init(&b.m,456,2000,(struct zm_io){&b,radio_out,serial_out});
    zm_init(&c.m,789,3000,(struct zm_io){&c,radio_out,serial_out});
    cmd(&a,"ATE0\r"); cmd(&b,"ATE0\r"); cmd(&c,"ATE0\r");
    a.len=b.len=c.len=0;
}
static void connect_pair(void) {
    cmd(&b,"ATS0=1\r"); cmd(&a,"ATDT456\r"); advance(3000);
    assert(a.m.state==ZM_CONNECTED && b.m.state==ZM_CONNECTED);
    a.len=b.len=0;
}
static void commands(void) {
    setup(); cmd(&a,"AT+PHONUM?\r"); assert(strstr((char *)a.output,"123"));
    cmd(&a,"ATDT18446744073709551616\r"); assert(a.m.state==ZM_IDLE);
    cmd(&a,"ATDT123\r"); assert(a.m.state==ZM_IDLE);
    cmd(&a,"ATDT0\r"); assert(a.m.state==ZM_IDLE);
    cmd(&a,"AT&\r"); cmd(&a,"ATS7\r");
    assert(strstr((char *)a.output,"ERROR"));
    cmd(&a,"ATV0\r"); assert(a.output[a.len-2]=='0');
    cmd(&a,"ATQ1\r"); size_t n=a.len; cmd(&a,"AT\r"); assert(a.len==n);
    cmd(&a,"ATQ0V1\r");
    char huge[300]; memset(huge,'A',sizeof(huge)); huge[298]='\r'; huge[299]=0;
    cmd(&a,huge); cmd(&a,"AT\r"); assert(strstr((char *)a.output,"OK"));
    setup(); cmd(&a,"ATDT456\r"); assert(b.m.state==ZM_RINGING);
    cmd(&b,"ATA\r"); advance(100); assert(a.m.online && b.m.online);
}
static void binary_loss(void) {
    setup(); drop_every=7; duplicate_every=3; drop_type=ZM_CONFIRM; connect_pair();
    uint8_t input[65536]; for(size_t i=0;i<sizeof(input);i++) input[i]=(uint8_t)(i*71);
    size_t x=0,y=0;
    for(unsigned t=0;t<200000 && (x<sizeof(input) || y<sizeof(input) ||
        a.m.qlen || b.m.qlen || a.m.inflight || b.m.inflight);t++) {
        x+=zm_input(&a.m,input+x,sizeof(input)-x);
        y+=zm_input(&b.m,input+y,sizeof(input)-y);
        advance(10);
    }
    assert(x==sizeof(input) && y==sizeof(input));
    assert(a.len==sizeof(input) && b.len==sizeof(input));
    assert(!memcmp(a.output,input,sizeof(input)) && !memcmp(b.output,input,sizeof(input)));
}
static void escape_backpressure(void) {
    setup(); connect_pair();
    cmd(&a,"abc+++def"); advance(500); assert(b.len==9);
    advance(1100); cmd(&a,"+++"); advance(1010); assert(!a.m.online);
    cmd(&a,"AT+PHONUM?\r"); assert(strstr((char *)a.output,"123"));
    cmd(&b,"held"); advance(1000); assert(b.m.inflight);
    cmd(&a,"ATO\r"); advance(500); assert(!b.m.inflight);
    assert(strstr((char *)a.output,"held"));
    advance(1100); cmd(&a,"++"); advance(1100); assert(a.m.online);
    advance(500); assert(b.output[b.len-1]=='+');
    b.blocked=true; cmd(&a,"backpressure"); advance(5000); assert(a.m.inflight);
    b.blocked=false; advance(500); assert(!a.m.inflight);
    advance(1100); cmd(&a,"+++"); advance(1010); drop_type=ZM_CLOSE;
    cmd(&a,"ATH\r"); advance(1000); assert(b.m.state==ZM_IDLE && a.m.state==ZM_IDLE);
}
static void lifecycle(void) {
    setup(); drop_type=ZM_ACCEPT; connect_pair();
    cmd(&c,"ATDT456\r"); assert(c.m.state==ZM_IDLE); assert(strstr((char *)c.output,"BUSY"));
    disconnected=true; advance(11000); assert(!a.m.online && !b.m.online);
    assert(strstr((char *)a.output,"NO CARRIER"));
    setup(); cmd(&a,"ATS7=1\rATDT789\r"); advance(1500);
    assert(strstr((char *)a.output,"NO ANSWER"));
    setup(); disconnected=true; cmd(&a,"ATDT456\r"); cmd(&b,"ATDT123\r");
    disconnected=false; advance(1000); assert(a.m.state==ZM_IDLE && b.m.state==ZM_IDLE);
    setup(); connect_pair(); uint64_t old=a.m.session;
    zm_overrun(&a.m); drain(); advance(4000); connect_pair(); assert(a.m.session!=old);
    uint8_t f[ZM_HEADER]={ 'Z','M',1,ZM_CLOSE };
    f[4]=123; f[12]=200; f[13]=1; /* destination 456 */
    for(unsigned i=0;i<8;i++) f[20+i]=(uint8_t)(old>>(i*8));
    zm_receive(&b.m,f,sizeof(f)); assert(b.m.state==ZM_CONNECTED);
    /* Truncated and random frames must not crash or change the session. */
    for(size_t n=0;n<ZM_HEADER;n++) zm_receive(&b.m,f,n);
    assert(b.m.state==ZM_CONNECTED);
}
int main(void) {
    commands(); binary_loss(); escape_backpressure(); lifecycle();
    puts("PASS: commands, manual/auto answer, lossy duplex 128 KiB, duplicates, escape,");
    puts("      backpressure, busy, timeout, hangup retry, overrun, stale/malformed frames");
    return 0;
}
