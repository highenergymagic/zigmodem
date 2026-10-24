/* SPDX-License-Identifier: MIT */
#include "modem.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct zm_modem m;
static int applied;
static bool fail;
static char output[512];
static bool power(void *ctx,int dbm) { (void)ctx; if(fail) return false; applied=dbm; return true; }
static bool serial(void *ctx,const uint8_t *p,size_t n) {
    (void)ctx; assert(n<sizeof(output)); memcpy(output,p,n); output[n]=0; return true;
}
static void radio(void *ctx,const uint8_t *p,size_t n) { (void)ctx; (void)p; (void)n; }
static void setup(void) {
    fail=false; applied=5;
    zm_init(&m,1,42,(struct zm_io){NULL,radio,serial});
    zm_enable_link(&m,(struct zm_link_ops){.power=power});
    assert(zm_power_config(&m,true,20));
    m.state=ZM_CONNECTED; m.peer=2; m.session=1; m.last_rssi=-50;
}
static void step(bool bad,bool peer_bad,bool feedback) {
    m.now+=2000; m.last_rx=m.now; m.bytes_acked+=4096; if(bad) m.retries+=5;
    uint8_t report[]={1,(bad || peer_bad)?48:78,peer_bad,45};
    if(feedback) zm_power_receive(&m,report);
    zm_power_tick(&m);
}
static void cmd(const char *s) { m.online=false; zm_input(&m,(const uint8_t *)s,strlen(s)); }
struct end { struct zm_modem modem; unsigned received; };
static struct end ends[2];
struct packet { unsigned dest; size_t n; uint8_t data[ZM_FRAME]; };
static struct packet packets[128];
static unsigned head,tail;
static void link_radio(void *ctx,const uint8_t *p,size_t n) {
    struct end *e=ctx; unsigned i=e==ends?0:1;
    /* Different link budgets in each direction, including missing ACKs. */
    if(e->modem.tx_power<(i==0?14:17)) return;
    assert(tail-head<128); struct packet *q=&packets[tail++%128];
    q->dest=1-i; q->n=n; memcpy(q->data,p,n);
}
static bool link_serial(void *ctx,const uint8_t *p,size_t n) {
    struct end *e=ctx;
    for(size_t i=0;i<n;i++) assert(p[i]==(uint8_t)(e->received++*73));
    return true;
}
static void transfer(void) {
    memset(ends,0,sizeof(ends)); head=tail=0; fail=false;
    for(unsigned i=0;i<2;i++) {
        struct zm_modem *x=&ends[i].modem;
        zm_init(x,i+1,42,(struct zm_io){&ends[i],link_radio,link_serial});
        zm_enable_link(x,(struct zm_link_ops){.power=power}); assert(zm_power_config(x,true,20));
        x->state=ZM_CONNECTED; x->online=true; x->peer=2-i; x->session=1; x->profile=13;
    }
    unsigned queued=0;
    for(unsigned now=5;now<=160000;now+=5) {
        if(queued<4096) {
            uint8_t data[64]; for(unsigned i=0;i<64;i++) data[i]=(uint8_t)((queued+i)*73);
            queued+=(unsigned)zm_input(&ends[0].modem,data,sizeof(data));
        }
        for(unsigned i=0;i<2;i++) { ends[i].modem.last_rssi=-65; zm_tick(&ends[i].modem,now); }
        while(head<tail) { struct packet q=packets[head++%128]; zm_receive(&ends[q.dest].modem,q.data,q.n); }
    }
    assert(queued==4096 && ends[1].received==4096 && ends[0].modem.bytes_acked==4096);
    assert(ends[0].modem.tx_power>=14 && ends[1].modem.tx_power>=17);
    assert(ends[0].modem.state==ZM_CONNECTED && ends[1].modem.state==ZM_CONNECTED);
}
int main(void) {
    setup(); step(true,false,true); assert(applied==5);
    step(true,false,true); assert(applied==14);
    step(true,false,true); assert(applied==14);
    step(true,false,true); assert(applied==17);
    step(true,false,true); step(true,false,true); assert(applied==20);
    for(unsigned i=0;i<10;i++) step(true,true,true);
    assert(applied==20);
    setup(); step(false,true,true); step(false,true,true); assert(applied==14);
    setup();
    for(unsigned i=0;i<10;i++) {
        m.now+=2000; m.last_rx=m.now; m.retries+=5;
        uint8_t strong[]={1,78,1,45}; zm_power_receive(&m,strong); zm_power_tick(&m);
    }
    assert(applied==5); /* strong-signal interference is not a power deficit */
    setup(); for(unsigned i=0;i<15;i++) step(false,false,true); assert(applied==5);
    step(false,false,true); assert(applied==0);
    for(unsigned i=0;i<100;i++) step(false,false,false); assert(applied==0);
    setup(); assert(zm_power_config(&m,false,-20));
    for(unsigned i=0;i<30;i++) step(true,true,true); assert(applied==-20);
    assert(!zm_power_config(&m,true,21) && !zm_power_config(&m,false,-19));
    setup(); fail=true; step(true,false,true); step(true,false,true);
    assert(m.tx_power==5 && m.power_errors==1);
    setup(); m.survey_state=1; for(unsigned i=0;i<20;i++) step(true,true,true); assert(applied==5);
    setup(); m.now=100; uint8_t invalid[]={1,200,0,45}; zm_power_receive(&m,invalid); assert(!m.power_feedback);
    /* Unauthenticated/wrong-session feedback cannot reach the controller. */
    uint8_t f[ZM_HEADER+4]={'Z','M',1,ZM_POWER}; f[4]=2; f[12]=1; f[20]=2;
    f[ZM_HEADER]=1; f[ZM_HEADER+1]=78; f[ZM_HEADER+2]=1; f[ZM_HEADER+3]=45;
    zm_receive(&m,f,sizeof(f)); assert(!m.power_feedback);
    f[20]=1; zm_receive(&m,f,sizeof(f)); assert(m.power_feedback==100 && m.power_pressure);
    setup(); cmd("AT+POWER=FIXED,-15\r"); assert(strstr(output,"OK") && applied==-15);
    cmd("AT+POWER=AUTO,20\r"); assert(strstr(output,"OK") && applied==5 && m.power_ceiling==20);
    cmd("AT+POWER=AUTO,21\r"); assert(strstr(output,"ERROR") && m.power_ceiling==20);
    m.state=ZM_IDLE; cmd("AT+MS=1,300,28800\r"); assert(m.power_ceiling==20);
    uint8_t p[ZM_PROFILE_SIZE]; zm_profile_encode(&m,p);
    assert(zm_power_config(&m,false,-20)); assert(zm_profile_decode(&m,p));
    assert(m.power_auto && m.power_ceiling==20 && applied==5);
    /* Migrate an actual version-1 record, with its reserved bytes and CRC. */
    p[3]=1; p[28]=p[29]=0;
    uint32_t crc=0xffffffffu;
    for(unsigned i=0;i<36;i++) {
        crc^=p[i]; for(unsigned j=0;j<8;j++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    crc=~crc; for(unsigned i=0;i<4;i++) p[36+i]=(uint8_t)(crc>>(8*i));
    assert(zm_profile_decode(&m,p) && m.power_auto && m.power_ceiling==5 && applied==5);
    setup(); m.state=ZM_DIALING; m.now=4000; zm_power_tick(&m); assert(applied==14);
    m.state=ZM_ANSWERING; m.now=8000; zm_power_tick(&m); assert(applied==17);
    m.state=ZM_IDLE; m.now=12000; zm_power_tick(&m); assert(applied==5);
    m.secure_stage=2; m.now=13000; zm_power_tick(&m); assert(applied==5);
    m.now=17000; zm_power_tick(&m); assert(applied==14);
    m.secure_stage=0; m.now=21000; zm_power_tick(&m); assert(applied==5);
    transfer();
    puts("PASS: power hysteresis, asymmetric feedback, ceilings, stale reports, fixed mode, driver failure, profiles");
}
