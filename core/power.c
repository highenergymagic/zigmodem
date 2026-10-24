/* SPDX-License-Identifier: MIT */
#include "modem.h"

/* Discrete settings shared by the two boards. Never round above the ceiling. */
static const int levels[]={-20,-15,-10,-5,0,5,14,17,20};
static bool supported(int dbm) {
    for(unsigned i=0;i<sizeof(levels)/sizeof(levels[0]);i++) if(levels[i]==dbm) return true;
    return false;
}
static bool apply(struct zm_modem *m,int dbm) {
    if(!m->link.power || !m->link.power(m->link.ctx,dbm)) { m->power_errors++; m->power_changed=m->now; return false; }
    if(m->tx_power!=dbm) m->power_changes++;
    m->tx_power=dbm; m->power_changed=m->now; m->power_good=0;
    return true;
}
static void raise_power(struct zm_modem *m) {
    if(m->now-m->power_changed<4000) return;
    for(unsigned i=0;i<sizeof(levels)/sizeof(levels[0]);i++)
        if(levels[i]>m->tx_power && levels[i]<=m->power_ceiling) { (void)apply(m,levels[i]); break; }
}
bool zm_power_config(struct zm_modem *m,bool automatic,int dbm) {
    if(!supported(dbm)) return false;
    int initial=automatic && dbm>5?5:dbm;
    if(!apply(m,initial)) return false;
    m->power_auto=automatic; m->power_ceiling=dbm;
    m->power_at=m->now; m->power_feedback=0; m->power_pressure=false;
    m->power_retries=m->retries; m->power_acked=m->bytes_acked;
    return true;
}
void zm_power_receive(struct zm_modem *m,const uint8_t *p) {
    if(p[0]!=1 || p[1]>127 || p[2]>1 || p[3]>60) return;
    m->peer_rssi=(int)p[1]-128; m->power_pressure=p[2]!=0;
    m->peer_power=(int)p[3]-40; m->power_feedback=m->now;
}
void zm_power_tick(struct zm_modem *m) {
    if(!m->link.power) return;
    bool handshake=m->state==ZM_IDLE && m->secure_stage>0 && m->secure_stage<4;
    if(!handshake) m->power_handshake_at=0;
    else if(!m->power_handshake_at) m->power_handshake_at=m->now;
    if(m->state!=ZM_CONNECTED) { m->power_feedback=0; m->power_good=0;
        /* A weak initial call must be able to reach the peer before feedback exists. */
        if(m->power_auto && (m->state==ZM_DIALING || m->state==ZM_ANSWERING) && m->now-m->started>=4000)
            raise_power(m);
        /* The answering side is still IDLE while Noise establishes encryption. */
        if(m->power_auto && handshake && m->now-m->power_handshake_at>=4000) raise_power(m);
        int initial=m->power_ceiling>5?5:m->power_ceiling;
        if(m->state==ZM_IDLE && !handshake && m->power_auto && m->tx_power!=initial && m->now-m->power_changed>=4000) (void)apply(m,initial);
        return; }
    if(m->survey_state || m->now-m->power_at<2000) return;
    uint32_t retries=m->retries-m->power_retries, acked=m->bytes_acked-m->power_acked;
    m->power_retries=m->retries; m->power_acked=m->bytes_acked; m->power_at=m->now;
    bool bad=retries>=4 || m->now-m->last_rx>=2000;
    uint8_t report[4]={1,(uint8_t)(m->last_rssi<0 && m->last_rssi>=-128?m->last_rssi+128:0),
                       bad?1:0,(uint8_t)(m->tx_power+40)};
    zm_control(m,ZM_POWER,0,report,sizeof(report));
    if(!m->power_auto) return;
    bool fresh=m->power_feedback && m->now-m->power_feedback<6000;
    if(bad || (fresh && m->power_pressure)) {
        m->power_good=0;
        /* Give the existing channel survey a chance to escape interference. */
        if(m->caller && m->survey_enabled && m->survey_due>m->now+1000) m->survey_due=m->now+1000;
        /* Strong received signal plus retries usually needs cleaner airtime,
         * not a stronger transmitter. Silence still permits recovery. */
        if(!fresh || m->peer_rssi<=-60 || m->now-m->last_rx>=2000) raise_power(m);
    } else if(fresh && !m->training && !m->recovering && !retries && acked>=1024 && m->peer_rssi>-70) {
        if(!m->power_good) m->power_good=m->now;
        if(m->now-m->power_good<30000 || m->now-m->power_changed<30000) return;
        for(unsigned i=1;i<sizeof(levels)/sizeof(levels[0]);i++)
            if(levels[i]==m->tx_power) { (void)apply(m,levels[i-1]); break; }
    } else m->power_good=0;
}
