/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 600
#include "modem.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct host { int udp,pty; struct sockaddr_in peer; uint8_t out[16384]; size_t len; };
static uint64_t milliseconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static bool serial_out(void *ctx,const uint8_t *p,size_t n) {
    struct host *h=ctx;
    if(n>sizeof(h->out)-h->len) return false;
    memcpy(h->out+h->len,p,n); h->len+=n; return true;
}
static void radio_out(void *ctx,const uint8_t *p,size_t n) {
    struct host *h=ctx;
    (void)sendto(h->udp,p,n,0,(struct sockaddr *)&h->peer,sizeof(h->peer));
}
int main(int argc,char **argv) {
    if(argc!=4) { fprintf(stderr,"usage: %s NUMBER LOCAL_UDP_PORT PEER_UDP_PORT\n",argv[0]); return 2; }
    struct host h={0}; struct zm_modem m;
    h.udp=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),
                             .sin_port=htons((uint16_t)atoi(argv[2]))};
    if(h.udp<0 || bind(h.udp,(struct sockaddr *)&addr,sizeof(addr))) { perror("bind"); return 1; }
    h.peer=addr; h.peer.sin_port=htons((uint16_t)atoi(argv[3]));
    h.pty=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(h.pty<0 || grantpt(h.pty) || unlockpt(h.pty)) { perror("pty"); return 1; }
    int slave=open(ptsname(h.pty),O_RDWR|O_NOCTTY);
    struct termios t; tcgetattr(slave,&t);
    t.c_iflag=0; t.c_oflag=0; t.c_lflag=0; t.c_cflag=CS8|CREAD|CLOCAL;
    t.c_cc[VMIN]=1; t.c_cc[VTIME]=0; tcsetattr(slave,TCSANOW,&t);
    uint64_t nonce; int rng=open("/dev/urandom",O_RDONLY);
    if(rng<0 || read(rng,&nonce,sizeof(nonce))!=sizeof(nonce)) return 1;
    close(rng);
    zm_init(&m,strtoull(argv[1],NULL,10),nonce,(struct zm_io){&h,radio_out,serial_out});
    printf("%s\n",ptsname(h.pty)); fflush(stdout);
    uint8_t input[4096]; size_t pending=0;
    for(;;) {
        zm_tick(&m,milliseconds());
        struct pollfd p[2]={{h.udp,POLLIN,0},{h.pty,(short)((pending?0:POLLIN)|(h.len?POLLOUT:0)),0}};
        if(poll(p,2,5)<0 && errno!=EINTR) return 1;
        if(p[0].revents&POLLIN) {
            uint8_t frame[256]; struct sockaddr_in src; socklen_t sl=sizeof(src);
            ssize_t n=recvfrom(h.udp,frame,sizeof(frame),0,(struct sockaddr *)&src,&sl);
            if(n>0 && src.sin_addr.s_addr==h.peer.sin_addr.s_addr && src.sin_port==h.peer.sin_port)
                zm_receive(&m,frame,(size_t)n);
        }
        if(p[1].revents&POLLOUT) {
            ssize_t n=write(h.pty,h.out,h.len);
            if(n>0) { h.len-=(size_t)n; memmove(h.out,h.out+n,h.len); }
        }
        if(p[1].revents&POLLIN) { ssize_t n=read(h.pty,input,sizeof(input)); if(n>0) pending=(size_t)n; }
        if(pending) { size_t n=zm_input(&m,input,pending); pending-=n; memmove(input,input+n,pending); }
    }
}
