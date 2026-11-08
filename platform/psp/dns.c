/* curl's synchronous resolver hook, implemented with bounded nonblocking I/O.
 * The firmware sceNetResolverStartNtoA can outlive its timeout/retry arguments.
 * Query only the connected WLAN profile's DNS servers, never a public fallback.
 * RFC 1035 sections 4/7; RFC 5452 section 9: connected UDP peer, random ID/port,
 * matching question and owner, bounded compression/CNAME parsing. TLS still
 * authenticates the original URL hostname, independently of DNS answers. */
#include "stoat.h"
#include "psp_dns.h"
#include <pspnet_apctl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#ifndef ST_DNS_PORT
#define ST_DNS_PORT 53
#endif
#define DNS_BUDGET 5000u
#define DNS_PACKET 4096u
static StDnsStats stats;
StDnsStats st_psp_dns_stats(void){return stats;}
static void close_socket(int fd){close(fd);stats.closed++;}
static unsigned u16(const unsigned char *p){return (unsigned)p[0]*256+p[1];}
static void put16(unsigned char *p,unsigned n){p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n;}
static int again(void){return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;}
static int running(uint64_t deadline) {
    if(st_cancelled()){stats.result=ST_DNS_CANCELLED;return 0;}
    if(st_millis()>=deadline){stats.result=ST_DNS_TIMEOUT;return 0;}
    return 1;
}
static int normalize(char out[254],const char *name) {
    size_t n=strlen(name);if(n&&name[n-1]=='.')n--;
    if(!n||n>253)return 0;
    unsigned label=0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)name[i];
        if(c=='.'){if(!label)return 0;label=0;}
        else {if(c<=32||c>=127||++label>63)return 0;if(c>='A'&&c<='Z')c+=32;}
        out[i]=(char)c;
    }
    out[n]=0;return label!=0;
}
/* The caller's offset advances only over the original encoded name, including
 * its first compression pointer. Pointer loops cannot consume CPU indefinitely. */
static int name_at(const unsigned char *p,size_t n,size_t *offset,char out[254]) {
    size_t pos=*offset,used=0;int jumped=0;
    for(unsigned steps=0;steps<128;steps++) {
        if(pos>=n)return 0;
        unsigned len=p[pos++];
        if((len&0xc0)==0xc0) {
            if(pos>=n)return 0;
            size_t target=((len&63u)<<8)|p[pos++];
            if(target<12||target>=n)return 0;
            if(!jumped)*offset=pos;
            jumped=1;pos=target;continue;
        }
        if(len&0xc0)return 0;
        if(!len){if(!jumped)*offset=pos;out[used]=0;return 1;}
        if(pos+len>n||used+(used!=0)+len>253)return 0;
        if(used)out[used++]='.';
        for(unsigned i=0;i<len;i++) {
            unsigned char c=p[pos++];if(c<=32||c>=127||c=='.')return 0;
            out[used++]=(char)(c>='A'&&c<='Z'?c+32:c);
        }
    }
    return 0;
}
static size_t question(unsigned char *p,const char *name,unsigned id) {
    memset(p,0,12);put16(p,id);p[2]=1;p[5]=1;size_t pos=12;
    const char *label=name;
    for(;;) {
        const char *end=strchr(label,'.');size_t n=end?(size_t)(end-label):strlen(label);
        p[pos++]=(unsigned char)n;memcpy(p+pos,label,n);pos+=n;
        if(!end)break;
        label=end+1;
    }
    p[pos++]=0;put16(p+pos,1);put16(p+pos+2,1);return pos+4;
}
enum Answer { IGNORE,FOUND,ALIAS,TRUNCATED,NOT_FOUND,REPLY_ERROR };
static enum Answer answer(const unsigned char *p,size_t n,unsigned id,const char *query,struct in_addr *address,char alias[254]) {
    if(n<12||u16(p)!=id||(u16(p+2)&0xf800)!=0x8000||u16(p+4)!=1)return IGNORE;
    size_t pos=12;char owner[254];
    if(!name_at(p,n,&pos,owner)||strcmp(owner,query)||pos+4>n||u16(p+pos)!=1||u16(p+pos+2)!=1)return IGNORE;
    pos+=4;unsigned flags=u16(p+2);
    if((flags&15)==3)return NOT_FOUND;
    if(flags&15)return REPLY_ERROR;
    if(flags&0x200)return TRUNCATED;
    size_t start=pos;char current[254];st_copy(current,sizeof(current),query);
    /* Some recursive servers return CNAME and A out of order. Only follow
     * records owned by the requested name or its explicit CNAME chain. */
    for(unsigned chain=0;chain<8;chain++) {
        pos=start;int renamed=0;
        for(unsigned i=0;i<u16(p+6);i++) {
            if(!name_at(p,n,&pos,owner)||pos+10>n)return IGNORE;
            unsigned type=u16(p+pos),klass=u16(p+pos+2),len=u16(p+pos+8);pos+=10;
            if(pos+len>n)return IGNORE;
            if(klass==1&&!strcmp(owner,current)) {
                if(type==1&&len==4){memcpy(address,p+pos,4);return FOUND;}
                if(type==5) {
                    size_t end=pos;char next[254];
                    if(!name_at(p,n,&end,next)||end!=pos+len||!next[0]||!strcmp(current,next))return IGNORE;
                    st_copy(current,sizeof(current),next);renamed=1;break;
                }
            }
            pos+=len;
        }
        if(!renamed) {
            if(strcmp(current,query)){st_copy(alias,254,current);return ALIAS;}
            return NOT_FOUND;
        }
    }
    return IGNORE;
}
static int open_socket(const struct in_addr *server,int type,uint64_t deadline) {
    if(!running(deadline))return -1;
    int fd=socket(AF_INET,type,0);if(fd<0){stats.result=ST_DNS_NETWORK;return -1;}
    stats.opened++;
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)goto failed;
    struct sockaddr_in local;memset(&local,0,sizeof(local));local.sin_family=AF_INET;
#if defined(__PSP__) || defined(__APPLE__)
    local.sin_len=sizeof(local);
#endif
    for(int i=0;i<4;i++) {
        uint16_t port;if(!st_random(&port,sizeof(port)))goto failed;
        local.sin_port=htons((uint16_t)(49152+(port&16383)));
        if(!bind(fd,(const struct sockaddr *)&local,sizeof(local)))break;
        if(errno!=EADDRINUSE||i==3)goto failed;
    }
    struct sockaddr_in peer;memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;
#if defined(__PSP__) || defined(__APPLE__)
    peer.sin_len=sizeof(peer);
#endif
    peer.sin_port=htons(ST_DNS_PORT);peer.sin_addr=*server;
    if(!connect(fd,(const struct sockaddr *)&peer,sizeof(peer)))return fd;
    if(type!=SOCK_STREAM||(errno!=EINPROGRESS&&errno!=EALREADY&&errno!=EWOULDBLOCK)||fd>=FD_SETSIZE)goto failed;
    while(running(deadline)) {
        fd_set writes;FD_ZERO(&writes);FD_SET(fd,&writes);struct timeval wait={0};
        int ready=select(fd+1,NULL,&writes,NULL,&wait);
        if(ready<0){if(errno==EINTR){st_pause();continue;}goto failed;}
        if(ready) {
            int error=0;socklen_t size=sizeof(error);
            if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0||error)goto failed;
            return fd;
        }
        st_pause();
    }
    close_socket(fd);return -1;
failed:
    close_socket(fd);stats.result=ST_DNS_NETWORK;return -1;
}
static int stream_io(int fd,unsigned char *p,size_t n,int writing,uint64_t deadline) {
    size_t pos=0;
    while(pos<n&&running(deadline)) {
        int count=(int)(writing?send(fd,p+pos,n-pos,MSG_DONTWAIT):recv(fd,p+pos,n-pos,MSG_DONTWAIT));
        if(count>0)pos+=(size_t)count;
        else if(count<0&&again())st_pause();
        else {stats.result=ST_DNS_NETWORK;return 0;}
    }
    return pos==n&&running(deadline);
}
static enum Answer tcp_query(const struct in_addr *server,unsigned char *query,size_t len,unsigned id,const char *name,
                              struct in_addr *address,char alias[254],uint64_t deadline) {
    stats.tcp++;int fd=open_socket(server,SOCK_STREAM,deadline);if(fd<0)return IGNORE;
    unsigned char prefix[2],packet[DNS_PACKET];put16(prefix,(unsigned)len);enum Answer result=IGNORE;
    if(stream_io(fd,prefix,2,1,deadline)&&stream_io(fd,query,len,1,deadline)&&stream_io(fd,prefix,2,0,deadline)) {
        size_t size=u16(prefix);
        if(size>=12&&size<=sizeof(packet)&&stream_io(fd,packet,size,0,deadline)) {
            stats.replies++;result=answer(packet,size,id,name,address,alias);
        }
    }
    close_socket(fd);return result;
}
static enum Answer udp_query(const struct in_addr *server,const char *name,struct in_addr *address,char alias[254],uint64_t deadline) {
    unsigned char query[272],packet[DNS_PACKET];uint16_t id;
    stats.result=ST_DNS_REPLY_ERROR;
    if(!st_random(&id,sizeof(id))){stats.result=ST_DNS_NETWORK;return IGNORE;}
    size_t len=question(query,name,id);int fd=open_socket(server,SOCK_DGRAM,deadline);if(fd<0)return IGNORE;
    stats.queries++;int sent=0;enum Answer result=IGNORE;
    while(running(deadline)) {
        if(!sent) {
            int count=(int)send(fd,query,len,MSG_DONTWAIT);
            if(count==(int)len)sent=1;
            else if(count<0&&again()){st_pause();continue;}
            else {stats.result=ST_DNS_NETWORK;break;}
        }
        int count=(int)recv(fd,packet,sizeof(packet),MSG_DONTWAIT);
        if(count>0) {
            stats.replies++;result=answer(packet,(size_t)count,id,name,address,alias);
            if(result!=IGNORE)break;
        }else if(count<0&&!again()){stats.result=ST_DNS_NETWORK;break;}
        st_pause();
    }
    close_socket(fd);
    if(result==TRUNCATED)return tcp_query(server,query,len,id,name,address,alias,deadline);
    return result;
}
static int resolve(const char *name,struct in_addr *address,uint64_t deadline) {
    struct in_addr servers[2];int count=0;
    for(int i=0;i<2;i++) {
        union SceNetApctlInfo info;memset(&info,0,sizeof(info));struct in_addr ip;
        if(sceNetApctlGetInfo(i?PSP_NET_APCTL_INFO_SECDNS:PSP_NET_APCTL_INFO_PRIMDNS,&info)<0)continue;
        char *text=i?info.secondaryDns:info.primaryDns;text[15]=0;
        if(inet_aton(text,&ip)&&ip.s_addr&&ip.s_addr!=htonl(UINT32_MAX)&&(!count||ip.s_addr!=servers[0].s_addr))servers[count++]=ip;
    }
    if(!count){stats.result=ST_DNS_NO_SERVER;return 0;}
    char current[254];st_copy(current,sizeof(current),name);
    for(unsigned chain=0;chain<8&&running(deadline);chain++) {
        int renamed=0;
        for(int attempt=0;attempt<3&&running(deadline);attempt++) {
            uint64_t until=st_millis()+1600;if(until>deadline)until=deadline;
            char alias[254];enum Answer result=udp_query(servers+attempt%count,current,address,alias,until);
            if(!running(deadline))return 0;
            if(result==FOUND){stats.result=ST_DNS_OK;return 1;}
            if(result==NOT_FOUND){stats.result=ST_DNS_NOT_FOUND;return 0;}
            if(result==ALIAS){st_copy(current,sizeof(current),alias);renamed=1;break;}
            if(result==REPLY_ERROR)stats.result=ST_DNS_REPLY_ERROR;
        }
        if(!renamed)return 0;
    }
    if(running(deadline))stats.result=ST_DNS_REPLY_ERROR;
    return 0;
}
struct hostent *gethostbyname(const char *name) {
    static struct hostent result;static struct in_addr address;
    static char hostname[254],*addresses[2];
    memset(&stats,0,sizeof(stats));uint64_t start=st_millis();st_stage("DNS");
    if(!running(start+DNS_BUDGET))return NULL;
    if(!name||!normalize(hostname,name)){stats.result=ST_DNS_INVALID;return NULL;}
    int ok=inet_aton(hostname,&address)||resolve(hostname,&address,start+DNS_BUDGET);
    stats.elapsed_ms=(unsigned)(st_millis()-start);
    if(!ok||st_cancelled()){if(st_cancelled())stats.result=ST_DNS_CANCELLED;return NULL;}
    addresses[0]=(char *)&address;addresses[1]=NULL;memset(&result,0,sizeof(result));
    result.h_name=hostname;result.h_addrtype=AF_INET;result.h_length=sizeof(address);result.h_addr_list=addresses;return &result;
}
