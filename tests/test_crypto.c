/* SPDX-License-Identifier: MIT */
#include <noise/protocol.h>
#include "codec.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "noise_vector.h"
static size_t unhex(const char *s,uint8_t *p) {
    size_t n=strlen(s)/2; for(size_t i=0;i<n;i++) { unsigned x; assert(sscanf(s+2*i,"%2x",&x)==1); p[i]=(uint8_t)x; } return n;
}
int main(void) {
    NoiseDHState *local,*remote; uint8_t secret[32]={1},zero[32]={0},shared[32];
    assert(!noise_dhstate_new_by_id(&local,NOISE_DH_CURVE25519));
    assert(!noise_dhstate_new_by_id(&remote,NOISE_DH_CURVE25519));
    assert(!noise_dhstate_set_keypair_private(local,secret,32));
    assert(!noise_dhstate_set_public_key(remote,zero,32));
    assert(noise_dhstate_calculate(local,remote,shared,32)!=0);
    noise_dhstate_free(local); noise_dhstate_free(remote);
    NoiseHandshakeState *hs[2]; NoiseCipherState *tx[2],*rx[2];
    for(unsigned i=0;i<2;i++) {
        uint8_t key[32],prologue[32];
        assert(!noise_handshakestate_new_by_name(&hs[i],"Noise_XX_25519_ChaChaPoly_BLAKE2s",i?NOISE_ROLE_RESPONDER:NOISE_ROLE_INITIATOR));
        unhex(i?v_resp_static:v_init_static,key);
        assert(!noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(hs[i]),key,32));
        unhex(i?v_resp_ephemeral:v_init_ephemeral,key);
        assert(!noise_dhstate_set_keypair_private(noise_handshakestate_get_fixed_ephemeral_dh(hs[i]),key,32));
        size_t n=unhex(v_init_prologue,prologue);
        assert(!noise_handshakestate_set_prologue(hs[i],prologue,n));
        assert(!noise_handshakestate_start(hs[i]));
    }
    for(unsigned i=0;i<6;i++) {
        uint8_t buf[256],payload[128],expected[256],decoded[128];
        size_t plen=unhex(v_payload[i],payload),n=unhex(v_cipher[i],expected);
        NoiseBuffer b,p,out; noise_buffer_set_output(b,buf,sizeof(buf));
        noise_buffer_set_input(p,payload,plen); noise_buffer_set_output(out,decoded,sizeof(decoded));
        unsigned sender=i%2,receiver=1-sender;
        if(i<3) assert(!noise_handshakestate_write_message(hs[sender],&b,&p));
        else { memcpy(buf,payload,plen); noise_buffer_set_inout(b,buf,plen,sizeof(buf)); assert(!noise_cipherstate_encrypt(tx[sender],&b)); }
        assert(b.size==n && !memcmp(buf,expected,n));
        if(i<3) { assert(!noise_handshakestate_read_message(hs[receiver],&b,&out)); assert(out.size==plen && !memcmp(decoded,payload,plen)); }
        else { assert(!noise_cipherstate_decrypt(rx[receiver],&b)); assert(b.size==plen && !memcmp(buf,payload,plen)); }
        if(i==2) for(unsigned j=0;j<2;j++) assert(!noise_handshakestate_split(hs[j],&tx[j],&rx[j]));
    }
    for(unsigned j=0;j<2;j++) { noise_handshakestate_free(hs[j]); noise_cipherstate_free(tx[j]); noise_cipherstate_free(rx[j]); }
    for(unsigned length=1;length<=96;length++) {
        uint8_t raw[96],encoded[128],decoded[96];
        for(unsigned mode=0;mode<2;mode++) {
            for(unsigned i=0;i<length;i++) raw[i]=mode?(uint8_t)(i*73):'A';
            size_t n=zm_compress(raw,length,encoded,sizeof(encoded)); assert(n);
            assert(zm_decompress(encoded,n,decoded,length)); assert(!memcmp(raw,decoded,length));
            assert(!zm_decompress(encoded,n,decoded,length-1));
        }
    }
    puts("PASS: independent Noise XX vectors and bounded compression round trips"); return 0;
}
