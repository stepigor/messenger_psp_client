/* App-local replacement for libcglue select, linked with --wrap=select.
 * Firmware socket IDs range up to 255; the installed libcglue instead builds
 * its firmware masks in newlib's 64-bit fd_set. A native ID >=64 writes past
 * that mask even when the POSIX descriptor has been reused and is still small.
 * Native poll uses explicit IDs and never indexes a bit mask by a firmware ID.
 * The SDK itself remains untouched. Only the serial network worker calls this. */
#include "transport.h"
#include "libcglue_fd.h"
#include <pspnet_inet.h>
#include <pspthreadman.h>
#include <errno.h>
#include <string.h>

static StPollStats stats;
StPollStats st_psp_poll_stats(void){return stats;}

int __wrap_select(int n,fd_set *readfds,fd_set *writefds,fd_set *exceptfds,struct timeval *timeout) {
    if(n<0||n>FD_SETSIZE||(timeout&&(timeout->tv_sec<0||timeout->tv_usec<0||timeout->tv_usec>=1000000))) {
        errno=EINVAL;return -1;
    }
    struct SceNetInetPollfd native[FD_SETSIZE];int descriptors[FD_SETSIZE],used=0;
    for(int fd=0;fd<n;fd++) {
        short events=0;
        if(readfds&&FD_ISSET(fd,readfds))events|=SCE_NET_INET_POLLIN|SCE_NET_INET_POLLRDNORM;
        if(writefds&&FD_ISSET(fd,writefds))events|=SCE_NET_INET_POLLOUT;
        if(exceptfds&&FD_ISSET(fd,exceptfds))events|=SCE_NET_INET_POLLPRI|SCE_NET_INET_POLLRDBAND;
        if(!events)continue;
        /* curl/mbedTLS only select sockets. Reject unsupported descriptors
         * explicitly instead of silently waiting forever for a file or pipe. */
        const StLibcFd *entry=__descriptormap[fd];
        if(!entry||entry->type!=ST_LIBC_SOCKET){errno=EBADF;return -1;}
        native[used]=(struct SceNetInetPollfd){(int)entry->descriptor,events,0};
        descriptors[used++]=fd;stats.last_native=entry->descriptor;
        if(entry->descriptor>stats.maximum_native)stats.maximum_native=entry->descriptor;
    }
    uint64_t started=(uint64_t)sceKernelGetSystemTimeWide();
    uint64_t duration=0;
    if(timeout) {
        uint64_t seconds=(uint64_t)timeout->tv_sec;
        duration=seconds>(UINT64_MAX-999999)/1000000?UINT64_MAX:seconds*1000000+(uint64_t)timeout->tv_usec;
    }
    for(;;) {
        /* curl 7.64.1 retries EINTR forever when timeout_ms==0. Cancellation
         * must be a terminal socket error, not a spurious signal interruption. */
        if(st_cancelled()){errno=ECANCELED;return -1;}
        for(int i=0;i<used;i++)native[i].revents=0;
        stats.calls++;
        int result=used?sceNetInetPoll(native,(size_t)used,0):0;
        if(result<0){stats.errors++;stats.last_errno=(unsigned)sceNetInetGetErrno();errno=(int)stats.last_errno;return -1;}
        fd_set ready_read,ready_write,ready_except;FD_ZERO(&ready_read);FD_ZERO(&ready_write);FD_ZERO(&ready_except);
        int ready=0;
        for(int i=0;i<used;i++) {
            short revents=native[i].revents;int fd=descriptors[i];
            if(revents&SCE_NET_INET_POLLNVAL){stats.errors++;stats.last_errno=EBADF;errno=EBADF;return -1;}
            if(readfds&&FD_ISSET(fd,readfds)&&(revents&(SCE_NET_INET_POLLIN|SCE_NET_INET_POLLRDNORM|SCE_NET_INET_POLLHUP|SCE_NET_INET_POLLERR))){FD_SET(fd,&ready_read);ready++;}
            if(writefds&&FD_ISSET(fd,writefds)&&(revents&(SCE_NET_INET_POLLOUT|SCE_NET_INET_POLLWRNORM|SCE_NET_INET_POLLHUP|SCE_NET_INET_POLLERR))){FD_SET(fd,&ready_write);ready++;}
            if(exceptfds&&FD_ISSET(fd,exceptfds)&&(revents&(SCE_NET_INET_POLLPRI|SCE_NET_INET_POLLRDBAND))){FD_SET(fd,&ready_except);ready++;}
        }
        uint64_t elapsed=(uint64_t)sceKernelGetSystemTimeWide()-started;
        if(ready||(timeout&&elapsed>=duration)) {
            if(readfds)*readfds=ready_read;
            if(writefds)*writefds=ready_write;
            if(exceptfds)*exceptfds=ready_except;
            return ready;
        }
        /* No uninterruptible firmware wait: keep shutdown/cancel responsive. */
        unsigned pause=1000;
        if(timeout&&duration-elapsed<pause)pause=(unsigned)(duration-elapsed);
        sceKernelDelayThread(pause);
    }
}
