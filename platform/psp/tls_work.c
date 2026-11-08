/* App-local adapters for the installed curl 7.64.1 / mbedTLS 2.28 backend.
 * Do not change certificates, verification flags, ciphers or cryptographic steps.
 * Each BIO call is explicitly nonblocking, independent of cached fcntl flags.
 * Walk the same handshake steps as mbedTLS, observing time between steps. */
#include "psp_tls.h"
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <sys/socket.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#ifdef __PSP__
#include <pspthreadman.h>
#endif

static StPspTlsStats stats;
static uint64_t deadline;
StPspTlsStats st_psp_tls_stats(void){return stats;}
void st_psp_tls_begin(void){memset(&stats,0,sizeof(stats));deadline=st_millis()+15000;}
static unsigned millis(uint64_t elapsed){return elapsed>UINT_MAX?UINT_MAX:(unsigned)elapsed;}
static void add(unsigned *value,unsigned n){*value=n>UINT_MAX-*value?UINT_MAX:*value+n;}
static uint64_t cpu_time(void) {
#ifdef __PSP__
    SceKernelThreadRunStatus info;memset(&info,0,sizeof(info));info.size=sizeof(info);unsigned seconds,microseconds;
    if(sceKernelReferThreadRunStatus(sceKernelGetThreadId(),&info)>=0&&sceKernelSysClock2USec(&info.runClocks,&seconds,&microseconds)>=0)
        return (uint64_t)seconds*1000000+microseconds;
#endif
    return UINT64_MAX;
}
static const char *phase(int state) {
    switch(state) {
        case MBEDTLS_SSL_HELLO_REQUEST:return "TLS start";
        case MBEDTLS_SSL_CLIENT_HELLO:return "TLS client hello";
        case MBEDTLS_SSL_SERVER_HELLO:return "TLS server hello";
        case MBEDTLS_SSL_SERVER_CERTIFICATE:return "TLS certificate";
        case MBEDTLS_SSL_SERVER_KEY_EXCHANGE:return "TLS server key";
        case MBEDTLS_SSL_CLIENT_KEY_EXCHANGE:return "TLS client key";
        case MBEDTLS_SSL_SERVER_FINISHED:return "TLS server finished";
        case MBEDTLS_SSL_CLIENT_FINISHED:return "TLS client finished";
        default:return "TLS handshake step";
    }
}
#ifdef __PSP__
int __real_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl);
#else
#define __real_mbedtls_ssl_handshake mbedtls_ssl_handshake
#endif
int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl) {
    if(!ssl||!ssl->conf||ssl->conf->transport!=MBEDTLS_SSL_TRANSPORT_STREAM||ssl->conf->endpoint!=MBEDTLS_SSL_IS_CLIENT)
        return __real_mbedtls_ssl_handshake(ssl);
    while(ssl->state!=MBEDTLS_SSL_HANDSHAKE_OVER) {
        if(st_cancelled()||st_millis()>=deadline){stats.expired=!st_cancelled();return stats.result=MBEDTLS_ERR_SSL_TIMEOUT;}
        stats.phase=(unsigned)ssl->state;st_stage(phase(ssl->state));
        uint64_t start=st_millis(),cpu_start=cpu_time();int ret=mbedtls_ssl_handshake_step(ssl);uint64_t cpu_end=cpu_time();
        unsigned elapsed=millis(st_millis()-start);stats.steps++;stats.result=ret;
        if(elapsed>stats.max_step_ms)stats.max_step_ms=elapsed;
        if(cpu_start!=UINT64_MAX&&cpu_end!=UINT64_MAX&&cpu_end>=cpu_start){stats.cpu_valid=1;add(&stats.cpu_ms,millis((cpu_end-cpu_start)/1000));}
        if(ret&&ret!=MBEDTLS_ERR_SSL_WANT_READ&&ret!=MBEDTLS_ERR_SSL_WANT_WRITE)return ret;
        if(st_cancelled()||st_millis()>=deadline){stats.expired=!st_cancelled();return stats.result=MBEDTLS_ERR_SSL_TIMEOUT;}
        if(ret)return ret;
    }
    return stats.result=0;
}
static int io(void *ctx,unsigned char *buffer,size_t size,int writing) {
    if(!ctx||((mbedtls_net_context *)ctx)->fd<0)return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    if(st_cancelled())return writing?MBEDTLS_ERR_NET_SEND_FAILED:MBEDTLS_ERR_NET_RECV_FAILED;
    int fd=((mbedtls_net_context *)ctx)->fd;uint64_t start=st_millis();
    int ret=(int)(writing?send(fd,buffer,size,MSG_DONTWAIT):recv(fd,buffer,size,MSG_DONTWAIT));int code=errno;
    unsigned elapsed=millis(st_millis()-start);if(elapsed>stats.max_io_ms)stats.max_io_ms=elapsed;
    if(writing)stats.writes++;else stats.reads++;
    if(ret>=0){add(writing?&stats.sent:&stats.received,(unsigned)ret);return ret;}
    if(code==EAGAIN||code==EWOULDBLOCK||code==EINTR)return writing?MBEDTLS_ERR_SSL_WANT_WRITE:MBEDTLS_ERR_SSL_WANT_READ;
    if(code==EPIPE||code==ECONNRESET)return MBEDTLS_ERR_NET_CONN_RESET;
    return writing?MBEDTLS_ERR_NET_SEND_FAILED:MBEDTLS_ERR_NET_RECV_FAILED;
}
int __wrap_mbedtls_net_recv(void *ctx,unsigned char *buffer,size_t size){return io(ctx,buffer,size,0);}
int __wrap_mbedtls_net_send(void *ctx,const unsigned char *buffer,size_t size){return io(ctx,(unsigned char *)buffer,size,1);}
