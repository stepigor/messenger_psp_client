#define _POSIX_C_SOURCE 200809L
#include "rtc_config.h"
#include "rtc_tls_symbols.h"
#include "rtc_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <mbedtls/md.h>
#include <zlib.h>
unsigned st_rtc_u16(const unsigned char *p){return (unsigned)p[0]*256+p[1];}
uint32_t st_rtc_u32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
void st_rtc_put16(unsigned char *p,unsigned n){p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n;}
void st_rtc_put32(unsigned char *p,uint32_t n){p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n;}
static int text(char *out,size_t cap,const char *in){size_t n=strlen(in);if(!n||n>=cap||strpbrk(in,"\r\n"))return 0;memcpy(out,in,n+1);return 1;}
int st_rtc_candidate(StRtcOffer *o,const char *candidate) {
    char foundation[80],transport[16],ip[64],kind[24];unsigned component,priority,port;struct in_addr a;
    if(!strncmp(candidate,"a=",2))candidate+=2;
    if(sscanf(candidate,"candidate:%79s %u %15s %u %63s %u typ %23s",foundation,&component,transport,&priority,ip,&port,kind)!=7||component!=1||strcasecmp(transport,"udp")||port<1||port>65535||inet_pton(AF_INET,ip,&a)!=1||!a.s_addr)return 0;
    for(unsigned i=0;i<o->candidate_count;i++)if(!strcmp(o->candidates[i].ip,ip)&&o->candidates[i].port==port)return 1;
    if(o->candidate_count>=ST_RTC_CANDIDATES)return 0;
    StRtcCandidate *c=o->candidates+o->candidate_count++;st_copy(c->ip,sizeof(c->ip),ip);c->port=port;c->priority=priority;return 1;
}
static int parse_sdp(const char *sdp,StRtcOffer *o,StError *err,int incoming) {
    memset(o,0,sizeof(*o));o->opus=o->red=-1;
    if(!sdp||strlen(sdp)>ST_RTC_SDP)return st_fail(err,"Voice offer exceeds memory limit");
    char *copy=malloc(strlen(sdp)+1);if(!copy)return st_fail(err,"Out of memory");strcpy(copy,sdp);
    int section=0;StRtcMedia *m=NULL;char *save=NULL;
    for(char *p=strtok_r(copy,"\n",&save);p;p=strtok_r(NULL,"\n",&save)) {
        size_t n=strlen(p);if(n&&p[n-1]=='\r')p[--n]=0;
        if(!strncmp(p,"m=",2)) {
            m=NULL;section=!strncmp(p,"m=audio ",8)?1:!strncmp(p,"m=application ",14)?2:3;
            unsigned port=0;sscanf(p,"m=%*s %u",&port);if(!port)section=3;
            if(section==1&&o->media_count<ST_RTC_STREAMS){m=o->media+o->media_count++;memset(m,0,sizeof(*m));m->opus=m->red=-1;o->audio=1;}
            if(section==2)o->data=1;
        }
        if(!strcmp(p,"a=ice-lite"))o->lite=1;
        if(!strncmp(p,"a=ice-ufrag:",12)&&!*o->ufrag)text(o->ufrag,sizeof(o->ufrag),p+12);
        else if(!strncmp(p,"a=ice-pwd:",10)&&!*o->password)text(o->password,sizeof(o->password),p+10);
        else if(!strncmp(p,"a=fingerprint:sha-256 ",22)&&!*o->fingerprint)text(o->fingerprint,sizeof(o->fingerprint),p+22);
        else if(!strncmp(p,"a=candidate:",12))st_rtc_candidate(o,p);
        else if(!strncmp(p,"a=mid:",6)){if(m)text(m->mid,sizeof(m->mid),p+6);else if(section==2)text(o->data_mid,sizeof(o->data_mid),p+6);}
        if(m&&(!strcmp(p,"a=inactive")||(incoming&&!strcmp(p,"a=recvonly")))){o->media_count--;m=NULL;}
        if(m) {
            int pt;char codec[32];if(sscanf(p,"a=rtpmap:%d %31s",&pt,codec)==2&&pt>=0&&pt<128){if(!strcmp(codec,"opus/48000/2"))m->opus=pt;else if(!strcmp(codec,"red/48000/2"))m->red=pt;}
            unsigned long source;if(!m->ssrc&&sscanf(p,"a=ssrc:%lu",&source)==1&&source<=UINT32_MAX)m->ssrc=(uint32_t)source;
            char stream[160],track[64];if(sscanf(p,"a=msid:%159s %63s",stream,track)==2)text(m->track,sizeof(m->track),track);
        }
    }
    free(copy);o->audio=o->media_count!=0;
    if(!o->ufrag[0]||!o->password[0]||strlen(o->fingerprint)!=95)return st_fail(err,"Voice offer has no authenticated ICE/DTLS parameters");
    for(unsigned i=0;i<o->media_count;i++)if(o->media[i].opus<0||!o->media[i].mid[0])return st_fail(err,"Voice server did not offer Opus");
    if(o->media_count){st_copy(o->mid,sizeof(o->mid),o->media[0].mid);o->opus=o->media[0].opus;o->red=o->media[0].red;}
    return 1;
}
int st_rtc_sdp(const char *sdp,StRtcOffer *o,StError *err){return parse_sdp(sdp,o,err,0);}
int st_rtc_sdp_incoming(const char *sdp,StRtcOffer *o,StError *err){return parse_sdp(sdp,o,err,1);}
static int append(char *out,size_t cap,const char *format,...) {
    size_t used=strlen(out);if(used>=cap)return 0;va_list ap;va_start(ap,format);int n=vsnprintf(out+used,cap-used,format,ap);va_end(ap);return n>=0&&(size_t)n<cap-used;
}
static int local_ice(char *out,size_t cap,const char *ufrag,const char *password,const char *fingerprint,const char *ip,unsigned port) {
    return append(out,cap,"a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 %s\r\na=setup:active\r\na=candidate:1 1 udp 2130706431 %s %u typ host\r\na=end-of-candidates\r\n",ufrag,password,fingerprint,ip,port);
}
int st_rtc_answer(const char *offer,const StRtcOffer *o,const char *ufrag,const char *password,const char *fingerprint,const char *ip,unsigned port,unsigned version,char *out,size_t cap) {
    if(!cap)return 0;out[0]=0;
    if(!append(out,cap,"v=0\r\no=- 1 %u IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE",version))return 0;
    if(o->data&&*o->data_mid&&!append(out,cap," %s",o->data_mid))return 0;
    for(unsigned i=0;i<o->media_count;i++)if(!append(out,cap," %s",o->media[i].mid))return 0;
    if(!append(out,cap,"\r\na=msid-semantic: WMS\r\n")||!local_ice(out,cap,ufrag,password,fingerprint,ip,port))return 0;
    char *copy=malloc(strlen(offer)+1);if(!copy)return 0;strcpy(copy,offer);char *save=NULL,kind[32]="",protocol[64]="",formats[512]="";int pending=0,ok=1;unsigned offered_port=0;
    for(char *p=strtok_r(copy,"\n",&save);p&&ok;p=strtok_r(NULL,"\n",&save)) {
        size_t n=strlen(p);if(n&&p[n-1]=='\r')p[--n]=0;
        if(!strncmp(p,"m=",2)) {
            int used=0;if(pending||sscanf(p,"m=%31s %u %63s %n",kind,&offered_port,protocol,&used)!=3||strlen(p+used)>=sizeof(formats)){ok=0;break;}
            strcpy(formats,p+used);pending=1;
        } else if(pending&&!strncmp(p,"a=mid:",6)) {
            const char *mid=p+6;const StRtcMedia *m=NULL;
            for(unsigned i=0;i<o->media_count;i++)if(!strcmp(mid,o->media[i].mid))m=o->media+i;
            if(offered_port&&!strcmp(kind,"audio")&&m) {
                ok=append(out,cap,"m=audio 9 UDP/TLS/RTP/SAVPF %d",m->opus);if(ok&&m->red>=0)ok=append(out,cap," %d",m->red);
                if(ok)ok=append(out,cap,"\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\na=recvonly\r\na=rtcp-mux\r\na=rtpmap:%d opus/48000/2\r\na=fmtp:%d minptime=10;useinbandfec=1\r\n",m->mid,m->opus,m->opus);
                if(ok&&m->red>=0)ok=append(out,cap,"a=rtpmap:%d red/48000/2\r\na=fmtp:%d %d/%d\r\n",m->red,m->red,m->opus,m->opus);
                if(ok)ok=local_ice(out,cap,ufrag,password,fingerprint,ip,port);
            } else if(offered_port&&!strcmp(kind,"application")&&o->data&&!strcmp(mid,o->data_mid)) {
                ok=append(out,cap,"m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\na=sctp-port:5000\r\na=max-message-size:65535\r\n",mid);
                if(ok)ok=local_ice(out,cap,ufrag,password,fingerprint,ip,port);
            } else ok=append(out,cap,"m=%s 0 %s %s\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\na=inactive\r\n",kind,protocol,formats,mid);
            pending=0;
        }
    }
    if(pending)ok=0;
    free(copy);return ok;
}
typedef struct {const unsigned char *p;size_t n;int bad;} Pb;
static uint64_t varint(Pb *b){uint64_t v=0;for(int i=0;i<10;i++){if(!b->n){b->bad=1;return 0;}unsigned c=*b->p++;b->n--;if(i==9&&(c&254)){b->bad=1;return 0;}v|=(uint64_t)(c&127)<<(i*7);if(!(c&128))return v;}b->bad=1;return 0;}
static int field(Pb *b,unsigned *id,uint64_t *number,Pb *child){if(!b->n||b->bad)return 0;uint64_t tag=varint(b);*id=(unsigned)(tag>>3);unsigned wire=(unsigned)tag&7;memset(child,0,sizeof(*child));*number=0;if(!*id){b->bad=1;return 0;}if(wire==0)*number=varint(b);else {uint64_t n=wire==2?varint(b):wire==1?8:wire==5?4:UINT64_MAX;if(n>b->n){b->bad=1;return 0;}child->p=b->p;child->n=(size_t)n;b->p+=n;b->n-=n;}return !b->bad;}
static void string(cJSON *o,const char *key,Pb p){if(!p.p||!p.n||p.n>ST_RTC_SDP||memchr(p.p,0,p.n))return;char *s=malloc(p.n+1);if(!s)return;memcpy(s,p.p,p.n);s[p.n]=0;cJSON_AddStringToObject(o,key,s);free(s);}
static cJSON *decode(Pb b,int type);
static void nested(cJSON *o,const char *key,int array,Pb p,int type,int *bad) {
    cJSON *child=p.p?decode(p,type):NULL;
    if(!child){*bad=1;return;}
    cJSON *parent=o;
    if(array){parent=cJSON_GetObjectItemCaseSensitive(o,key);if(!parent)parent=cJSON_AddArrayToObject(o,key);}
    int ok=array?cJSON_AddItemToArray(parent,child):cJSON_AddItemToObject(parent,key,child);
    if(!ok){cJSON_Delete(child);*bad=1;}
}
static cJSON *decode(Pb b,int type) {
    cJSON *o=cJSON_CreateObject();if(!o)return NULL;unsigned id;uint64_t v;Pb p;
    while(field(&b,&id,&v,&p)) {
        if(type==0){const char *key=id==1?"join":id==2?"answer":id==3?"offer":id==4?"trickle":id==5?"update":id==6?"trackPublished":id==8?"leave":NULL;if(key){int t=id==1?1:id==2?2:id==3?2:id==4?3:id==5?4:id==6?9:8;nested(o,key,0,p,t,&b.bad);}}
        else if(type==1){if(id==2)nested(o,"participant",0,p,5,&b.bad);else if(id==3)nested(o,"otherParticipants",1,p,5,&b.bad);else if(id==12)nested(o,"serverInfo",0,p,7,&b.bad);else if(id==10||id==11)cJSON_AddNumberToObject(o,id==10?"pingTimeout":"pingInterval",(double)v);}
        else if(type==2){if(id==1||id==2)string(o,id==1?"type":"sdp",p);else if(id==3)cJSON_AddNumberToObject(o,"id",(double)v);else if(id==4){cJSON *entry=decode(p,10),*map=cJSON_GetObjectItemCaseSensitive(o,"midToTrackId");if(!map)map=cJSON_AddObjectToObject(o,"midToTrackId");if(!entry||!*st_str(entry,"key")||!*st_str(entry,"value"))b.bad=1;else cJSON_AddStringToObject(map,st_str(entry,"key"),st_str(entry,"value"));cJSON_Delete(entry);}}
        else if(type==3){if(id==1)string(o,"candidateInit",p);else if(id==2)cJSON_AddNumberToObject(o,"target",(double)v);}
        else if(type==4&&id==1)nested(o,"participants",1,p,5,&b.bad);
        else if(type==5){if(id==1||id==2||id==5||id==9)string(o,id==1?"sid":id==2?"identity":id==5?"metadata":"name",p);else if(id==3)cJSON_AddNumberToObject(o,"state",(double)v);else if(id==4)nested(o,"tracks",1,p,6,&b.bad);}
        else if(type==10&&(id==1||id==2))string(o,id==1?"key":"value",p);
        else if(type==9){if(id==1)string(o,"cid",p);else if(id==2)nested(o,"track",0,p,6,&b.bad);}
        else if(type==7&&id==2)string(o,"version",p);
        else if(type==6){if(id==1||id==11)string(o,id==1?"sid":"mimeType",p);else if(id==4)cJSON_AddBoolToObject(o,"muted",v!=0);else if(id==2||id==9||id==16)cJSON_AddNumberToObject(o,id==2?"type":id==9?"source":"encryption",(double)v);}
    }
    if(b.bad){cJSON_Delete(o);return NULL;}return o;
}
cJSON *st_rtc_signal(const unsigned char *p,size_t n){if(n>65536)return NULL;return decode((Pb){p,n,0},0);}
static size_t attr(unsigned char *p,size_t at,unsigned type,const void *v,size_t n){st_rtc_put16(p+at,type);st_rtc_put16(p+at+2,(unsigned)n);memcpy(p+at+4,v,n);size_t padded=(n+3)&~3u;memset(p+at+4+n,0,padded-n);return at+4+padded;}
static size_t integrity(unsigned char *p,size_t at,const char *pwd) {
    st_rtc_put16(p+2,(unsigned)(at+24-20));unsigned char mac[20];
    if(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),(const unsigned char *)pwd,strlen(pwd),p,at,mac))return 0;
    at=attr(p,at,8,mac,20);st_rtc_put16(p+2,(unsigned)(at+8-20));uint32_t crc=(uint32_t)crc32(0,p,(uInt)at)^0x5354554e;unsigned char v[4];st_rtc_put32(v,crc);return attr(p,at,0x8028,v,4);
}
size_t st_rtc_stun_request(unsigned char p[256],const unsigned char id[12],const char *remote,const char *local,const char *pwd,int controlling,uint64_t tie) {
    char user[168];int n=snprintf(user,sizeof(user),"%s:%s",remote,local);if(n<=0||n>160)return 0;
    memset(p,0,256);st_rtc_put16(p,1);st_rtc_put32(p+4,0x2112a442);memcpy(p+8,id,12);size_t at=attr(p,20,6,user,(size_t)n);unsigned char v[8];st_rtc_put32(v,1845501695);at=attr(p,at,0x24,v,4);st_rtc_put32(v,(uint32_t)(tie>>32));st_rtc_put32(v+4,(uint32_t)tie);at=attr(p,at,controlling?0x802a:0x8029,v,8);if(controlling)at=attr(p,at,0x25,v,0);return integrity(p,at,pwd);
}
int st_rtc_stun_attr(const unsigned char *p,size_t n,unsigned wanted,const unsigned char **v,size_t *size) {
    if(n<20||st_rtc_u32(p+4)!=0x2112a442||st_rtc_u16(p+2)!=n-20)return 0;
    for(size_t at=20;at+4<=n;){size_t z=st_rtc_u16(p+at+2),pad=(z+3)&~3u;unsigned type=st_rtc_u16(p+at);if(pad>n-at-4)return 0;if(type==wanted){*v=p+at+4;*size=z;return 1;}if(type==8&&wanted!=0x8028)return 0;at+=4+pad;}return 0;
}
int st_rtc_stun_check(const unsigned char *p,size_t n,const char *pwd) {
    const unsigned char *v;size_t z;if(n>1600||!st_rtc_stun_attr(p,n,8,&v,&z)||z!=20)return 0;
    size_t at=(size_t)(v-p)-4;unsigned char copy[1600],mac[20];memcpy(copy,p,at);st_rtc_put16(copy+2,(unsigned)(at+4));
    if(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),(const unsigned char *)pwd,strlen(pwd),copy,at,mac))return 0;
    unsigned diff=0;for(int i=0;i<20;i++)diff|=mac[i]^v[i];return !diff;
}
size_t st_rtc_stun_reply(const unsigned char *request,size_t n,const char *pwd,uint32_t ip,unsigned port,unsigned char p[256]) {
    if(n<20||st_rtc_u16(request)!=1||!st_rtc_stun_check(request,n,pwd))return 0;
    memset(p,0,256);st_rtc_put16(p,0x101);st_rtc_put32(p+4,0x2112a442);memcpy(p+8,request+8,12);unsigned char v[8]={0,1};st_rtc_put16(v+2,port^0x2112);st_rtc_put32(v+4,ip^0x2112a442);size_t at=attr(p,20,0x20,v,8);return integrity(p,at,pwd);
}
