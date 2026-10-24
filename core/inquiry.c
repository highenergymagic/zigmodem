/* SPDX-License-Identifier: MIT */
#include "modem.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static void text(struct zm_modem *m,const char *s) {
    zm_emit(m,s);
}
void zm_show_profile(struct zm_modem *m,const uint8_t p[ZM_PROFILE_SIZE]) {
    char s[320];
    if(!zm_profile_valid(p)) { text(m,"\r\nEMPTY OR INVALID\r\n"); return; }
    snprintf(s,sizeof(s),"\r\nPERSONALITY=%s E%u V%u Q%u X%u REPORT=%u\r\n"
        "FLOW_TX=%u FLOW_RX=%u COMPRESSION=%u SURVEY=%u\r\n"
        "S0=%u S2=%u S3=%u S4=%u S5=%u S7=%u S12=%u S13=%u S19=%u +MS=%u,%u,%u\r\n",
        p[12]?"USR":"NATIVE",p[4],p[5],p[6],p[20],p[19],p[7],p[8],p[9],p[11],
        p[13],p[14],p[21],p[22],p[23],p[15],p[16],p[25]?128:0,p[24],p[10],zm_rate(p[17]),zm_rate(p[18])); text(m,s);
}
bool zm_inquiry(struct zm_modem *m,unsigned id) {
    char s[240]; uint8_t p[ZM_PROFILE_SIZE];
    switch(id) {
    case 0: text(m,"\r\n2880\r\n"); break;
    case 1:
        if(!m->firmware_crc_valid) return false;
        snprintf(s,sizeof(s),"\r\n%" PRIu32 "\r\n",m->firmware_crc); text(m,s); break;
    case 2: {
        /* Non-destructive scratch RAM test; never overwrite live modem state. */
        volatile uint8_t scratch[64];
        for(unsigned pattern=0;pattern<256;pattern+=85) {
            for(unsigned i=0;i<sizeof(scratch);i++) scratch[i]=(uint8_t)(pattern^i);
            for(unsigned i=0;i<sizeof(scratch);i++) if(scratch[i]!=(uint8_t)(pattern^i)) return false;
        }
        text(m,"\r\nOK\r\n"); break;
    }
    case 3: text(m,"\r\nZigmodem 0.5 experimental - 28800 encrypted radio modem\r\n"); break;
    case 4:
        text(m,"\r\nACTIVE PROFILE\r\n"); zm_profile_encode(m,p); zm_show_profile(m,p); break;
    case 5:
        for(unsigned i=0;i<2;i++) {
            snprintf(s,sizeof(s),"\r\nSTORED PROFILE %u%s\r\n",i,i==m->boot_profile?" (BOOT)":""); text(m,s);
            memset(p,0,sizeof(p));
            if(m->storage.read) (void)m->storage.read(m->storage.ctx,i,p,sizeof(p));
            zm_show_profile(m,p);
        }
        break;
    case 6:
    case 11:
        snprintf(s,sizeof(s),"\r\n%sLINK DIAGNOSTICS (COUNTERS SINCE BOOT)\r\n"
            "Rate %u  Channel %u  RTT %" PRIu32 " ms\r\n"
            "Received %" PRIu32 "  Acknowledged %" PRIu32 "  Retries %" PRIu32 "\r\n"
            "Overruns %" PRIu32 "  Fault %" PRIu32 "  Retrains %" PRIu32 "\r\n",
            id==11?"EXTENDED ":"",zm_rate(m->profile),m->channel,m->rtt_ms,m->bytes_rx,m->bytes_acked,m->retries,m->overruns,m->last_fault,m->retrains); text(m,s);
        if(id==11) {
            snprintf(s,sizeof(s),"AUTH=%" PRIu32 " REJECT=%" PRIu32 " SAVED=%" PRIu32 " SCANS=%" PRIu32 " MOVES=%" PRIu32 "\r\n",
                m->authenticated_frames,m->rejected_frames,m->compression_saved,m->surveys,m->survey_moves); text(m,s);
        }
        break;
    case 7:
        text(m,"\r\nHigh Energy Magic Zigmodem\r\n"
            "IEEE 802.15.4 / NOISE XX / SELECTIVE REPEAT / HEATSHRINK\r\n"
            "UART 115200 8N1; optional XON/XOFF\r\n"
            "Data only; no PSTN, fax, voice, hardware modem signals\r\n"); break;
    case 9:
        text(m,"\r\nHGM2880 Zigmodem encrypted radio modem\r\n"); break;
    default: return false;
    }
    return true;
}
