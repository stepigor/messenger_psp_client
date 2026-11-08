/* Track curl-owned sockets and enforce nonblocking I/O for the mbedTLS BIO.
 * dns.c owns its short-lived sockets and observes cancellation itself. */
#include "transport.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
static volatile int sockets[8]={-1,-1,-1,-1,-1,-1,-1,-1};
static StSocketStats stats;
static volatile int protected_socket=-1;
void st_network_protect(int fd){protected_socket=fd;__sync_synchronize();}
StSocketStats st_psp_socket_stats(void){StSocketStats copy=stats;copy.live=0;for(int i=0;i<8;i++)if(sockets[i]>=0)copy.live++;return copy;}
static curl_socket_t socket_open(void *ctx,curlsocktype type,struct curl_sockaddr *address) {
    (void)ctx;(void)type;if(st_cancelled())return CURL_SOCKET_BAD;
    int slot;for(slot=0;slot<8&&sockets[slot]>=0;slot++);
    if(slot==8){stats.open_errors++;return CURL_SOCKET_BAD;}
    int fd=socket(address->family,address->socktype,address->protocol);
    if(fd<0){stats.open_errors++;return CURL_SOCKET_BAD;}
    sockets[slot]=fd;stats.opened++;__sync_synchronize();return fd;
}
static int socket_options(void *ctx,curl_socket_t fd,curlsocktype type) {
    (void)ctx;(void)type;
    if(st_cancelled())return CURL_SOCKOPT_ERROR;
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return CURL_SOCKOPT_ERROR;
    st_stage("TCP connect");return CURL_SOCKOPT_OK;
}
static int socket_close(void *ctx,curl_socket_t fd) {
    (void)ctx;
    for(int i=0;i<8;i++)if(sockets[i]==fd)sockets[i]=-1;
    if(protected_socket==fd)protected_socket=-1;
    __sync_synchronize();int result=close(fd);stats.closed++;if(result<0)stats.close_errors++;return result;
}
void st_psp_transport_options(CURL *h) {
    curl_easy_setopt(h,CURLOPT_OPENSOCKETFUNCTION,socket_open);
    curl_easy_setopt(h,CURLOPT_SOCKOPTFUNCTION,socket_options);
    curl_easy_setopt(h,CURLOPT_CLOSESOCKETFUNCTION,socket_close);
}
void st_network_abort(void) {
    /* Shutdown wakes I/O; only the owner closes/deletes objects and frees memory. */
    /* Reused connections do not repeat SOCKOPTFUNCTION. Include every owned
     * socket so an old descriptor cannot replace the active one in tracking. */
    for(int i=0;i<8;i++){int fd=sockets[i];if(fd>=0&&fd!=protected_socket)shutdown(fd,SHUT_RDWR);}
}
void st_network_abort_all(void){protected_socket=-1;st_network_abort();}
#ifdef ST_NET_TEST
int st_test_socket_open(void){struct curl_sockaddr address={.family=AF_INET,.socktype=SOCK_STREAM,.protocol=0};return socket_open(NULL,CURLSOCKTYPE_IPCXN,&address);}
int st_test_socket_options(int fd){return socket_options(NULL,fd,CURLSOCKTYPE_IPCXN);}
int st_test_socket_close(int fd){return socket_close(NULL,fd);}
#endif
