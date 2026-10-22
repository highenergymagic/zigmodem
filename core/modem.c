/* SPDX-License-Identifier: MIT */
#include "modem.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void put(uint8_t *p, uint64_t v, unsigned n) {
    for (unsigned i=0; i<n; i++) { p[i]=(uint8_t)v; v >>= 8; }
}
static uint64_t get(const uint8_t *p, unsigned n) {
    uint64_t v=0; for (unsigned i=0; i<n; i++) v |= (uint64_t)p[i] << (8*i); return v;
}
static void emit(struct zm_modem *m, const char *s) {
    if (!m->io.serial(m->io.ctx, (const uint8_t *)s, strlen(s))) m->serial_failed=true;
}
static void result(struct zm_modem *m, unsigned code) {
    static const char *const names[] = {"OK", "CONNECT", "RING", "NO CARRIER", "ERROR",
                                       "", "NO DIALTONE", "BUSY", "NO ANSWER"};
    char s[40];
    if (m->quiet) return;
    if (m->verbose) snprintf(s,sizeof(s),"\r\n%s\r\n", names[code]);
    else snprintf(s,sizeof(s),"%u\r",code);
    emit(m,s);
}
static void send_to(struct zm_modem *m, unsigned type, uint64_t dst, uint64_t session,
                    uint32_t seq, const uint8_t *data, size_t n) {
    uint8_t f[ZM_FRAME] = {'Z','M',1,0};
    f[3]=(uint8_t)type;
    put(f+4,m->number,8); put(f+12,dst,8); put(f+20,session,8); put(f+28,seq,4);
    if(n) memcpy(f+ZM_HEADER,data,n);
    m->frames_tx++;
    m->io.radio(m->io.ctx,f,ZM_HEADER+n);
}
static void send(struct zm_modem *m, unsigned type) {
    send_to(m,type,m->peer,m->session,0,NULL,0);
}
static void clear_stream(struct zm_modem *m) {
    m->qhead=m->qlen=m->pending_len=0; m->inflight=false;
    m->tx_seq=m->rx_seq=0; m->escape_count=0; m->command_len=0; m->overflow=false;
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
    m->state=ZM_CONNECTED; m->online=true; m->last_rx=m->last_ping=m->now;
    m->last_input=m->now; result(m,1);
}
static void defaults(struct zm_modem *m) {
    m->echo=true; m->verbose=true; m->quiet=false;
    m->autoanswer=0; m->dial_seconds=30; m->guard_ms=1000;
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
static void command(struct zm_modem *m) {
    char *s=m->command; char out[200]; uint64_t n;
    s[m->command_len]=0;
    for(char *p=s; *p; p++) *p=(char)toupper((unsigned char)*p);
    if(m->overflow || s[0]!='A' || s[1]!='T') { result(m,4); return; }
    const char *p=s+2;
    while(*p) {
        char c=*p++;
        if(c==' ') continue;
        if(c=='+') {
            if(strcmp(p,"PHONUM?")==0) {
                snprintf(out,sizeof(out),"\r\n%" PRIu64 "\r\n",m->number); emit(m,out);
                p+=strlen(p);
            } else if(strcmp(p,"STATS?")==0) {
                snprintf(out,sizeof(out),"\r\nTX=%" PRIu32 " RX=%" PRIu32
                    " RETRY=%" PRIu32 " OVERRUN=%" PRIu32 " IN=%" PRIu32
                    " ACKED=%" PRIu32 " FAULT=%" PRIu32 "\r\n",
                    m->frames_tx,m->frames_rx,m->retries,m->overruns,
                    m->bytes_rx,m->bytes_acked,m->last_fault);
                emit(m,out); p+=strlen(p);
            } else { result(m,4); return; }
        } else if(c=='I') {
            if(*p=='0') p++;
            emit(m,"\r\nZigmodem 0.1 experimental\r\n");
        } else if(c=='E' || c=='V' || c=='Q') {
            if(*p!='0' && *p!='1') { result(m,4); return; }
            bool b=*p++=='1';
            if(c=='E') m->echo=b; else if(c=='V') m->verbose=b; else m->quiet=b;
        } else if(c=='Z' || c=='&') {
            if(c=='&' && *p++!='F') { result(m,4); return; }
            if(*p=='0') p++;
            close_call(m,false); defaults(m);
        } else if(c=='H') {
            if(*p=='0') p++;
            close_call(m,false);
        } else if(c=='O') {
            if(*p=='0') p++;
            if(*p || m->state!=ZM_CONNECTED) { result(m,4); return; }
            m->online=true; m->last_input=m->now; result(m,1); return;
        } else if(c=='A') {
            if(*p || m->state!=ZM_RINGING) { result(m,4); return; }
            answer(m); return;
        } else if(c=='D') {
            if(*p=='T' || *p=='P') p++;
            if(!decimal(&p,&n) || *p || !n || n==m->number || m->state!=ZM_IDLE) {
                result(m,4); return;
            }
            clear_stream(m); m->peer=n;
            /* Boot nonce must come from platform entropy. Never reuse a session. */
            if(++m->nonce==0) ++m->nonce;
            m->session=m->nonce; m->state=ZM_DIALING;
            m->started=m->last_control=m->now; send(m,ZM_CALL); return;
        } else if(c=='S') {
            uint64_t reg;
            if(!decimal(&p,&reg) || (reg!=0 && reg!=7 && reg!=12)) { result(m,4); return; }
            if(*p=='?') {
                p++; unsigned v=reg==0?m->autoanswer:reg==7?m->dial_seconds:m->guard_ms/20;
                snprintf(out,sizeof(out),"\r\n%u\r\n",v); emit(m,out);
            } else if(*p++=='=' && decimal(&p,&n) && n<=255 && (reg==0 || n>0)) {
                if(reg==0) m->autoanswer=(unsigned)n;
                else if(reg==7) m->dial_seconds=(unsigned)n;
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
    while(m->escape_count) { enqueue(m,'+'); m->escape_count--; }
    return true;
}
size_t zm_input(struct zm_modem *m, const uint8_t *data, size_t n) {
    size_t i;
    for(i=0;i<n;i++) {
        uint8_t b=data[i];
        if(m->online) {
            if(m->escape_count && b=='+' && m->escape_count<3 &&
               m->now-m->escape_at<m->guard_ms) {
                m->escape_count++; m->escape_at=m->last_input=m->now; continue;
            }
            if(!m->escape_count && b=='+' && m->now-m->last_input>=m->guard_ms) {
                m->escape_count=1; m->escape_at=m->last_input=m->now; continue;
            }
            if(!flush_escape(m) || !enqueue(m,b)) break;
            m->last_input=m->now;
        } else {
            if(m->echo && !m->io.serial(m->io.ctx,&b,1)) break;
            if(b=='\r') {
                command(m); m->command_len=0; m->overflow=false;
            } else if(b==8 || b==127) {
                if(m->command_len) m->command_len--;
            } else if(b!='\n') {
                if(m->command_len<sizeof(m->command)-1) m->command[m->command_len++]=(char)b;
                else m->overflow=true;
            }
        }
    }
    return i;
}
void zm_receive(struct zm_modem *m, const uint8_t *f, size_t n) {
    if(n<ZM_HEADER || n>ZM_FRAME || f[0]!='Z' || f[1]!='M' || f[2]!=1 ||
       get(f+12,8)!=m->number) return;
    unsigned type=f[3]; uint64_t src=get(f+4,8), session=get(f+20,8);
    uint32_t seq=(uint32_t)get(f+28,4);
    if(!src || src==m->number || !session || type<ZM_CALL || type>ZM_CLOSED ||
       (type!=ZM_DATA && n!=ZM_HEADER) || (type==ZM_DATA && n==ZM_HEADER)) return;
    m->frames_rx++;
    if(type==ZM_CALL) {
        if(m->state==ZM_IDLE) {
            clear_stream(m); m->peer=src; m->session=session; m->state=ZM_RINGING;
            m->started=m->last_control=m->now; m->rings=1; result(m,2);
            if(m->autoanswer==1) answer(m);
        } else if(m->peer==src && m->session==session &&
                  (m->state==ZM_ANSWERING || m->state==ZM_CONNECTED)) send(m,ZM_ACCEPT);
        else if(!(m->peer==src && m->session==session && m->state==ZM_RINGING))
            send_to(m,ZM_BUSY,src,session,0,NULL,0);
        return;
    }
    if(src!=m->peer || session!=m->session) return;
    if(type==ZM_CLOSE) {
        send(m,ZM_CLOSED);
        if(m->state!=ZM_IDLE && m->state!=ZM_CLOSING) { idle(m); result(m,3); }
        else idle(m);
        return;
    }
    if(type==ZM_CLOSED && m->state==ZM_CLOSING) { idle(m); return; }
    if(type==ZM_BUSY && m->state==ZM_DIALING) { idle(m); result(m,7); return; }
    if(type==ZM_ACCEPT && (m->state==ZM_DIALING || m->state==ZM_CONNECTED)) {
        send(m,ZM_CONFIRM);
        if(m->state==ZM_DIALING) connected(m);
        return;
    }
    if(m->state==ZM_ANSWERING && (type==ZM_CONFIRM || type==ZM_DATA || type==ZM_PING))
        connected(m);
    if(m->state!=ZM_CONNECTED) return;
    m->last_rx=m->now;
    if(type==ZM_PING) send(m,ZM_PONG);
    if(type==ZM_DATA) {
        if(seq==m->rx_seq) {
            /* Hold remote data while locally in command mode. */
            if(!m->online || !m->io.serial(m->io.ctx,f+ZM_HEADER,n-ZM_HEADER)) return;
            m->rx_seq++;
            m->bytes_rx+=(uint32_t)(n-ZM_HEADER);
        } else if(seq!=m->rx_seq-1 || m->rx_seq==0) return;
        send_to(m,ZM_ACK,m->peer,m->session,seq,NULL,0);
    } else if(type==ZM_ACK && m->inflight && seq==m->tx_seq) {
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
        if(now-m->last_rx>=10000) { m->last_fault=2; close_call(m,true); break; }
        if(now-m->last_ping>=1000+m->number%251) { send(m,ZM_PING); m->last_ping=now; }
        if(m->escape_count && now-m->escape_at>=m->guard_ms) {
            if(m->escape_count==3) { m->escape_count=0; m->online=false; result(m,0); }
            else (void)flush_escape(m);
        }
        if(!m->inflight && m->qlen) {
            m->pending_len=m->qlen<ZM_PAYLOAD?m->qlen:ZM_PAYLOAD;
            for(size_t i=0;i<m->pending_len;i++) {
                m->pending[i]=m->queue[m->qhead]; m->qhead=(m->qhead+1)%ZM_QUEUE;
            }
            m->qlen-=m->pending_len; m->inflight=true; m->last_tx=now;
            m->retry_ms=retry_delay(m);
            send_to(m,ZM_DATA,m->peer,m->session,m->tx_seq,m->pending,m->pending_len);
        } else if(m->inflight && now-m->last_tx>=m->retry_ms) {
            m->last_tx=now;
            m->retries++; m->retry_ms=retry_delay(m);
            send_to(m,ZM_DATA,m->peer,m->session,m->tx_seq,m->pending,m->pending_len);
        }
        break;
    case ZM_IDLE: break;
    }
}
void zm_overrun(struct zm_modem *m) { m->overruns++; m->last_fault=1; close_call(m,true); }
