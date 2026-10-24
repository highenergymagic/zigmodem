/* SPDX-License-Identifier: MIT */
#include "modem.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint8_t saved[3][ZM_PROFILE_SIZE]; static size_t sizes[3]; static bool fail_write;
static char output[16384]; static size_t used;
static struct zm_modem m;
static bool serial(void *ctx,const uint8_t *p,size_t n) {
    (void)ctx; assert(used+n<sizeof(output)); memcpy(output+used,p,n); used+=n; output[used]=0; return true;
}
static void radio(void *ctx,const uint8_t *p,size_t n) { (void)ctx;(void)p;(void)n; }
static int read_profile(void *ctx,unsigned slot,uint8_t *p,size_t n) {
    (void)ctx; assert(slot<3); if(!sizes[slot]) return -1;
    if(n>sizes[slot]) n=sizes[slot]; memcpy(p,saved[slot],n); return (int)n;
}
static bool write_profile(void *ctx,unsigned slot,const uint8_t *p,size_t n) {
    (void)ctx; if(fail_write) return false; assert(slot<3 && n<=ZM_PROFILE_SIZE);
    memcpy(saved[slot],p,n); sizes[slot]=n; return true;
}
static void boot(void) {
    zm_init(&m,123,456,(struct zm_io){NULL,radio,serial});
    zm_enable_link(&m,(struct zm_link_ops){0});
    zm_set_storage(&m,(struct zm_storage_ops){NULL,read_profile,write_profile}); used=0; output[0]=0;
}
static void cmd(const char *s) {
    used=0; output[0]=0; assert(zm_input(&m,(const uint8_t *)s,strlen(s))==strlen(s));
}
int main(void) {
    boot(); cmd("ATE0S0=2&K4W1&W0\r"); assert(strstr(output,"OK"));
    cmd("AT&F\r"); assert(!m.flow_tx && !m.flow_rx && !m.autoanswer);
    cmd("ATZ\r"); assert(m.autoanswer==2 && m.flow_tx && m.flow_rx && m.report_mode==1 && !m.echo);
    cmd("AT+COMPAT=USR\r"); assert(m.usr_commands);
    cmd("AT&K0&H2&I0&A3S0=3&W1Y1\r"); assert(strstr(output,"OK"));
    assert(!m.compression && m.flow_tx && !m.flow_rx && m.boot_profile==1);
    boot(); assert(m.usr_commands && m.autoanswer==3 && !m.compression && m.report_mode==3);
    cmd("ATZ1\r"); assert(!m.usr_commands && m.autoanswer==2 && m.compression);
    cmd("AT+COMPAT=USR\r"); cmd("AT&F2&H2&I2&K1&M5&B1\r"); assert(strstr(output,"OK"));
    assert(m.flow_tx && m.flow_rx && m.compression);
    cmd("AT&F1\r"); assert(strstr(output,"ERROR"));
    cmd("AT&K4\r"); assert(strstr(output,"ERROR"));
    cmd("AT&N14\r"); assert(m.min_profile==13 && m.max_profile==13 && !m.adaptive);
    cmd("AT&U6\r"); assert(m.min_profile==5 && m.max_profile==13 && m.adaptive);
    cmd("AT&N2\r"); assert(strstr(output,"ERROR") && m.max_profile==13);
    cmd("AT&N38\r"); assert(m.max_profile==16 && zm_rate(m.max_profile)==56000);
    cmd("AT&N39\r"); assert(strstr(output,"ERROR") && m.max_profile==16);
    cmd("AT&U0&N0\r"); assert(m.min_profile==0 && m.max_profile==13);
    cmd("ATY4\r"); boot(); assert(m.usr_commands && m.flow_tx && m.flow_rx);
    cmd("ATY2\r"); boot(); assert(m.usr_commands && !m.flow_tx && !m.flow_rx);
    cmd("ATY3\r"); assert(strstr(output,"ERROR") && m.boot_profile==2);
    cmd("ATY1Z\r"); assert(m.autoanswer==3 && m.usr_commands);
    cmd("AT&F2\r");
    const char *unsupported[]={"AT&D2\r","AT&C1\r","AT&H1\r","AT&R2\r","AT&M0\r","AT+FCLASS=1\r","ATI10\r"};
    for(unsigned i=0;i<sizeof(unsupported)/sizeof(*unsupported);i++) {
        cmd(unsupported[i]); assert(strstr(output,"ERROR"));
    }
    cmd("ATI4I5I6I7I9I11\r"); assert(strstr(output,"STORED PROFILE") && strstr(output,"EXTENDED LINK"));
    cmd("ATI8\r"); assert(strstr(output,"ERROR"));
    cmd("ATI2\r"); assert(strstr(output,"OK"));
    m.firmware_crc_valid=true; m.firmware_crc=123456789;
    cmd("ATI1\r"); assert(strstr(output,"123456789"));
    cmd("ATI3\r"); cmd("A/"); assert(strstr(output,"Zigmodem 0.5"));
    fail_write=true; cmd("AT&W0\r"); assert(strstr(output,"ERROR")); fail_write=false;
    uint8_t snapshot[ZM_PROFILE_SIZE]; zm_profile_encode(&m,snapshot);
    for(unsigned i=0;i<ZM_PROFILE_SIZE;i++) {
        uint8_t corrupt[ZM_PROFILE_SIZE]; memcpy(corrupt,snapshot,sizeof(corrupt)); corrupt[i]^=0x80;
        assert(!zm_profile_decode(&m,corrupt));
    }
    assert(m.flow_tx && m.flow_rx && m.compression);
    cmd("ATS3=59\r"); cmd("ATS4=33;"); assert(strstr(output,";!OK;!"));
    cmd("ATS3=13;"); cmd("ATS4=10\r"); assert(strstr(output,"\r\nOK\r\n"));
    cmd("ATS1?\r"); assert(strstr(output,"OK"));
    cmd("ATS1=4\r"); assert(strstr(output,"ERROR"));
    cmd("AT&V\r"); assert(strstr(output,"ACTIVE PROFILE") && strstr(output,"STORED PROFILE 1"));
    cmd("AT&A3\r"); m.state=ZM_CONNECTED; m.online=false; m.profile=13;
    cmd("ATO\r"); assert(strstr(output,"CARRIER 28800") && strstr(output,"PROTOCOL: ZIGMODEM-ARQ") && strstr(output,"COMPRESSION: HEATSHRINK") && strstr(output,"CONNECT 28800"));
    m.online=false;
    cmd("ATV0\r"); m.state=ZM_CONNECTED; m.online=false; m.profile=13;
    cmd("ATO\r"); assert(strstr(output,"107\r")); m.online=false;
    cmd("ATV1S19=1\r"); m.last_data_at=0; zm_tick(&m,60001);
    assert(m.state==ZM_CLOSING && strstr(output,"NO CARRIER"));
    boot(); cmd("AT&F2S13=128\r");
    m.state=ZM_CONNECTED; m.online=true; m.now=2000; m.last_input=0;
    cmd("+++"); zm_tick(&m,3001);
    assert(m.state==ZM_CLOSING && strstr(output,"NO CARRIER"));
    puts("PASS: native/USR profiles, boot/reset persistence, split flow, inquiries, repeat, custom delimiters, failed/corrupt writes");
}
