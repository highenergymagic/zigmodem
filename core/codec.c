/* SPDX-License-Identifier: MIT */
#include "codec.h"
#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"
size_t zm_compress(const uint8_t *p,size_t n,uint8_t *out,size_t cap) {
    heatshrink_encoder e; heatshrink_encoder_reset(&e); size_t consumed=0,total=0;
    if(n>256 || heatshrink_encoder_sink(&e,(uint8_t *)p,n,&consumed)<0 || consumed!=n) return 0;
    for(unsigned budget=0;budget<1024;budget++) {
        HSE_finish_res done=heatshrink_encoder_finish(&e); size_t got=0;
        if(total==cap) return 0;
        if(heatshrink_encoder_poll(&e,out+total,cap-total,&got)<0) return 0;
        total+=got; if(done==HSER_FINISH_DONE) return total;
    }
    return 0;
}
bool zm_decompress(const uint8_t *p,size_t n,uint8_t *out,size_t expected) {
    heatshrink_decoder d; heatshrink_decoder_reset(&d); size_t consumed=0,total=0;
    if(expected>256) return false;
    for(unsigned budget=0;budget<1024;budget++) {
        size_t took=0,got=0;
        if(consumed<n && heatshrink_decoder_sink(&d,(uint8_t *)p+consumed,n-consumed,&took)<0) return false;
        consumed+=took;
        uint8_t extra;
        if(heatshrink_decoder_poll(&d,total<expected?out+total:&extra,total<expected?expected-total:1,&got)<0) return false;
        if(total+got>expected) return false;
        total+=got;
        if(consumed==n && heatshrink_decoder_finish(&d)==HSDR_FINISH_DONE) return total==expected;
    }
    return false;
}
