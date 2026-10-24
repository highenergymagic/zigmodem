/* SPDX-License-Identifier: MIT */
#include "secure.h"
#include "codec.h"
#include <string.h>
#define HEAD 44
#define CHUNK 64
extern int zm_noise_decrypt_at(NoiseCipherState *,uint64_t,const uint8_t *,size_t,NoiseBuffer *);
enum { HS1=1,HS2,HS3,DATA,READY };
static uint64_t get(const uint8_t *p,unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(i*8); return v;
}
static void put(uint8_t *p,uint64_t v,unsigned n) {
    for(unsigned i=0;i<n;i++) { p[i]=(uint8_t)v; v>>=8; }
}
static bool fingerprint(const uint8_t pub[32],uint8_t id[16]) {
    NoiseHashState *h=NULL; uint8_t digest[32];
    bool ok=noise_hashstate_new_by_id(&h,NOISE_HASH_BLAKE2s)==0 &&
        noise_hashstate_hash_one(h,pub,32,digest,sizeof(digest))==0;
    if(ok) memcpy(id,digest,16);
    if(h) noise_hashstate_free(h);
    return ok;
}
bool zs_identity(const uint8_t key[32],uint8_t id[16]) {
    NoiseDHState *d=NULL; uint8_t pub[32];
    bool ok=noise_dhstate_new_by_id(&d,NOISE_DH_CURVE25519)==0 &&
        noise_dhstate_set_keypair_private(d,key,32)==0 &&
        noise_dhstate_get_public_key(d,pub,32)==0 && fingerprint(pub,id);
    if(d) noise_dhstate_free(d);
    return ok;
}
void zs_reset(struct zs_link *s) {
    if(s->hs) noise_handshakestate_free(s->hs);
    if(s->tx) noise_cipherstate_free(s->tx);
    if(s->rx) noise_cipherstate_free(s->rx);
    s->hs=NULL; s->tx=s->rx=NULL; s->ready=false; s->stage=0;
    s->cached_len=s->pending_len=s->assembly_mask=0;
    s->tx_counter=s->rx_top=s->rx_bits=0;
    noise_clean(s->cached,sizeof(s->cached)); noise_clean(s->assembly,sizeof(s->assembly));
    noise_clean(s->pending,sizeof(s->pending));
}
bool zs_init(struct zs_link *s,const uint8_t key[32],struct zs_io io) {
    memset(s,0,sizeof(*s)); s->io=io; memcpy(s->private_key,key,32);
    if(!zs_identity(key,s->identity)) return false;
    s->number=zm_phone(s->identity); return s->number!=0;
}
static void header(struct zs_link *s,uint8_t h[HEAD],unsigned type,uint64_t counter) {
    memset(h,0,HEAD); h[0]='Z'; h[1]='S'; h[2]=5; h[3]=(uint8_t)type;
    put(h+4,s->number,8); put(h+12,s->peer,8); put(h+20,s->session,8); put(h+28,counter,8);
    /* Bytes 36..39 reserved and authenticated. */
}
static void record(struct zs_link *s,uint8_t h[HEAD],const uint8_t *p,size_t n) {
    put(h+42,n,2);
    for(size_t off=0;off<n;off+=CHUNK) {
        uint8_t f[ZS_WIRE_MAX]; size_t count=n-off<CHUNK?n-off:CHUNK;
        h[40]=(uint8_t)(off/CHUNK); h[41]=(uint8_t)((n+CHUNK-1)/CHUNK);
        memcpy(f,h,HEAD); memcpy(f+HEAD,p+off,count); s->io.wire(s->io.ctx,f,HEAD+count);
    }
}
static void resend(struct zs_link *s) {
    uint8_t h[HEAD]; header(s,h,s->cached_type,0); record(s,h,s->cached,s->cached_len);
    s->last_send=s->now;
}
static bool start(struct zs_link *s,bool initiator) {
    s->initiator=initiator; s->started=s->now;
    uint8_t prologue[28]={'Z','M','5',0};
    put(prologue+4,initiator?s->number:s->peer,8);
    put(prologue+12,initiator?s->peer:s->number,8); put(prologue+20,s->session,8);
    return noise_handshakestate_new_by_name(&s->hs,"Noise_XX_25519_ChaChaPoly_BLAKE2s",
        initiator?NOISE_ROLE_INITIATOR:NOISE_ROLE_RESPONDER)==0 &&
        noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(s->hs),s->private_key,32)==0 &&
        noise_handshakestate_set_prologue(s->hs,prologue,sizeof(prologue))==0 &&
        noise_handshakestate_start(s->hs)==0;
}
static bool write_hs(struct zs_link *s,unsigned type) {
    NoiseBuffer b; noise_buffer_set_output(b,s->cached,sizeof(s->cached));
    if(noise_handshakestate_write_message(s->hs,&b,NULL)) return false;
    s->cached_len=b.size; s->cached_type=type; resend(s); return true;
}
static bool split(struct zs_link *s) {
    if(noise_handshakestate_split(s->hs,&s->tx,&s->rx)) return false;
    noise_handshakestate_free(s->hs); s->hs=NULL; return true;
}
/* Established records fit one MAC packet. Noise keys already bind both
 * identities and session; only type and the explicit nonce need airtime. */
static void encrypted(struct zs_link *s,unsigned type,const uint8_t *p,size_t n) {
    uint8_t wire[ZS_WIRE_MAX]={'Z','S',5,0},compact[13+ZM_PAYLOAD]; NoiseBuffer b;
    if(!s->tx || s->tx_counter==UINT64_MAX) return;
    wire[3]=(uint8_t)type; put(wire+4,s->tx_counter++,8);
    size_t length=0;
    if(n) {
        compact[0]=p[3]; memcpy(compact+1,p+28,12); memcpy(compact+13,p+ZM_HEADER,n-ZM_HEADER);
        size_t plain=13+n-ZM_HEADER;
        size_t compressed=s->compression?zm_compress(compact,plain,wire+15,sizeof(wire)-31):0;
        wire[12]=(compressed && compressed<plain)?1:0; put(wire+13,plain,2);
        if(wire[12]) { s->compression_saved+=(uint32_t)(plain-compressed); length=compressed+3; }
        else { memcpy(wire+15,compact,plain); length=plain+3; }
    }
    noise_buffer_set_inout(b,wire+12,length,sizeof(wire)-12);
    if(noise_cipherstate_encrypt_with_ad(s->tx,wire,12,&b)) { zs_reset(s); return; }
    s->io.wire(s->io.ctx,wire,b.size+12); noise_clean(compact,sizeof(compact));
}
void zs_send(struct zs_link *s,const uint8_t *p,size_t n,const uint8_t expected[16]) {
    if(n<ZM_HEADER || n>ZM_FRAME) return;
    uint64_t peer=get(p+12,8),session=get(p+20,8);
    if(s->ready && peer==s->peer && session==s->session) {
        encrypted(s,DATA,p,n); return;
    }
    if(p[3]!=ZM_CALL) return;
    if(s->stage && peer==s->peer && session==s->session) return;
    zs_reset(s); s->peer=peer; s->session=session; memcpy(s->expected,expected,16);
    memcpy(s->pending,p,n); s->pending_len=n;
    if(!start(s,true) || !write_hs(s,HS1)) { zs_reset(s); return; }
    s->stage=1;
}
static bool remote(struct zs_link *s) {
    uint8_t pub[32],zero[16]={0};
    if(noise_dhstate_get_public_key(noise_handshakestate_get_remote_public_key_dh(s->hs),pub,32) ||
       !fingerprint(pub,s->remote_id) || zm_phone(s->remote_id)!=s->peer) return false;
    if(s->initiator && !noise_is_equal(s->expected,zero,16) && !noise_is_equal(s->remote_id,s->expected,16)) return false;
    /* Persistent pins are checked even for explicitly fingerprint-addressed calls. */
    return !s->io.trust || s->io.trust(s->io.ctx,s->remote_id);
}
static void receive_data(struct zs_link *s,const uint8_t *f,size_t n) {
    if(!s->rx || n<28 || n>ZS_WIRE_MAX) return;
    uint64_t c=get(f+4,8);
    if(c==UINT64_MAX || (s->rx_bits && c<=s->rx_top &&
        (s->rx_top-c>=64 || (s->rx_bits & (UINT64_C(1)<<(s->rx_top-c)))))) { s->rejected++; return; }
    uint8_t buf[ZS_WIRE_MAX]; memcpy(buf,f+12,n-12);
    NoiseBuffer b; noise_buffer_set_inout(b,buf,n-12,sizeof(buf));
    if(zm_noise_decrypt_at(s->rx,c,f,12,&b)) { s->rejected++; return; }
    if(!s->rx_bits || c>s->rx_top) {
        uint64_t shift=c-s->rx_top; s->rx_bits=shift>=64?1:(s->rx_bits<<shift)|1; s->rx_top=c;
    } else s->rx_bits|=UINT64_C(1)<<(s->rx_top-c);
    s->authenticated++;
    if(f[3]==READY && b.size==0 && s->initiator) {
        s->ready=true; s->stage=4;
        if(s->pending_len) { encrypted(s,DATA,s->pending,s->pending_len); s->pending_len=0; }
    } else if(f[3]==DATA && s->ready && b.size>=3) {
        uint8_t compact[13+ZM_PAYLOAD],decoded[ZM_FRAME]={'Z','M',1,0}; size_t length=(size_t)get(buf+1,2);
        if(length<13 || length>sizeof(compact) || buf[0]>1) return;
        if(buf[0]) { if(!zm_decompress(buf+3,b.size-3,compact,length)) return; }
        else { if(b.size-3!=length) return; memcpy(compact,buf+3,length); }
        decoded[3]=compact[0]; put(decoded+4,s->peer,8); put(decoded+12,s->number,8); put(decoded+20,s->session,8);
        memcpy(decoded+28,compact+1,12); memcpy(decoded+ZM_HEADER,compact+13,length-13);
        s->io.plain(s->io.ctx,decoded,ZM_HEADER+length-13);
    }
    noise_clean(buf,sizeof(buf));
}
static void complete(struct zs_link *s,unsigned type,uint8_t *buf,size_t n,const uint8_t h[40]) {
    (void)h;
    if(type==HS1 && s->stage==2) { resend(s); return; }
    if(type==HS2 && s->stage>=3 && s->initiator) { resend(s); return; }
    if(type==HS3 && s->stage==4 && !s->initiator) { encrypted(s,READY,NULL,0); return; }
    if(!s->hs || !((type==HS1 && s->stage==0) || (type==HS2 && s->stage==1) ||
                      (type==HS3 && s->stage==2))) return;
    NoiseBuffer b; noise_buffer_set_inout(b,buf,n,ZS_RECORD_MAX);
    if(noise_handshakestate_read_message(s->hs,&b,NULL)) { s->rejected++; zs_reset(s); return; }
    bool ok=true;
    if(type==HS1) { ok=write_hs(s,HS2); s->stage=2; }
    else if(type==HS2) { ok=remote(s) && write_hs(s,HS3) && split(s); s->stage=3; }
    else { ok=remote(s) && split(s); s->stage=4; s->ready=ok; if(ok) encrypted(s,READY,NULL,0); }
    if(!ok) { s->rejected++; zs_reset(s); }
}
void zs_receive(struct zs_link *s,const uint8_t *f,size_t n) {
    s->wire_rx++; s->last_type=n>3?f[3]:0; s->last_length=(unsigned)n;
    if(n>=4 && f[0]=='Z' && f[1]=='S' && f[2]==5 && (f[3]==DATA || f[3]==READY)) {
        receive_data(s,f,n); return;
    }
    if(n<=HEAD || n>ZS_WIRE_MAX || f[0]!='Z' || f[1]!='S' || f[2]!=5 ||
       f[3]<HS1 || f[3]>READY || get(f+12,8)!=s->number || get(f+36,4)) { s->invalid_wire++; return; }
    uint64_t src=get(f+4,8),sid=get(f+20,8); unsigned type=f[3],i=f[40],parts=f[41];
    size_t len=(size_t)get(f+42,2),off=i*CHUNK;
    if(!src || src==s->number || !sid || !len || len>ZS_RECORD_MAX ||
       parts!=(len+CHUNK-1)/CHUNK || i>=parts || off+n-HEAD>len ||
       n-HEAD!=(len-off<CHUNK?len-off:CHUNK)) { s->invalid_wire++; return; }
    if(((!s->hs && !s->tx) || src!=s->peer || sid!=s->session) && type==HS1) {
        if(!s->io.accept(s->io.ctx)) return;
        zs_reset(s); s->peer=src; s->session=sid;
        if(!start(s,false)) { zs_reset(s); return; }
    }
    if(src!=s->peer || sid!=s->session) return;
    if(s->assembly_len!=len || memcmp(s->assembly_header,f,40)) {
        memcpy(s->assembly_header,f,40); s->assembly_len=len; s->assembly_mask=0;
    }
    memcpy(s->assembly+off,f+HEAD,n-HEAD); s->assembly_mask|=1u<<i;
    if(s->assembly_mask==(1u<<parts)-1) {
        uint8_t assembled[ZS_RECORD_MAX],h[40];
        memcpy(assembled,s->assembly,len); memcpy(h,s->assembly_header,40); s->assembly_mask=0;
        complete(s,type,assembled,len,h); noise_clean(assembled,sizeof(assembled));
    }
}
void zs_tick(struct zs_link *s,uint64_t now) {
    s->now=now;
    if(s->stage && s->stage<4) {
        if(now-s->started>15000) zs_reset(s);
        else if(now-s->last_send>=(s->stage==1?150u:400u)) resend(s);
    }
}
