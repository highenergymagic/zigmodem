/* SPDX-License-Identifier: MIT */
#include "modem.h"
static const unsigned channels[]={25,15,20,26,11};
enum { IDLE, REQUEST, READY, SCHEDULED, MEASURING, REPORT, MOVE_WAIT, MOVE };
static bool tune(struct zm_modem *m,unsigned ch) {
    if(ch==m->channel) return true;
    if(!m->link.channel || !m->link.channel(m->link.ctx,ch)) return false;
    m->channel=ch; m->channel_changes++; m->channel_at=m->now;
    for(unsigned i=0;i<5;i++) if(channels[i]==ch) m->channel_index=i;
    return true;
}
static void message(struct zm_modem *m,unsigned phase,unsigned value) {
    uint8_t p[4]={(uint8_t)phase,(uint8_t)m->survey_index,(uint8_t)value,(uint8_t)(value>>8)};
    zm_control(m,ZM_SURVEY,m->survey_id,p,sizeof(p)); m->survey_tx=m->now;
}
static void finish(struct zm_modem *m) {
    m->survey_state=IDLE; m->survey_due=m->now+(m->survey_round<5?300:10000);
}
static void assess(struct zm_modem *m,int peer) {
    if(!m->survey_sampled || peer==127 || m->survey_local==127) { m->survey_round++; finish(m); return; }
    unsigned idx=m->survey_index;
    m->channel_energy[idx]=m->survey_local>peer?m->survey_local:peer;
    m->channel_measured[idx]=m->now; m->surveys++; m->survey_round++;
    unsigned best=5,current=5;
    for(unsigned i=0;i<5;i++) {
        if(channels[i]==m->channel) current=i;
        if(m->channel_measured[i] && m->now-m->channel_measured[i]<65000 &&
           (best==5 || m->channel_energy[i]<m->channel_energy[best])) best=i;
    }
    /* Wait for an initial complete survey, and demand meaningful improvement. */
    if(m->survey_round>=5 && best<5 && current<5 && best!=current &&
       m->channel_measured[current] && m->now-m->channel_measured[current]<65000 &&
       m->channel_energy[best]+6<m->channel_energy[current]) {
        m->survey_index=best; m->survey_state=MOVE_WAIT; m->survey_at=m->now;
        message(m,5,0);
    } else finish(m);
}
void zm_survey_receive(struct zm_modem *m,uint32_t id,const uint8_t *p) {
    if(!m->link.energy || p[0]>6 || p[1]>=5 || !id) return;
    unsigned phase=p[0],value=p[2]|((unsigned)p[3]<<8);
    if(phase==0 && !m->caller && (m->survey_state==IDLE || id==m->survey_id)) {
        if(id!=m->survey_id || m->survey_state==IDLE) {
            m->survey_id=id; m->survey_index=p[1]; m->survey_origin=m->channel;
            m->survey_state=READY; m->survey_at=m->now; m->survey_sampled=false;
        }
        if(m->survey_state==READY) message(m,1,0);
        return;
    }
    if(id!=m->survey_id) return;
    if(phase==1 && m->caller && m->survey_state==REQUEST) {
        m->survey_start=m->now+150; m->survey_end=m->survey_start+180;
        m->survey_state=SCHEDULED; message(m,2,150);
    } else if(phase==2 && !m->caller && m->survey_state==READY && value<=150) {
        m->survey_start=m->now+value; m->survey_end=m->survey_start+180;
        m->survey_state=SCHEDULED;
    } else if(phase==3 && m->caller && m->survey_state==REPORT) {
        assess(m,(int)(int8_t)p[2]);
    } else if(phase==4 && !m->caller && (m->survey_state==REPORT || m->survey_state==IDLE)) {
        message(m,3,(unsigned)(uint8_t)(m->survey_sampled?m->survey_local:127));
    } else if(phase==5 && !m->caller && (m->survey_state==REPORT || m->survey_state==IDLE || m->survey_state==MOVE)) {
        if(m->survey_state!=MOVE) {
            m->survey_index=p[1]; m->survey_start=m->now+200; m->survey_state=MOVE; m->survey_at=m->now;
        }
        message(m,6,0);
    } else if(phase==6 && m->caller && m->survey_state==MOVE_WAIT) {
        m->survey_start=m->now+200; m->survey_state=MOVE;
    }
}
bool zm_survey_tick(struct zm_modem *m) {
    if(!m->link.energy) return false;
    if(m->survey_state && m->now-m->survey_at>2500) {
        (void)tune(m,m->survey_origin); if(m->caller) m->survey_round++; finish(m);
    }
    if(m->survey_state==IDLE && m->caller && m->survey_enabled && !m->training && !m->recovering && m->now>=m->survey_due) {
        if(++m->survey_id==0) ++m->survey_id;
        m->survey_index=m->survey_round%5; m->survey_origin=m->channel;
        m->survey_at=m->now; m->survey_state=REQUEST; m->survey_sampled=false;
        message(m,0,0);
    }
    switch(m->survey_state) {
    case REQUEST: if(m->now-m->survey_tx>=100) message(m,0,0); break;
    case SCHEDULED:
        if(m->now>=m->survey_start) {
            if(!tune(m,channels[m->survey_index])) { finish(m); break; }
            m->survey_state=MEASURING;
        } else if(m->caller && m->now-m->survey_tx>=30) message(m,2,(unsigned)(m->survey_start-m->now));
        break;
    case MEASURING:
        if(!m->survey_sampled && m->now>=m->survey_start+40) {
            m->survey_local=m->link.energy(m->link.ctx,8); m->survey_sampled=true;
        }
        if(m->now>=m->survey_end) {
            if(!tune(m,m->survey_origin)) { finish(m); break; }
            m->survey_state=REPORT;
            message(m,m->caller?4:3,m->caller?0:(unsigned)(uint8_t)m->survey_local);
        }
        break;
    case REPORT:
        if(m->now-m->survey_tx>=100) message(m,m->caller?4:3,m->caller?0:(unsigned)(uint8_t)m->survey_local);
        if(!m->caller && m->now>m->survey_end+600) finish(m);
        break;
    case MOVE_WAIT: if(m->now-m->survey_tx>=50) message(m,5,0); break;
    case MOVE:
        if(m->now>=m->survey_start) {
            if(tune(m,channels[m->survey_index])) { m->survey_moves++; zm_retrain(m); }
            finish(m);
        } else if(!m->caller && m->now-m->survey_tx>=30) message(m,6,0);
        break;
    default: break;
    }
    return m->survey_state==SCHEDULED || m->survey_state==MEASURING || m->survey_state==MOVE;
}
