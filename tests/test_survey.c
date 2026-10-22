/* SPDX-License-Identifier: MIT */
#include "modem.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct endpoint { struct zm_modem m; unsigned bytes,scans; };
static struct endpoint a,b;
struct packet { struct endpoint *to; uint8_t data[ZM_FRAME]; size_t n; };
static struct packet packets[256];
static unsigned head,tail,count,loss;
static int drop_phase=-1;
static uint64_t now;
static void radio(void *ctx,const uint8_t *p,size_t n) {
    struct endpoint *from=ctx,*to=from==&a?&b:&a; count++;
    if(from->m.channel!=to->m.channel || (loss && count%loss==0) ||
       (p[3]==ZM_SURVEY && p[ZM_HEADER]==drop_phase)) return;
    assert(tail-head<256); struct packet *q=&packets[tail++%256];
    q->to=to; q->n=n; memcpy(q->data,p,n);
}
static bool serial(void *ctx,const uint8_t *p,size_t n) { (void)p; ((struct endpoint *)ctx)->bytes+=(unsigned)n; return true; }
static bool channel(void *ctx,unsigned ch) { (void)ctx; (void)ch; return true; }
static int energy(void *ctx,unsigned duration) {
    struct endpoint *e=ctx; assert(duration==8); e->scans++;
    switch(e->m.channel) { case 25: return -45; case 15: return -65; case 20: return e==&a?-95:-90; case 26:return -80; default:return -75; }
}
static void advance(unsigned ms) {
    for(unsigned t=0;t<ms;t+=5) {
        now+=5; a.m.now=b.m.now=now;
        zm_tick(&a.m,now); zm_tick(&b.m,now);
        while(head<tail) { struct packet q=packets[head++%256]; zm_receive(&q.to->m,q.data,q.n); }
    }
}
static void setup(void) {
    memset(&a,0,sizeof(a)); memset(&b,0,sizeof(b)); head=tail=count=loss=0; now=0; drop_phase=-1;
    struct endpoint *ends[]={&a,&b};
    for(unsigned i=0;i<2;i++) {
        struct endpoint *e=ends[i]; zm_init(&e->m,i+1,10+i,(struct zm_io){e,radio,serial});
        zm_enable_link(&e->m,(struct zm_link_ops){e,channel,NULL,NULL,energy});
        e->m.state=ZM_CONNECTED; e->m.online=true; e->m.peer=2-i; e->m.session=1;
        e->m.caller=i==0; e->m.survey_due=100;
    }
}
int main(void) {
    setup(); loss=9; advance(18000);
    assert(a.m.channel==20 && b.m.channel==20 && a.m.survey_moves && b.m.survey_moves);
    assert(a.scans>=5 && b.scans>=5 && a.m.surveys>=5 && a.m.channel_energy[2]==-90);
    uint8_t bytes[1024]; memset(bytes,0x35,sizeof(bytes));
    unsigned before=b.bytes; assert(zm_input(&a.m,bytes,sizeof(bytes))==sizeof(bytes)); advance(60000);
    assert(b.bytes==before+sizeof(bytes) && a.m.state==ZM_CONNECTED && b.m.state==ZM_CONNECTED);
    setup(); drop_phase=2; advance(18000);
    assert(a.m.channel==25 && b.m.channel==25 && !a.m.survey_moves);
    assert(a.m.state==ZM_CONNECTED && b.m.state==ZM_CONNECTED);
    setup(); drop_phase=6; advance(24000);
    assert(a.m.state==ZM_CONNECTED && b.m.state==ZM_CONNECTED && a.m.channel==b.m.channel);
    puts("PASS: coordinated energy slots, worst-endpoint selection, hysteresis, lost commit/ACK recovery");
}
