/* SPDX-License-Identifier: MIT */
#include "modem.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static const unsigned rates[]={300,1200,2400,4800,7200,9600,12000,14400,16800,19200,21600,24000,26400,28800,31200,33600,56000,64000,115200};
unsigned zm_rate(unsigned profile) { return profile<ZM_RATE_COUNT?rates[profile]:0; }
#define DEFAULT_MAX_PROFILE 13 /* 28.8 kbit/s; higher rates are opt-in experiments. */
static const unsigned channels[]={25,15,20,26,11};
static void train(struct zm_modem *m) {
    m->training=true; m->probes=m->probe_acks=m->last_probe_ack=0; m->train_at=m->now; m->probe_at=0;
    m->successes=m->failures=0; m->retrains++;
    m->profile=m->peer_max<m->max_profile?m->peer_max:m->max_profile;
    m->probe_base=m->probe_seq; m->probe_mask=0;
}
void zm_enable_link(struct zm_modem *m,struct zm_link_ops link) {
    m->advanced=true; m->link=link; m->channel=25; m->max_profile=m->peer_max=DEFAULT_MAX_PROFILE;
    m->profile=2; m->adaptive=true; m->compression=true; m->survey_enabled=true;
}

static void put(uint8_t *p, uint64_t v, unsigned n) {
    for (unsigned i=0; i<n; i++) { p[i]=(uint8_t)v; v >>= 8; }
}
static uint64_t get(const uint8_t *p, unsigned n) {
    uint64_t v=0; for (unsigned i=0; i<n; i++) v |= (uint64_t)p[i] << (8*i); return v;
}
void zm_emit(struct zm_modem *m, const char *s) {
    uint8_t out[256];
    while(*s) {
        size_t n=0;
        while(*s && n<sizeof(out)) { unsigned char c=(unsigned char)*s++; out[n++]=(uint8_t)(c=='\r'?m->command_cr:c=='\n'?m->response_lf:c); }
        if(!m->io.serial(m->io.ctx,out,n)) { m->serial_failed=true; return; }
    }
}
#define emit zm_emit
static void result(struct zm_modem *m, unsigned code) {
    static const char *const names[] = {"OK", "CONNECT", "RING", "NO CARRIER", "ERROR",
                                       "", "NO DIALTONE", "BUSY", "NO ANSWER"};
    char s[160];
    if (m->quiet) return;
    if(m->usr_commands && m->result_level<3 && (code==7 || code==8)) code=3;
    if (m->verbose && code==1 && m->advanced) {
        if(m->report_mode) {
            snprintf(s,sizeof(s),"\r\nCARRIER %u\r\nPROTOCOL: ZIGMODEM-ARQ\r\nCOMPRESSION: %s\r\n",rates[m->profile],m->compression?"HEATSHRINK":"NONE");
            emit(m,s);
        }
        if(m->result_level==0) snprintf(s,sizeof(s),"\r\nCONNECT\r\n");
        else snprintf(s,sizeof(s),"\r\nCONNECT %u\r\n",rates[m->profile]);
    }
    else if (m->verbose) snprintf(s,sizeof(s),"\r\n%s\r\n", names[code]);
    else {
        if(code==1 && m->advanced && m->usr_commands && m->result_level) {
            static const unsigned numeric[]={1,5,10,18,20,13,21,25,43,85,91,99,103,107,151,155,232,1,1};
            code=numeric[m->profile];
        }
        snprintf(s,sizeof(s),"%u\r",code);
    }
    emit(m,s);
}
static void send_to(struct zm_modem *m, unsigned type, uint64_t dst, uint64_t session,
                    uint32_t seq, const uint8_t *data, size_t n) {
    uint8_t f[ZM_FRAME] = {'Z','M',1,0};
    f[3]=(uint8_t)type;
    if(m->advanced) {
        uint32_t mask=0;
        for(unsigned i=0;i<ZM_WINDOW;i++) if(m->rx_window[i].used) {
            uint32_t d=m->rx_window[i].seq-m->rx_seq;
            if(d<32) mask|=1u<<d;
        }
        put(f+32,m->rx_seq,4); put(f+36,mask,4); m->ack_pending=false; m->ack_count=0;
    }
    put(f+4,m->number,8); put(f+12,dst,8); put(f+20,session,8); put(f+28,seq,4);
    if(n) memcpy(f+ZM_HEADER,data,n);
    m->frames_tx++;
    m->io.radio(m->io.ctx,f,ZM_HEADER+n);
}
void zm_control(struct zm_modem *m,unsigned type,uint32_t seq,const uint8_t *data,size_t n) {
    send_to(m,type,m->peer,m->session,seq,data,n);
}
void zm_retrain(struct zm_modem *m) { m->survey_state=0; m->survey_due=m->now+10000; train(m); }
static void send(struct zm_modem *m, unsigned type) {
    if(m->advanced && (type==ZM_CALL || type==ZM_ACCEPT)) {
        uint8_t caps[3]={5,(uint8_t)m->max_profile,(uint8_t)m->min_profile};
        send_to(m,type,m->peer,m->session,0,caps,sizeof(caps));
    } else send_to(m,type,m->peer,m->session,0,NULL,0);
}
static void clear_stream(struct zm_modem *m) {
    m->qhead=m->qlen=m->pending_len=0; m->inflight=false;
    m->tx_seq=m->rx_seq=m->tx_base=0; m->ack_pending=false; m->ack_count=0;
    memset(m->tx_window,0,sizeof(m->tx_window)); memset(m->rx_window,0,sizeof(m->rx_window)); m->escape_count=0;
    m->connect_pending=false; m->recovering=false; m->training=false;
    m->survey_state=0; m->survey_round=0; m->survey_due=m->now+1000;
}
static void idle(struct zm_modem *m) {
    m->state=ZM_IDLE; m->online=false; clear_stream(m);
}
static void close_call(struct zm_modem *m, bool report) {
    if (m->state!=ZM_IDLE && m->state!=ZM_CLOSING) {
        send(m,ZM_CLOSE); m->state=ZM_CLOSING; m->started=m->last_control=m->now;
    }
    m->online=false; clear_stream(m);
    if(report) result(m,3);
}
static void connected(struct zm_modem *m) {
    m->state=ZM_CONNECTED; m->online=!m->advanced; m->last_rx=m->last_ping=m->now;
    m->last_input=m->now;
    m->last_data_at=m->now;
    m->connect_pending=m->advanced;
    if(m->advanced) train(m); else result(m,1);
}
static void defaults(struct zm_modem *m) {
    m->echo=true; m->verbose=true; m->quiet=false;
    m->autoanswer=0; m->dial_seconds=30; m->guard_ms=1000;
    m->software_flow=m->flow_tx=m->flow_rx=false; m->escape_char='+'; m->survey_enabled=true;
    m->report_mode=0; m->result_level=4;
    m->command_cr=13; m->response_lf=10; m->command_bs=8;
    m->inactivity_minutes=0; m->escape_hangup=false;
    m->usr_ceiling=m->usr_floor=0;
    if(m->advanced) { m->min_profile=0; m->max_profile=DEFAULT_MAX_PROFILE; m->adaptive=true; m->compression=true; }
}
void zm_init(struct zm_modem *m, uint64_t number, uint64_t nonce, struct zm_io io) {
    memset(m,0,sizeof(*m)); m->number=number; m->nonce=nonce; m->io=io; defaults(m);
    m->random_state=(uint32_t)(number ^ nonce ^ (nonce>>32));
    if(!m->random_state) m->random_state=1;
}
static bool decimal(const char **p, uint64_t *value) {
    const char *s=*p; uint64_t v=0;
    if(!isdigit((unsigned char)*s)) return false;
    while(isdigit((unsigned char)*s)) {
        unsigned d=(unsigned)(*s++-'0');
        if(v>(UINT64_MAX-d)/10) return false;
        v=v*10+d;
    }
    *p=s; *value=v; return true;
}
static void answer(struct zm_modem *m) {
    m->state=ZM_ANSWERING; m->last_control=m->now; send(m,ZM_ACCEPT);
}
uint64_t zm_phone(const uint8_t identity[16]) {
    return UINT64_C(1000000000)+get(identity,8)%UINT64_C(9000000000);
}
void zm_set_identity(struct zm_modem *m,const uint8_t identity[16]) {
    memcpy(m->identity,identity,16); m->secure_identity=true; m->number=zm_phone(identity);
}
static bool hex_identity(const char *p,uint8_t id[16]) {
    if(strlen(p)!=32) return false;
    for(unsigned i=0;i<16;i++) {
        unsigned value=0;
        for(unsigned j=0;j<2;j++) {
            unsigned c=(unsigned char)*p++,digit;
            if(c>='0' && c<='9') digit=c-'0';
            else if(c>='A' && c<='F') digit=c-'A'+10;
            else return false;
            value=value*16+digit;
        }
        id[i]=(uint8_t)value;
    }
    return true;
}
static void command(struct zm_modem *m) {
    char *s=m->command; char out[200]; uint64_t n;
    s[m->command_len]=0;
    for(char *p=s; *p; p++) *p=(char)toupper((unsigned char)*p);
    if(m->overflow || s[0]!='A' || s[1]!='T') { result(m,4); return; }
    memcpy(m->last_command,s,m->command_len+1);
    const char *p=s+2;
    while(*p) {
        char c=*p++;
        if(c==' ') continue;
        if(c=='+') {
            if(strcmp(p,"COMPAT?")==0) { emit(m,m->usr_commands?"\r\nUSR\r\n":"\r\nNATIVE\r\n"); p+=strlen(p);
            } else if((strcmp(p,"COMPAT=USR")==0 || strcmp(p,"COMPAT=NATIVE")==0) && m->state==ZM_IDLE) {
                m->usr_commands=p[7]=='U'; p+=strlen(p);
            } else if(strcmp(p,"GMI")==0 || strcmp(p,"GMI?")==0) { emit(m,"\r\nHigh Energy Magic\r\n"); p+=strlen(p);
            } else if(strcmp(p,"GMM")==0 || strcmp(p,"GMM?")==0) { emit(m,"\r\nZigmodem encrypted radio modem\r\n"); p+=strlen(p);
            } else if(strcmp(p,"GMR")==0 || strcmp(p,"GMR?")==0) { emit(m,"\r\n0.5 experimental\r\n"); p+=strlen(p);
            } else if(strcmp(p,"GCAP")==0 || strcmp(p,"GCAP?")==0) { emit(m,"\r\n+GCAP: +FCLASS\r\n"); p+=strlen(p);
            } else if(strcmp(p,"FCLASS=?")==0 || strcmp(p,"FCLASS?")==0) { emit(m,"\r\n0\r\n"); p+=strlen(p);
            } else if(strncmp(p,"FCLASS=0",8)==0) { p+=8;
            } else if(strcmp(p,"PERF?")==0) {
                snprintf(out,sizeof(out),"\r\nRADIO_US=%" PRIu32 " CRYPTO_US=%" PRIu32 " RADIO_BYTES=%" PRIu32 "\r\n",m->radio_us,m->crypto_us,m->radio_bytes);
                emit(m,out); p+=strlen(p);
            } else if(strcmp(p,"CALLCH?")==0) { emit(m,"\r\n25\r\n"); p+=strlen(p);
            } else if(strcmp(p,"SURVEY?")==0) {
                snprintf(out,sizeof(out),"\r\nENABLED=%u STATE=%u COUNT=%" PRIu32 " MOVES=%" PRIu32 "\r\n",m->survey_enabled,m->survey_state,m->surveys,m->survey_moves); emit(m,out);
                for(unsigned i=0;i<5;i++) {
                    snprintf(out,sizeof(out),"CH=%u ED=%d AGE=%" PRIu64 "\r\n",channels[i],m->channel_measured[i]?m->channel_energy[i]:127,m->channel_measured[i]?m->now-m->channel_measured[i]:0); emit(m,out);
                }
                p+=strlen(p);
            } else if(strcmp(p,"SCAN")==0 && m->state==ZM_CONNECTED && m->caller && m->link.energy && m->survey_enabled) {
                m->survey_due=m->now; p+=strlen(p);
            } else if(strncmp(p,"SCAN=",5)==0 && (p[5]=='0' || p[5]=='1') && !p[6]) {
                m->survey_enabled=p[5]=='1'; p+=6;
            } else if(strcmp(p,"IDENT?")==0 && m->secure_identity) {
                if(m->secure_identity) {
                    emit(m,"\r\n");
                    for(unsigned i=0;i<16;i++) { snprintf(out,sizeof(out),"%02X",m->identity[i]); emit(m,out); }
                    emit(m,"\r\n");
                } else { snprintf(out,sizeof(out),"\r\n%" PRIu64 "\r\n",m->number); emit(m,out); }
                p+=strlen(p);
            } else if(strcmp(p,"PHONUM?")==0) {
                snprintf(out,sizeof(out),"\r\n%" PRIu64 "\r\n",m->number); emit(m,out); p+=strlen(p);
            } else if(strncmp(p,"PIN=",4)==0 && m->state==ZM_IDLE && m->link.pin) {
                uint8_t id[16];
                if(!hex_identity(p+4,id) || !m->link.pin(m->link.ctx,id)) { result(m,4); return; }
                p+=strlen(p);
            } else if(strcmp(p,"BOOK?")==0 && m->link.lookup) {
                uint8_t id[16];
                for(unsigned i=1;i<=16;i++) if(m->link.lookup(m->link.ctx,i,id)) {
                    snprintf(out,sizeof(out),"\r\n%u=%" PRIu64 ",",i,zm_phone(id)); emit(m,out);
                    for(unsigned j=0;j<16;j++) { snprintf(out,sizeof(out),"%02X",id[j]); emit(m,out); }
                }
                emit(m,"\r\n"); p+=strlen(p);
            } else if(strcmp(p,"STATS?")==0) {
                snprintf(out,sizeof(out),"\r\nTX=%" PRIu32 " RX=%" PRIu32
                    " RETRY=%" PRIu32 " OVERRUN=%" PRIu32 " IN=%" PRIu32
                    " ACKED=%" PRIu32 " FAULT=%" PRIu32 " RFERR=%" PRIu32 " RSSI=%d STATUS=%08" PRIx32 "\r\n",
                    m->frames_tx,m->frames_rx,m->retries,m->overruns,
                    m->bytes_rx,m->bytes_acked,m->last_fault,m->radio_errors,(int)m->last_rssi,m->radio_status);
                emit(m,out); p+=strlen(p);
            } else if(strcmp(p,"LINK?")==0 && m->advanced) {
                snprintf(out,sizeof(out),"\r\nRATE=%u CHANNEL=%u TRAINING=%u RECOVERING=%u RETRAINS=%" PRIu32
                         " SWITCHES=%" PRIu32 " RTT=%" PRIu32 " COMP=%u\r\n",
                         rates[m->profile],m->channel,m->training,m->recovering,m->retrains,m->channel_changes,m->rtt_ms,m->compression);
                emit(m,out); p+=strlen(p);
            } else if(strncmp(p,"CHANNEL=",8)==0 && m->advanced && m->link.channel) {
                p+=8;
                if(!decimal(&p,&n) || *p) { result(m,4); return; }
                unsigned index=5;
                for(unsigned i=0;i<5;i++) if(channels[i]==n) index=i;
                if(index==5 || !m->link.channel(m->link.ctx,(unsigned)n)) { result(m,4); return; }
                m->channel_index=index; m->channel=(unsigned)n; m->channel_at=m->now; m->channel_changes++;
                if(m->state==ZM_CONNECTED) { m->recovering=true; m->last_ping=0; }
            } else if(strcmp(p,"SEC?")==0 && m->secure_identity) {
                snprintf(out,sizeof(out),"\r\nNOISE_XX_25519_CHACHAPOLY_BLAKE2S AUTH=%" PRIu32
                    " REJECT=%" PRIu32 " SAVED=%" PRIu32 " STAGE=%u\r\n",m->authenticated_frames,m->rejected_frames,m->compression_saved,m->secure_stage);
                emit(m,out); p+=strlen(p);
            } else if(strcmp(p,"WIRE?")==0 && m->secure_identity) {
                snprintf(out,sizeof(out),"\r\nRX=%" PRIu32 " INVALID=%" PRIu32 " TYPE=%u LEN=%u\r\n",
                    m->wire_rx,m->invalid_wire,m->wire_type,m->wire_length);
                emit(m,out); p+=strlen(p);
            } else if(strcmp(p,"MS?")==0 && m->advanced) {
                snprintf(out,sizeof(out),"\r\n%u,%u,%u\r\n",m->adaptive,rates[m->min_profile],rates[m->max_profile]);
                emit(m,out); p+=strlen(p);
            } else if(strncmp(p,"MS=",3)==0 && m->advanced && m->state==ZM_IDLE) {
                uint64_t low,high; p+=3;
                if(!decimal(&p,&n) || n>1 || *p++!=',' || !decimal(&p,&low) || *p++!=',' || !decimal(&p,&high) || *p) { result(m,4); return; }
                unsigned lo=ZM_RATE_COUNT,hi=ZM_RATE_COUNT;
                for(unsigned i=0;i<ZM_RATE_COUNT;i++) { if(rates[i]==low) lo=i; if(rates[i]==high) hi=i; }
                if(lo>hi || hi>=ZM_RATE_COUNT) { result(m,4); return; }
                m->adaptive=(bool)n; m->min_profile=lo; m->max_profile=hi;
                m->usr_ceiling=m->usr_floor=0;
            } else { result(m,4); return; }
        } else if(c=='%' && *p++=='C' && m->advanced && (*p=='0' || *p=='1')) {
            m->compression=*p++=='1';
        } else if(c=='I') {
            n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
            if(n>UINT32_MAX || !zm_inquiry(m,(unsigned)n)) { result(m,4); return; }
        } else if(c=='X' || c=='W' || (c=='Y' && m->usr_commands)) {
            n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
            if(c=='X' && n<=4) m->result_level=(unsigned)n;
            else if(c=='W' && n<=2) m->report_mode=(unsigned)n;
            else if(c=='Y' && n<=4 && n!=3 && zm_select_profile(m,(unsigned)n)) { }
            else { result(m,4); return; }
        } else if(c=='E' || c=='V' || c=='Q') {
            n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
            if(n>1) { result(m,4); return; }
            bool b=n==1;
            if(c=='E') m->echo=b; else if(c=='V') m->verbose=b; else m->quiet=b;
        } else if(c=='Z' || c=='&') {
            if(c=='&' && m->usr_commands && (*p=='N' || *p=='U') && m->state==ZM_IDLE) {
                char sub=*p++; n=0;
                if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
                if(n>16 && n!=38) { result(m,4); return; }
                unsigned ceiling=sub=='N'?(unsigned)n:m->usr_ceiling;
                unsigned floor=sub=='U'?(unsigned)n:m->usr_floor;
                unsigned hi=ceiling==38?16:ceiling?ceiling-1:DEFAULT_MAX_PROFILE;
                unsigned lo=floor==38?16:floor?floor-1:ceiling?hi:0;
                if(lo>hi) { result(m,4); return; }
                m->usr_ceiling=ceiling; m->usr_floor=floor;
                m->min_profile=lo; m->max_profile=hi; m->adaptive=lo!=hi; continue;
            }
            if(c=='&' && *p=='V') {
                p++; (void)zm_inquiry(m,4); (void)zm_inquiry(m,5); continue;
            }
            if(c=='&' && m->usr_commands && strchr("AHIKMB",*p) && *p) {
                char sub=*p++; n=0;
                if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
                if(sub=='A' && n<=3) m->report_mode=(unsigned)n;
                else if(sub=='H' && (n==0 || n==2)) m->flow_tx=n==2;
                else if(sub=='I' && (n==0 || n==2)) m->flow_rx=n==2;
                else if(sub=='K' && n<=3) m->compression=n!=0;
                else if(sub=='M' && (n==4 || n==5)) { /* Always reliable. */ }
                else if(sub=='B' && n==1) { /* UART is fixed at 115200. */ }
                else { result(m,4); return; }
                m->software_flow=m->flow_tx && m->flow_rx; continue;
            }
            if(c=='&' && *p=='K' && m->advanced && !m->usr_commands) {
                p++;
                if(*p=='?') { p++; snprintf(out,sizeof(out),"\r\n%u\r\n",m->software_flow?4:0); emit(m,out); continue; }
                if(*p!='0' && *p!='4') { result(m,4); return; }
                m->software_flow=m->flow_tx=m->flow_rx=*p++=='4'; continue;
            }
            if(c=='&' && *p=='W') {
                p++; n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
                if(n>1 || !zm_save_profile(m,(unsigned)n)) { result(m,4); return; }
                continue;
            }
            if(c=='&' && *p=='Y' && !m->usr_commands) {
                p++; n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
                if(n>1 || !zm_select_profile(m,(unsigned)n)) { result(m,4); return; }
                continue;
            }
            if(c=='Z') {
                n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
                unsigned slot=m->boot_profile;
                if(n>(m->usr_commands?5u:1u) || (m->usr_commands && n==4)) { result(m,4); return; }
                if(m->usr_commands && n) slot=(unsigned)n-1;
                else if(!m->usr_commands && n) slot=(unsigned)n;
                close_call(m,false); defaults(m);
                if(slot==4) m->software_flow=m->flow_tx=m->flow_rx=true;
                else (void)zm_load_profile(m,slot);
                continue;
            }
            if(c=='&' && *p++!='F') { result(m,4); return; }
            n=0; if(*p>='0' && *p<='9' && !decimal(&p,&n)) { result(m,4); return; }
            if(n!=0 && n!=2) { result(m,4); return; }
            close_call(m,false); defaults(m);
            if(n==2) m->software_flow=m->flow_tx=m->flow_rx=true;
        } else if(c=='H') {
            if(*p=='0') p++;
            close_call(m,false);
        } else if(c=='O') {
            bool retrain=*p=='1'; if(*p=='0' || *p=='1') p++;
            if(*p || m->state!=ZM_CONNECTED) { result(m,4); return; }
            if(retrain && m->advanced) { m->online=false; m->connect_pending=true; train(m); return; }
            if(m->advanced && m->training) { m->connect_pending=true; return; }
            m->online=true; m->last_input=m->now; result(m,1); return;
        } else if(c=='A') {
            if(*p || m->state!=ZM_RINGING) { result(m,4); return; }
            answer(m); return;
        } else if(c=='D') {
            if(*p=='T' || *p=='P') p++;
            bool valid;
            if(m->secure_identity && strlen(p)==32) {
                valid=hex_identity(p,m->peer_identity); n=valid?zm_phone(m->peer_identity):0; p+=strlen(p);
            } else if(m->secure_identity) {
                memset(m->peer_identity,0,16); valid=decimal(&p,&n);
                if(valid && n>=1 && n<=16 && m->link.lookup && m->link.lookup(m->link.ctx,(unsigned)n,m->peer_identity))
                    n=zm_phone(m->peer_identity);
                else valid=valid && n>=UINT64_C(1000000000) && n<=UINT64_C(9999999999);
            }
            else valid=decimal(&p,&n);
            if(!valid || *p || !n || n==m->number || m->state!=ZM_IDLE) {
                result(m,4); return;
            }
            clear_stream(m); m->peer=n;
            m->caller=true; m->last_rx=m->now;
            /* Boot nonce must come from platform entropy. Never reuse a session. */
            if(++m->nonce==0) ++m->nonce;
            m->session=m->nonce; m->state=ZM_DIALING;
            m->started=m->last_control=m->now; send(m,ZM_CALL); return;
        } else if(c=='S') {
            uint64_t reg;
            if(!decimal(&p,&reg) || (reg>5 && reg!=7 && reg!=12 && reg!=13 && reg!=19)) { result(m,4); return; }
            if(*p=='?') {
                p++; unsigned v=reg==0?m->autoanswer:reg==1?m->rings:reg==2?m->escape_char:
                    reg==3?m->command_cr:reg==4?m->response_lf:reg==5?m->command_bs:reg==7?m->dial_seconds:
                    reg==13?(m->escape_hangup?128u:0u):reg==19?m->inactivity_minutes:m->guard_ms/20;
                snprintf(out,sizeof(out),"\r\n%u\r\n",v); emit(m,out);
            } else if(reg!=1 && *p++=='=' && decimal(&p,&n) && n<=255 && ((reg!=7 && reg!=12) || n>0) && (!(reg>=3 && reg<=5) || n<=127) && (reg!=13 || n==0 || n==128)) {
                if(reg==0) m->autoanswer=(unsigned)n;
                else if(reg==2) m->escape_char=(unsigned)n;
                else if(reg==3) m->command_cr=(unsigned)n;
                else if(reg==4) m->response_lf=(unsigned)n;
                else if(reg==5) m->command_bs=(unsigned)n;
                else if(reg==7) m->dial_seconds=(unsigned)n;
                else if(reg==13) m->escape_hangup=n==128;
                else if(reg==19) m->inactivity_minutes=(unsigned)n;
                else m->guard_ms=(unsigned)n*20;
            } else { result(m,4); return; }
        } else { result(m,4); return; }
    }
    result(m,0);
}
static bool enqueue(struct zm_modem *m, uint8_t b) {
    if(m->qlen==ZM_QUEUE) return false;
    m->queue[(m->qhead+m->qlen)%ZM_QUEUE]=b; m->qlen++; return true;
}
static bool flush_escape(struct zm_modem *m) {
    if(ZM_QUEUE-m->qlen<m->escape_count) return false;
    while(m->escape_count) { enqueue(m,(uint8_t)m->escape_char); m->escape_count--; }
    return true;
}
size_t zm_input(struct zm_modem *m, const uint8_t *data, size_t n) {
    size_t i;
    for(i=0;i<n;i++) {
        uint8_t b=data[i];
        if(m->online) {
            if(m->escape_count && m->escape_char<128 && b==m->escape_char && m->escape_count<3 &&
               m->now-m->escape_at<m->guard_ms) {
                m->escape_count++; m->escape_at=m->last_input=m->now; continue;
            }
            if(!m->escape_count && m->escape_char<128 && b==m->escape_char && m->now-m->last_input>=m->guard_ms) {
                m->escape_count=1; m->escape_at=m->last_input=m->now; continue;
            }
            if(!flush_escape(m) || !enqueue(m,b)) break;
            m->last_input=m->now;
            m->last_data_at=m->now;
        } else {
            if(m->echo && !m->io.serial(m->io.ctx,&b,1)) break;
            if(b==m->command_cr) {
                command(m); m->command_len=0; m->overflow=false;
            } else if(b=='/' && m->command_len==1 && (m->command[0]=='A' || m->command[0]=='a')) {
                strcpy(m->command,m->last_command); m->command_len=strlen(m->command);
                command(m); m->command_len=0; m->overflow=false;
            } else if(b==m->command_bs || b==127) {
                if(m->command_len) m->command_len--;
            } else if(b!=m->response_lf) {
                if(m->command_len<sizeof(m->command)-1) m->command[m->command_len++]=(char)b;
                else m->overflow=true;
            }
        }
    }
    return i;
}
static void drain_received(struct zm_modem *m) {
    while(m->online) {
        struct zm_slot *slot=&m->rx_window[m->rx_seq%ZM_WINDOW];
        if(!slot->used || slot->seq!=m->rx_seq || !m->io.serial(m->io.ctx,slot->data,slot->len)) break;
        m->bytes_rx+=(uint32_t)slot->len; m->last_data_at=m->now; slot->used=false; m->rx_seq++;
        if(!m->ack_pending) m->ack_at=m->now;
        m->ack_pending=true;
    }
}
static void acknowledge(struct zm_modem *m,const uint8_t *f) {
    uint32_t base=(uint32_t)get(f+32,4),mask=(uint32_t)get(f+36,4);
    if(base>m->tx_seq) return;
    for(unsigned i=0;i<ZM_WINDOW;i++) {
        struct zm_slot *slot=&m->tx_window[i]; uint32_t d=slot->seq-base;
        if(slot->used && (slot->seq<base || (d<32 && (mask&(1u<<d))))) {
            m->bytes_acked+=(uint32_t)slot->len; m->rtt_ms=(uint32_t)(m->now-slot->sent);
            slot->used=false; m->successes++;
        }
    }
    while(m->tx_base<m->tx_seq && !m->tx_window[m->tx_base%ZM_WINDOW].used) m->tx_base++;
    m->inflight=m->tx_base<m->tx_seq;
    if(m->adaptive && m->successes>=64) {
        unsigned max=m->peer_max<m->max_profile?m->peer_max:m->max_profile;
        if(m->failures==0 && m->profile<max) m->profile++;
        m->successes=m->failures=0;
    }
}
void zm_receive(struct zm_modem *m, const uint8_t *f, size_t n) {
    if(n<ZM_HEADER || n>ZM_FRAME || f[0]!='Z' || f[1]!='M' || f[2]!=1 ||
       get(f+12,8)!=m->number) return;
    unsigned type=f[3]; uint64_t src=get(f+4,8), session=get(f+20,8);
    uint32_t seq=(uint32_t)get(f+28,4);
    bool caps=m->advanced && (type==ZM_CALL || type==ZM_ACCEPT);
    bool probe=m->advanced && type==ZM_PROBE;
    bool survey=m->advanced && type==ZM_SURVEY;
    if(!src || src==m->number || !session || type<ZM_CALL || type>ZM_SURVEY ||
       (!caps && !probe && !survey && type!=ZM_DATA && n!=ZM_HEADER) || (type==ZM_DATA && n==ZM_HEADER)) return;
    if(caps && (n!=ZM_HEADER+3 || f[ZM_HEADER]!=5 || f[ZM_HEADER+1]>=ZM_RATE_COUNT ||
       f[ZM_HEADER+2]>f[ZM_HEADER+1] || f[ZM_HEADER+1]<m->min_profile || f[ZM_HEADER+2]>m->max_profile)) return;
    if(survey && n!=ZM_HEADER+4) return;
    m->frames_rx++;
    if(type==ZM_CALL) {
        if(m->state==ZM_IDLE) {
            m->caller=false; m->last_rx=m->now;
            if(caps) { m->peer_max=f[ZM_HEADER+1]; m->peer_min=f[ZM_HEADER+2]; }
            clear_stream(m); m->peer=src; m->session=session; m->state=ZM_RINGING;
            m->started=m->last_control=m->now; m->rings=1; result(m,2);
            if(m->autoanswer==1) answer(m);
        } else if(m->peer==src && m->session==session &&
                  (m->state==ZM_ANSWERING || m->state==ZM_CONNECTED)) send(m,ZM_ACCEPT);
        else if(m->peer==src && m->session==session && m->state==ZM_RINGING && m->advanced) {
            m->last_rx=m->now; send(m,ZM_WAIT);
        } else if(!(m->peer==src && m->session==session && m->state==ZM_RINGING))
            send_to(m,ZM_BUSY,src,session,0,NULL,0);
        return;
    }
    if(src!=m->peer || session!=m->session) return;
    m->last_rx=m->now;
    if(type==ZM_WAIT && m->advanced) return;
    if(type==ZM_CLOSE) {
        send(m,ZM_CLOSED);
        if(m->state!=ZM_IDLE && m->state!=ZM_CLOSING) { idle(m); result(m,3); }
        else idle(m);
        return;
    }
    if(type==ZM_CLOSED && m->state==ZM_CLOSING) { idle(m); return; }
    if(type==ZM_BUSY && m->state==ZM_DIALING) { idle(m); result(m,7); return; }
    if(type==ZM_ACCEPT && (m->state==ZM_DIALING || m->state==ZM_CONNECTED)) {
        if(caps) { m->peer_max=f[ZM_HEADER+1]; m->peer_min=f[ZM_HEADER+2]; }
        send(m,ZM_CONFIRM);
        if(m->state==ZM_DIALING) connected(m);
        return;
    }
    if(m->state==ZM_ANSWERING && (type==ZM_CONFIRM || type==ZM_DATA || type==ZM_PING))
        connected(m);
    if(m->state!=ZM_CONNECTED) return;
    m->last_rx=m->now;
    if(m->recovering) { m->recovering=false; train(m); }
    if(m->advanced) acknowledge(m,f);
    if(survey) { zm_survey_receive(m,seq,f+ZM_HEADER); return; }
    if(type==ZM_PROBE && m->advanced) { send_to(m,ZM_PROBE_ACK,m->peer,m->session,seq,NULL,0); return; }
    if(type==ZM_PROBE_ACK && m->training && seq>m->probe_base && seq-m->probe_base<=m->probes) {
        uint32_t bit=1u<<(seq-m->probe_base-1);
        if(!(m->probe_mask&bit)) { m->probe_mask|=bit; m->probe_acks++; }
        if(seq==m->probe_seq) m->rtt_ms=(uint32_t)(m->now-m->probe_at);
        return;
    }
    if(type==ZM_PING) send(m,ZM_PONG);
    if(type==ZM_DATA && m->advanced) {
        if(seq>=m->rx_seq && seq-m->rx_seq<ZM_WINDOW) {
            struct zm_slot *slot=&m->rx_window[seq%ZM_WINDOW];
            if(!slot->used) { slot->used=true; slot->seq=seq; slot->len=n-ZM_HEADER; memcpy(slot->data,f+ZM_HEADER,slot->len); }
        }
        drain_received(m);
        if(!m->ack_pending) m->ack_at=m->now;
        m->ack_pending=true; m->ack_count++; return;
    }
    if(type==ZM_DATA) {
        if(seq==m->rx_seq) {
            /* Hold remote data while locally in command mode. */
            if(!m->online || !m->io.serial(m->io.ctx,f+ZM_HEADER,n-ZM_HEADER)) return;
            m->rx_seq++;
            m->bytes_rx+=(uint32_t)(n-ZM_HEADER);
        } else if(seq!=m->rx_seq-1 || m->rx_seq==0) return;
        send_to(m,ZM_ACK,m->peer,m->session,seq,NULL,0);
    } else if(type==ZM_ACK && m->inflight && seq==m->tx_seq) {
        m->rtt_ms=(uint32_t)(m->now-m->last_tx); m->successes++;
        if(m->advanced && m->adaptive && m->successes>=32) {
            unsigned max=m->peer_max<m->max_profile?m->peer_max:m->max_profile;
            unsigned min=m->peer_min>m->min_profile?m->peer_min:m->min_profile;
            if(m->failures>8 && m->profile>min) m->profile--;
            else if(m->failures==0 && m->profile<max) m->profile++;
            m->successes=m->failures=0;
        }
        m->bytes_acked+=(uint32_t)m->pending_len;
        m->inflight=false; m->tx_seq++;
        /* Avoid wrap: fail before a sequence can be reused in one session. */
        if(m->tx_seq==UINT32_MAX) close_call(m,true);
    }
}
static uint32_t retry_delay(struct zm_modem *m) {
    uint32_t x=m->random_state;
    x^=x<<13; x^=x>>17; x^=x<<5; m->random_state=x;
    return 150+x%151;
}
void zm_tick(struct zm_modem *m, uint64_t now) {
    m->now=now;
    if(m->advanced && m->link.channel) {
        if(m->state==ZM_IDLE && m->channel!=25 && m->link.channel(m->link.ctx,25)) {
            m->channel=25; m->channel_index=0; m->channel_at=now;
        }
        bool search=!m->survey_state && now-m->last_rx>=2000 && m->state==ZM_CONNECTED;
        unsigned dwell=m->state==ZM_IDLE?(m->channel==25?4000:800):!m->caller?1700:350;
        if(search && now-m->channel_at>=dwell) {
            unsigned index=(m->channel_index+1)%5;
            if(m->link.channel(m->link.ctx,channels[index])) {
                m->channel_index=index; m->channel=channels[index]; m->channel_changes++;
                if(m->state==ZM_CONNECTED) { m->recovering=true; m->last_ping=0; }
            }
            m->channel_at=now;
        }
    }
    if(m->serial_failed) { m->serial_failed=false; m->last_fault=3; close_call(m,false); }
    switch(m->state) {
    case ZM_DIALING:
        if(now-m->started>=(uint64_t)m->dial_seconds*1000) { close_call(m,false); result(m,8); }
        else if(now-m->last_control>=500) { send(m,ZM_CALL); m->last_control=now; }
        break;
    case ZM_RINGING:
    case ZM_ANSWERING:
        if(now-m->started>=60000) { close_call(m,true); break; }
        if(now-m->last_control>=(m->state==ZM_RINGING?2000u:500u)) {
            m->last_control=now;
            if(m->state==ZM_ANSWERING) send(m,ZM_ACCEPT);
            else { result(m,2); m->rings++; if(m->autoanswer && m->rings>=m->autoanswer) answer(m); }
        }
        break;
    case ZM_CLOSING:
        if(now-m->started>=3000) idle(m);
        else if(now-m->last_control>=300) { send(m,ZM_CLOSE); m->last_control=now; }
        break;
    case ZM_CONNECTED:
        if(m->inactivity_minutes && now-m->last_data_at>=(uint64_t)m->inactivity_minutes*60000) { close_call(m,true); break; }
        if(m->advanced && zm_survey_tick(m)) break;
        if(m->advanced) {
            drain_received(m);
            if(m->ack_pending && (m->ack_count>=4 || now-m->ack_at>=32)) send(m,ZM_ACK);
        }
        if(now-m->last_rx>=(m->advanced?30000u:10000u)) { m->last_fault=2; close_call(m,true); break; }
        if(now-m->last_ping>=1000+m->number%251) { send(m,ZM_PING); m->last_ping=now; }
        if(m->escape_count && now-m->escape_at>=m->guard_ms) {
            if(m->escape_count==3) {
                m->escape_count=0;
                if(m->escape_hangup) close_call(m,true);
                else { m->online=false; result(m,0); }
            }
            else (void)flush_escape(m);
        }
        if(m->recovering) break;
        if(m->training) {
            unsigned interval=(ZM_PAYLOAD*8000+rates[m->profile]-1)/rates[m->profile];
            unsigned jitter=interval/8+1; if(jitter>23) jitter=23;
            interval+=(unsigned)((m->number ^ ((uint64_t)m->probe_seq*2654435761u))%jitter);
            if(now-m->train_at>=60000) { close_call(m,true); break; }
            if(m->probes<8 && now-m->probe_at>=interval) {
                uint8_t pattern[ZM_PAYLOAD]; for(unsigned i=0;i<sizeof(pattern);i++) pattern[i]=(uint8_t)(i*73+m->probes);
                m->probes++; m->probe_seq++; m->probe_at=now;
                send_to(m,ZM_PROBE,m->peer,m->session,m->probe_seq,pattern,sizeof(pattern));
            } else if(m->probes==8 && now-m->probe_at>=interval+250) {
                unsigned min=m->peer_min>m->min_profile?m->peer_min:m->min_profile;
                if(m->probe_acks<7 && m->profile>min && m->adaptive) {
                    m->profile--; m->probes=m->probe_acks=0;
                    m->probe_base=m->probe_seq; m->probe_mask=0; break;
                }
                if(m->probe_acks<2) { close_call(m,true); break; }
                m->trained_rate=rates[m->profile]; m->training=false;
                if(m->connect_pending) { m->connect_pending=false; m->online=true; m->last_input=now; result(m,1); }
            }
            break;
        }
        if(m->advanced) {
            /* Retransmit only missing packets, while later packets remain buffered. */
            unsigned retry=m->rtt_ms*3+20; if(retry<100) retry=100; if(retry>250) retry=250;
            bool sent=false;
            for(unsigned i=0;i<ZM_WINDOW;i++) {
                struct zm_slot *slot=&m->tx_window[i];
                if(slot->used && now-slot->sent>=retry) {
                    slot->sent=now; m->retries++; m->failures++;
                    unsigned min=m->peer_min>m->min_profile?m->peer_min:m->min_profile;
                    if(m->adaptive && m->failures%16==0 && m->profile>min) m->profile--;
                    send_to(m,ZM_DATA,m->peer,m->session,slot->seq,slot->data,slot->len); sent=true; break;
                }
            }
            if(!sent && m->qlen && m->tx_seq-m->tx_base<ZM_WINDOW && now*1000>=m->next_data) {
                if(m->tx_seq==UINT32_MAX) { close_call(m,true); break; }
                struct zm_slot *slot=&m->tx_window[m->tx_seq%ZM_WINDOW];
                slot->seq=m->tx_seq++; slot->used=true; slot->sent=now;
                slot->len=m->qlen<ZM_PAYLOAD?m->qlen:ZM_PAYLOAD;
                for(size_t i=0;i<slot->len;i++) { slot->data[i]=m->queue[m->qhead]; m->qhead=(m->qhead+1)%ZM_QUEUE; }
                m->qlen-=slot->len; m->inflight=true;
                uint64_t spacing=slot->len*UINT64_C(8000000)/rates[m->profile];
                if(now*1000>m->next_data+spacing) m->next_data=now*1000;
                m->next_data+=spacing;
                send_to(m,ZM_DATA,m->peer,m->session,slot->seq,slot->data,slot->len);
            }
            break;
        }
        if(!m->inflight && m->qlen) {
            m->pending_len=m->qlen<ZM_PAYLOAD?m->qlen:ZM_PAYLOAD;
            for(size_t i=0;i<m->pending_len;i++) {
                m->pending[i]=m->queue[m->qhead]; m->qhead=(m->qhead+1)%ZM_QUEUE;
            }
            m->qlen-=m->pending_len; m->inflight=true; m->last_tx=now;
            if(m->advanced) m->next_data=now+(m->pending_len*8000+rates[m->profile]-1)/rates[m->profile];
            m->retry_ms=retry_delay(m);
            send_to(m,ZM_DATA,m->peer,m->session,m->tx_seq,m->pending,m->pending_len);
        } else if(m->inflight && now-m->last_tx>=m->retry_ms) {
            m->last_tx=now;
            m->retries++; m->retry_ms=retry_delay(m);
            m->failures++;
            unsigned min=m->peer_min>m->min_profile?m->peer_min:m->min_profile;
            if(m->advanced && m->adaptive && m->failures%8==0 && m->profile>min) m->profile--;
            if(m->advanced && m->failures%24==0 && now>=2000) m->last_rx=now-2000;
            send_to(m,ZM_DATA,m->peer,m->session,m->tx_seq,m->pending,m->pending_len);
        }
        break;
    case ZM_IDLE: break;
    }
}
void zm_overrun(struct zm_modem *m) {
    m->overruns++; m->last_fault=1; m->command_len=0; m->overflow=false; close_call(m,true);
}
