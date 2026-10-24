/* SPDX-License-Identifier: MIT */
#include "secure.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct end { struct zs_link s; struct zm_modem m; uint8_t output[8192], pin[16]; size_t len; bool pinned; };
static struct end a,b;
struct packet { struct end *dst; uint8_t f[ZS_WIRE_MAX]; size_t n; };
static struct packet q[4096],saved[4];
static unsigned head,tail,count,drop,save_count;
static uint64_t now;
static bool capture;
static unsigned jammed;
static bool change(void *ctx,unsigned channel) {
    struct end *e=ctx; (void)channel;
    return !(e->s.hs && e->s.now-e->s.started<2000);
}
static void wire(void *ctx,const uint8_t *p,size_t n) {
    assert(n<=108);
    struct end *e=ctx; count++;
    struct end *other=e==&a?&b:&a;
    if((drop && count%drop==0) || e->m.channel!=other->m.channel || e->m.channel==jammed) return;
    assert(tail-head<4096); struct packet *x=&q[tail++%4096];
    x->dst=e==&a?&b:&a; x->n=n; memcpy(x->f,p,n);
    if(capture && p[3]==4 && save_count<4) saved[save_count++]=*x;
}
static bool serial(void *ctx,const uint8_t *p,size_t n) {
    struct end *e=ctx; assert(e->len+n<sizeof(e->output));
    memcpy(e->output+e->len,p,n); e->len+=n; e->output[e->len]=0; return true;
}
static void plain(void *ctx,const uint8_t *p,size_t n) { zm_receive(&((struct end *)ctx)->m,p,n); }
static bool accept(void *ctx) { return ((struct end *)ctx)->m.state==ZM_IDLE; }
static bool trust(void *ctx,const uint8_t id[16]) {
    struct end *e=ctx;
    if(e->pinned) return !memcmp(e->pin,id,16);
    memcpy(e->pin,id,16); e->pinned=true; return true;
}
static bool lookup(void *ctx,unsigned index,uint8_t id[16]) {
    struct end *e=ctx;
    if(index!=1 || !e->pinned) return false;
    memcpy(id,e->pin,16); return true;
}
static void radio(void *ctx,const uint8_t *p,size_t n) {
    struct end *e=ctx; zs_send(&e->s,p,n,e->m.peer_identity);
}
static void drain(void) {
    unsigned budget=0;
    while(head<tail) { struct packet x=q[head++%4096]; zs_receive(&x.dst->s,x.f,x.n); assert(++budget<10000); }
}
static void advance(unsigned ms) {
    for(unsigned i=0;i<ms;i+=10) {
        now+=10; zs_tick(&a.s,now); zs_tick(&b.s,now);
        zm_tick(&a.m,now); zm_tick(&b.m,now); drain();
    }
}
static void cmd(struct end *e,const char *p) { assert(zm_input(&e->m,(const uint8_t *)p,strlen(p))==strlen(p)); drain(); }
static void setup(void) {
    zs_reset(&a.s); zs_reset(&b.s); memset(&a,0,sizeof(a)); memset(&b,0,sizeof(b));
    head=tail=count=drop=save_count=jammed=0; now=0; capture=false;
    struct end *ends[]={&a,&b};
    for(unsigned i=0;i<2;i++) {
        uint8_t key[32]; memset(key,(int)(i+1),sizeof(key)); struct end *e=ends[i];
        assert(zs_init(&e->s,key,(struct zs_io){e,wire,plain,accept,trust}));
        e->s.compression=true;
        zm_init(&e->m,e->s.number,100+i,(struct zm_io){e,radio,serial});
        zm_set_identity(&e->m,e->s.identity); cmd(e,"ATE0\r"); e->len=0;
        zm_enable_link(&e->m,(struct zm_link_ops){e,change,trust,lookup});
        cmd(e,"AT+MS=0,300,300\rAT%C0\rAT&K4\r");
        assert(e->m.max_profile==0 && !e->m.compression && e->m.software_flow);
        cmd(e,"AT&F\rATE0\r");
        assert(e->m.max_profile==13 && e->m.compression && !e->m.software_flow);
    }
}
static void dial(bool wrong) {
    char s[40]="ATDT";
    for(unsigned i=0;i<16;i++) sprintf(s+4+i*2,"%02X",b.s.identity[i]);
    if(wrong) s[35]=s[35]=='0'?'1':'0'; /* Same routing prefix, wrong authenticated identity. */
    strcat(s,"\r"); cmd(&b,"ATS0=1\r"); cmd(&a,s);
}
int main(void) {
    setup(); drop=7; dial(false); advance(60000);
    assert(a.m.online && b.m.online && a.s.ready && b.s.ready);
    a.len=b.len=0; uint8_t data[1024]; for(unsigned i=0;i<sizeof(data);i++) data[i]=(uint8_t)i;
    assert(zm_input(&a.m,data,sizeof(data))==sizeof(data));
    assert(zm_input(&b.m,data,sizeof(data))==sizeof(data)); advance(90000);
    assert(a.len==sizeof(data) && b.len==sizeof(data));
    assert(!memcmp(a.output,data,sizeof(data)) && !memcmp(b.output,data,sizeof(data)));
    drop=0; capture=true; cmd(&a,"secret payload"); advance(1000); capture=false;
    size_t before=b.len; assert(save_count>=1);
    for(unsigned i=0;i<save_count;i++) zs_receive(&b.s,saved[i].f,saved[i].n);
    assert(b.len==before);
    /* An unauthenticated high counter must not prevent later legitimate traffic. */
    for(unsigned i=0;i<save_count;i++) {
        saved[i].f[4]=0xff; saved[i].f[5]=0xff; saved[i].f[saved[i].n-1]^=1;
        zs_receive(&b.s,saved[i].f,saved[i].n);
    }
    cmd(&a,"still works"); advance(1000); assert(b.len==before+11);
    uint64_t session=a.m.session; jammed=a.m.channel; before=b.len;
    cmd(&a,"survives channel loss"); advance(20000);
    assert(a.m.online && b.m.online && a.m.session==session && b.len==before+21);
    assert(a.m.channel==b.m.channel && a.m.channel!=jammed && a.m.retrains>1);
    advance(1200); cmd(&a,"+++"); advance(1100); assert(!a.m.online);
    jammed=a.m.channel; advance(20000);
    assert(a.m.state==ZM_CONNECTED && !a.m.online);
    cmd(&a,"ATO1\r"); advance(20000); assert(a.m.online);
    setup(); dial(true); advance(5000); assert(!a.m.online && !b.m.online && b.m.state==ZM_IDLE);
    /* Short numeric calls pin full identities; a changed pin fails closed. */
    setup(); char numeric[32]; snprintf(numeric,sizeof(numeric),"ATDT%llu\r",(unsigned long long)b.m.number);
    assert(b.m.number>=1000000000 && b.m.number<=9999999999ULL);
    cmd(&b,"ATS0=1\r"); cmd(&a,numeric); advance(10000);
    assert(a.m.online && b.m.online && a.pinned && b.pinned);
    advance(1200); cmd(&a,"+++"); advance(1100); cmd(&a,"ATH\r"); advance(4000);
    cmd(&a,"ATDT1\r"); advance(10000); assert(a.m.online && b.m.online);
    advance(1200); cmd(&a,"+++"); advance(1100);
    /* CLOSED arriving between UART chunks must not erase the next command. */
    assert(zm_input(&a.m,(const uint8_t *)"ATH\rAT+STA",10)==10);
    drain(); assert(a.m.command_len==6); a.len=0;
    cmd(&a,"TS?\r"); assert(strstr((const char *)a.output,"TX=")); advance(4000);
    a.pin[15]^=1; cmd(&a,numeric); advance(10000); assert(!a.m.online && !b.m.online);
    /* New responder boot must not accept recordings from an old session. */
    setup(); dial(false); advance(4000); capture=true; cmd(&a,"old"); advance(1000); capture=false;
    zs_reset(&b.s); b.m.state=ZM_IDLE; before=b.len;
    for(unsigned i=0;i<save_count;i++) zs_receive(&b.s,saved[i].f,saved[i].n);
    assert(b.len==before);
    zs_reset(&a.s); zs_reset(&b.s);
    puts("PASS: encrypted lossy duplex, fingerprint binding, replay, forged nonce, reboot isolation");
    return 0;
}
