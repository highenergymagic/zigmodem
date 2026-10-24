/* SPDX-License-Identifier: MIT */
#include "modem.h"
#include <string.h>
/* Versioned byte encoding: no native structs, padding or private key material. */
static uint32_t checksum(const uint8_t *p,size_t n) {
    uint32_t c=0xffffffffu;
    while(n--) { c^=*p++; for(unsigned i=0;i<8;i++) c=(c>>1)^((0u-(c&1u))&0xedb88320u); }
    return ~c;
}
void zm_profile_encode(const struct zm_modem *m,uint8_t p[ZM_PROFILE_SIZE]) {
    memset(p,0,ZM_PROFILE_SIZE); memcpy(p,"ZMP",3); p[3]=2;
    p[4]=m->echo; p[5]=m->verbose; p[6]=m->quiet;
    p[7]=m->flow_tx; p[8]=m->flow_rx; p[9]=m->compression;
    p[10]=m->adaptive; p[11]=m->survey_enabled; p[12]=m->usr_commands;
    p[13]=(uint8_t)m->autoanswer; p[14]=(uint8_t)m->escape_char;
    p[15]=(uint8_t)m->dial_seconds; p[16]=(uint8_t)(m->guard_ms/20);
    p[17]=(uint8_t)m->min_profile; p[18]=(uint8_t)m->max_profile;
    p[19]=(uint8_t)m->report_mode; p[20]=(uint8_t)m->result_level;
    p[21]=(uint8_t)m->command_cr; p[22]=(uint8_t)m->response_lf; p[23]=(uint8_t)m->command_bs;
    p[24]=(uint8_t)m->inactivity_minutes; p[25]=m->escape_hangup;
    p[26]=(uint8_t)m->usr_ceiling; p[27]=(uint8_t)m->usr_floor;
    p[28]=m->power_auto; p[29]=(uint8_t)(m->power_ceiling+20);
    uint32_t c=checksum(p,36);
    for(unsigned i=0;i<4;i++) p[36+i]=(uint8_t)(c>>(8*i));
}
bool zm_profile_valid(const uint8_t p[ZM_PROFILE_SIZE]) {
    uint32_t c=0; for(unsigned i=0;i<4;i++) c|=(uint32_t)p[36+i]<<(8*i);
    if(memcmp(p,"ZMP",3) || (p[3]!=1 && p[3]!=2) || c!=checksum(p,36)) return false;
    for(unsigned i=4;i<=12;i++) if(p[i]>1) return false;
    for(unsigned i=p[3]==1?28:30;i<36;i++) if(p[i]) return false;
    if(p[3]==2) {
        if(p[28]>1) return false;
        switch(p[29]) { case 0: case 5: case 10: case 15: case 20: case 25: case 34: case 37: case 40: break; default:return false; }
    }
    if((p[26]>16 && p[26]!=38) || (p[27]>16 && p[27]!=38)) return false;
    if(p[25]>1) return false;
    for(unsigned i=21;i<24;i++) if(p[i]>127) return false;
    if(!p[15] || !p[16] || p[17]>p[18] || p[18]>=ZM_RATE_COUNT || p[19]>3 || p[20]>4) return false;
    return true;
}
bool zm_profile_decode(struct zm_modem *m,const uint8_t p[ZM_PROFILE_SIZE]) {
    if(!zm_profile_valid(p)) return false;
    bool automatic=p[3]==1 || p[28]; int ceiling=p[3]==1?5:(int)p[29]-20;
    if(m->link.power && !zm_power_config(m,automatic,ceiling)) return false;
    m->power_auto=automatic; m->power_ceiling=ceiling;
    m->echo=p[4]; m->verbose=p[5]; m->quiet=p[6];
    m->flow_tx=p[7]; m->flow_rx=p[8]; m->software_flow=m->flow_tx && m->flow_rx;
    m->compression=p[9]; m->adaptive=p[10]; m->survey_enabled=p[11]; m->usr_commands=p[12];
    m->autoanswer=p[13]; m->escape_char=p[14]; m->dial_seconds=p[15]; m->guard_ms=p[16]*20u;
    m->min_profile=p[17]; m->max_profile=p[18]; m->report_mode=p[19]; m->result_level=p[20];
    m->command_cr=p[21]; m->response_lf=p[22]; m->command_bs=p[23];
    m->inactivity_minutes=p[24]; m->escape_hangup=p[25];
    m->usr_ceiling=p[26]; m->usr_floor=p[27];
    return true;
}
bool zm_save_profile(struct zm_modem *m,unsigned slot) {
    if(slot>1 || !m->storage.write || !m->storage.read) return false;
    uint8_t p[ZM_PROFILE_SIZE],check[ZM_PROFILE_SIZE]; zm_profile_encode(m,p);
    return m->storage.write(m->storage.ctx,slot,p,sizeof(p)) &&
        m->storage.read(m->storage.ctx,slot,check,sizeof(check))==(int)sizeof(check) && !memcmp(p,check,sizeof(p));
}
bool zm_load_profile(struct zm_modem *m,unsigned slot) {
    if(slot>1 || !m->storage.read) return false;
    uint8_t p[ZM_PROFILE_SIZE];
    return m->storage.read(m->storage.ctx,slot,p,sizeof(p))==(int)sizeof(p) && zm_profile_decode(m,p);
}
bool zm_select_profile(struct zm_modem *m,unsigned slot) {
    if(slot>4 || slot==3 || !m->storage.write || !m->storage.read) return false;
    uint8_t p[5]={'Z','B',1,(uint8_t)slot,(uint8_t)m->usr_commands},check[5];
    if(!m->storage.write(m->storage.ctx,2,p,5) || m->storage.read(m->storage.ctx,2,check,5)!=5 || memcmp(p,check,5)) return false;
    m->boot_profile=slot; return true;
}
void zm_set_storage(struct zm_modem *m,struct zm_storage_ops ops) {
    m->storage=ops; uint8_t p[5]; m->boot_profile=0;
    if(ops.read && ops.read(ops.ctx,2,p,5)==5 && !memcmp(p,"ZB\1",3) && p[3]<=4 && p[3]!=3 && p[4]<=1) {
        m->boot_profile=p[3]; m->usr_commands=p[4];
    }
    if(m->boot_profile==4) m->software_flow=m->flow_tx=m->flow_rx=true;
    else (void)zm_load_profile(m,m->boot_profile);
}
