#include "rtc.h"
#include "rtc_tls.h"
#include "voice_codec.h"
#include <srtp.h>
#include <opus/opus.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef __PSP__
#include "libcglue_fd.h"
#include <pspnet_inet.h>
#endif
#define SIGNAL_MAX 65536u
#define PCM_COUNT 8192u
typedef struct {unsigned char bytes[1500];unsigned length;uint16_t sequence;int full;} RtpSlot;
typedef struct {
    char track[64];uint32_t ssrc,base_seq,highest,received,last_sr;uint64_t sr_at;unsigned member;int opus,red;
    OpusDecoder *decoder;RtpSlot slots[64];uint16_t expected;int started;
    uint64_t play_at,last_rtp,speaking_until;int16_t samples[4096];unsigned at,count;
} VoiceStream;
struct StRtc {
    StRtcStats stats;StError error;struct StRtc *publisher;int is_publisher,publish_requested,publish_offered,server_muted;char published_sid[64];uint16_t send_sequence;uint32_t send_timestamp;uint64_t mix_at,last_send;VoiceStream *streams[ST_RTC_STREAMS];
    StRtcTls *tls,*dtls;int tcp,udp,tcp_ready,upgraded;
    char host[256],authority[256],path[768],user[64],ufrag[32],pwd[64],accept[29];
    char local_ip[16];unsigned local_port;StRtcOffer offer;unsigned candidate,offer_version;
    uint64_t deadline,last_receive,last_ping,ice_at,ice_started,report_at,consent_at;
    unsigned char transaction[12];uint64_t tie;int controlling,nominated,protected,dtls_ready;
    unsigned char *tx,*input,*message;size_t tx_size,tx_at,tx_chunk,input_size,message_size;int assembling;
    unsigned char datagram[1600] __attribute__((aligned(16)));size_t datagram_size;
    srtp_t inbound,outbound;uint32_t local_ssrc,sent_packets,sent_octets;
    int16_t *pcm;volatile uint32_t read_at,write_at;
};
static int fail(StRtc *r,const char *message){st_copy(r->stats.error,sizeof(r->stats.error),message);r->stats.state=ST_RTC_FAILED;return 0;}
static int again(void){return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR||errno==EINPROGRESS||errno==EALREADY;}
/* Voice cancellation is independent of the serial HTTP worker's select hook. */
static int tcp_ready(int fd) {
#ifdef __PSP__
    if(fd<0||fd>=FD_SETSIZE||!__descriptormap[fd]||__descriptormap[fd]->type!=ST_LIBC_SOCKET){errno=EBADF;return -1;}
    struct SceNetInetPollfd p={(int)__descriptormap[fd]->descriptor,SCE_NET_INET_POLLOUT,0};
    return sceNetInetPoll(&p,1,0);
#else
    fd_set fds;FD_ZERO(&fds);FD_SET(fd,&fds);struct timeval tv={0,0};return select(fd+1,NULL,&fds,NULL,&tv);
#endif
}
static int tcp_send(void *ctx,const unsigned char *p,size_t n){StRtc *r=ctx;int z=(int)send(r->tcp,p,n,MSG_DONTWAIT);return z<0&&again()?-0x6880:z<0?-0x004e:z;}
static int tcp_recv(void *ctx,unsigned char *p,size_t n){StRtc *r=ctx;int z=(int)recv(r->tcp,p,n,MSG_DONTWAIT);return z<0&&again()?-0x6900:z<0?-0x004c:z;}
static int udp_send(void *ctx,const unsigned char *p,size_t n){StRtc *r=ctx;int z=(int)send(r->udp,p,n,MSG_DONTWAIT);return z<0&&again()?-0x6880:z<0?-0x004e:z;}
static int udp_recv(void *ctx,unsigned char *p,size_t n){StRtc *r=ctx;if(!r->datagram_size)return -0x6900;if(r->datagram_size>n)return -0x7100;size_t z=r->datagram_size;memcpy(p,r->datagram,z);r->datagram_size=0;return (int)z;}
static int random_text(char *out,size_t n){static const char hex[]="0123456789abcdef";unsigned char b[64];if(n>sizeof(b)*2||!st_random(b,(n+1)/2))return 0;for(size_t i=0;i<n;i++)out[i]=hex[(b[i/2]>>(i%2?0:4))&15];out[n]=0;st_wipe(b,sizeof(b));return 1;}
static int queue_bytes(StRtc *r,const void *p,size_t n) {
    size_t left=r->tx_size-r->tx_at;if(n>SIGNAL_MAX-left)return fail(r,"Voice send queue limit");
    if(r->tx_size+n>SIGNAL_MAX){if(r->tx_chunk)return fail(r,"Voice send queue blocked");memmove(r->tx,r->tx+r->tx_at,left);r->tx_at=0;r->tx_size=left;}
    memcpy(r->tx+r->tx_size,p,n);r->tx_size+=n;return 1;
}
static int frame(StRtc *r,int opcode,const void *p,size_t n){unsigned char *data=NULL;size_t size;if(!st_ws_masked(opcode,p,n,&data,&size))return fail(r,"Cannot frame voice signaling");int ok=queue_bytes(r,data,size);st_wipe(data,size);free(data);return ok;}
static int signal_json(StRtc *r,cJSON *o){char *s=cJSON_PrintUnformatted(o);cJSON_Delete(o);if(!s)return fail(r,"Not enough memory for voice signaling");
int ok=frame(r,1,s,strlen(s));st_wipe(s,strlen(s));free(s);return ok;}
static int ping(StRtc *r){cJSON *o=cJSON_CreateObject();if(!o)return fail(r,"Out of memory");cJSON_AddNumberToObject(o,"ping",(double)time(NULL)*1000);r->last_ping=st_millis();return signal_json(r,o);}
static int member_index(StRtc *r,const char *user) {
    for(unsigned i=0;i<r->stats.member_count;i++)if(!strcmp(r->stats.members[i].user,user))return (int)i;
    if(!*user||strlen(user)>=64||r->stats.member_count>=ST_RTC_MEMBERS)return -1;
    unsigned i=r->stats.member_count++;StRtcMember *m=r->stats.members+i;st_copy(m->user,sizeof(m->user),user);m->volume=100;return (int)i;
}
static void stream_free(VoiceStream *s){if(s){opus_decoder_destroy(s->decoder);st_wipe(s,sizeof(*s));free(s);}}
static int subscription(StRtc *r,const cJSON *participant) {
    int index=member_index(r,st_str(participant,"identity"));if(index<0)return 1;
    StRtcMember *member=r->stats.members+index;
    const cJSON *state=cJSON_GetObjectItemCaseSensitive(participant,"state");
    member->connected=!(cJSON_IsNumber(state)&&state->valueint==3)&&strcmp(st_str(participant,"state"),"DISCONNECTED");
    st_copy(member->sid,sizeof(member->sid),st_str(participant,"sid"));st_copy(member->name,sizeof(member->name),st_str(participant,"name"));
    cJSON *metadata=NULL;if(strlen(st_str(participant,"metadata"))<=8192)metadata=cJSON_Parse(st_str(participant,"metadata"));
    if(metadata){const char *name=st_str(metadata,"display_name");if(!*name)name=st_str(metadata,"username");if(*name)st_copy(member->name,sizeof(member->name),name);st_copy(member->avatar,sizeof(member->avatar),st_str(cJSON_GetObjectItemCaseSensitive(metadata,"avatar"),"_id"));cJSON_Delete(metadata);}
    if(!*member->name)st_copy(member->name,sizeof(member->name),member->user);
    if(!member->connected){member->speaking=0;member->subscribed=0;for(unsigned i=0;i<ST_RTC_STREAMS;i++)if(r->streams[i]&&r->streams[i]->member==(unsigned)index){stream_free(r->streams[i]);r->streams[i]=NULL;}member->track[0]=0;return 1;}
    const cJSON *tracks=cJSON_GetObjectItemCaseSensitive(participant,"tracks"),*t;
    cJSON_ArrayForEach(t,tracks) {
        const cJSON *type=cJSON_GetObjectItemCaseSensitive(t,"type"),*source=cJSON_GetObjectItemCaseSensitive(t,"source");
        int audio=(cJSON_IsNumber(type)&&type->valueint==0)||!strcmp(st_str(t,"type"),"AUDIO")||!type;
        int mic=(cJSON_IsNumber(source)&&source->valueint==2)||!strcmp(st_str(t,"source"),"MICROPHONE");
        if(!audio||!mic||!*st_str(t,"sid")||strlen(st_str(t,"sid"))>=64)continue;
        member->muted=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t,"muted"));
        if(!strcmp(member->track,st_str(t,"sid")))return 1;
        if(!strcmp(member->user,r->stats.self)){st_copy(member->track,sizeof(member->track),st_str(t,"sid"));return 1;}
        const cJSON *encryption=cJSON_GetObjectItemCaseSensitive(t,"encryption");
        if(encryption&&((cJSON_IsNumber(encryption)&&encryption->valueint)||(!cJSON_IsNumber(encryption)&&strcmp(st_str(t,"encryption"),"NONE"))))continue;
        unsigned subscribed=0;for(unsigned i=0;i<r->stats.member_count;i++)if(i!=(unsigned)index)subscribed+=r->stats.members[i].subscribed!=0;
        if(subscribed>=ST_RTC_STREAMS)return 1;
        st_copy(member->track,sizeof(member->track),st_str(t,"sid"));member->subscribed=1;
        cJSON *o=cJSON_CreateObject(),*sub=cJSON_AddObjectToObject(o,"subscription"),*a=cJSON_AddArrayToObject(sub,"trackSids");cJSON_AddItemToArray(a,cJSON_CreateString(member->track));cJSON_AddBoolToObject(sub,"subscribe",1);return signal_json(r,o);
    }
    member->muted=strcmp(member->user,r->stats.self)!=0;
    if(member->track[0]){member->track[0]=0;member->subscribed=0;member->speaking=0;for(unsigned i=0;i<ST_RTC_STREAMS;i++)if(r->streams[i]&&r->streams[i]->member==(unsigned)index){stream_free(r->streams[i]);r->streams[i]=NULL;}}
    return 1;
}
static int publisher_offer(StRtc *r) {
    StRtc *p=r->publisher;char sdp[2500];
    int n=snprintf(sdp,sizeof(sdp),"v=0\r\no=- 2 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\na=msid-semantic: WMS psp\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10;useinbandfec=1\r\na=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 %s\r\na=setup:actpass\r\na=msid:psp psp-microphone\r\na=ssrc:%lu cname:psp\r\na=ssrc:%lu msid:psp psp-microphone\r\na=candidate:1 1 udp 2130706431 %s %u typ host\r\na=end-of-candidates\r\n",p->ufrag,p->pwd,st_rtc_tls_fingerprint(p->dtls),(unsigned long)p->local_ssrc,(unsigned long)p->local_ssrc,p->local_ip,p->local_port);
    if(n<0||(size_t)n>=sizeof(sdp))return fail(r,"Publisher offer limit");
    cJSON *o=cJSON_CreateObject(),*offer=cJSON_AddObjectToObject(o,"offer");cJSON_AddStringToObject(offer,"type","offer");cJSON_AddStringToObject(offer,"sdp",sdp);r->publish_offered=1;return signal_json(r,o);
}
static int publish_request(StRtc *r) {
    cJSON *o=cJSON_CreateObject(),*a=cJSON_AddObjectToObject(o,"addTrack");cJSON_AddStringToObject(a,"cid","psp-microphone");cJSON_AddStringToObject(a,"name","PSP microphone");cJSON_AddStringToObject(a,"type","AUDIO");cJSON_AddStringToObject(a,"source","MICROPHONE");cJSON_AddBoolToObject(a,"disableRed",1);cJSON_AddBoolToObject(a,"muted",r->stats.mic_muted||r->stats.deafen);r->publisher->deadline=st_millis()+40000;r->publish_requested=1;return signal_json(r,o);
}
static void map_streams(StRtc *r,const cJSON *description) {
    const cJSON *map=cJSON_GetObjectItemCaseSensitive(description,"midToTrackId");
    if(!map)map=cJSON_GetObjectItemCaseSensitive(description,"midToTrackID");
    for(unsigned slot=0;slot<ST_RTC_STREAMS;slot++)if(r->streams[slot]) {
        int found=0;for(unsigned i=0;i<r->offer.media_count;i++){StRtcMedia *m=r->offer.media+i;const char *track=st_str(map,m->mid);if(!*track)track=m->track;if(!strcmp(track,r->streams[slot]->track))found=1;}
        if(!found){stream_free(r->streams[slot]);r->streams[slot]=NULL;}
    }
    for(unsigned i=0;i<r->offer.media_count;i++) {
        StRtcMedia *m=r->offer.media+i;const char *track=st_str(map,m->mid);if(*track)st_copy(m->track,sizeof(m->track),track);else track=m->track;
        int member=-1;for(unsigned k=0;k<r->stats.member_count;k++)if(!strcmp(r->stats.members[k].track,track)&&r->stats.members[k].subscribed)member=(int)k;
        if(member<0)continue;VoiceStream *stream=NULL;
        for(unsigned k=0;k<ST_RTC_STREAMS;k++)if(r->streams[k]&&!strcmp(r->streams[k]->track,track))stream=r->streams[k];
        if(!stream)for(unsigned k=0;k<ST_RTC_STREAMS;k++)if(!r->streams[k]){int error;stream=calloc(1,sizeof(*stream));if(!stream)break;stream->decoder=opus_decoder_create(ST_VOICE_RATE,1,&error);if(!stream->decoder){free(stream);stream=NULL;break;}r->streams[k]=stream;st_copy(stream->track,sizeof(stream->track),track);break;}
        if(stream){if(stream->ssrc!=m->ssrc){stream->started=0;stream->at=stream->count=0;memset(stream->slots,0,sizeof(stream->slots));opus_decoder_ctl(stream->decoder,OPUS_RESET_STATE);}stream->ssrc=m->ssrc;stream->opus=m->opus;stream->red=m->red;stream->member=(unsigned)member;}
    }
}
static int media_offer(StRtc *r,const cJSON *description) {
    const char *sdp=st_str(description,"sdp");StRtcOffer offer;
    if(!st_rtc_sdp_incoming(sdp,&offer,&r->error))return fail(r,r->error.text);
    if(r->offer.audio&&strcmp(r->offer.ufrag,offer.ufrag))return fail(r,"Voice server requested reconnect. Reconnect manually.");
    if(r->dtls_ready&&strcmp(r->offer.fingerprint,offer.fingerprint))return fail(r,"Voice identity changed. Reconnect manually.");
    if(r->offer.audio&&!strcmp(r->offer.ufrag,offer.ufrag)) {
        StRtcOffer merged=offer;merged.candidate_count=r->offer.candidate_count;
        memcpy(merged.candidates,r->offer.candidates,sizeof(merged.candidates));
        for(unsigned i=0;i<offer.candidate_count;i++) {
            char line[100];snprintf(line,sizeof(line),"candidate:1 1 udp %u %s %u typ host",offer.candidates[i].priority,offer.candidates[i].ip,offer.candidates[i].port);st_rtc_candidate(&merged,line);
        }
        offer=merged;
    }
    r->offer=offer;r->controlling=offer.lite;map_streams(r,description);
    if(!st_rtc_tls_peer(r->dtls,offer.fingerprint))return fail(r,"Invalid voice certificate fingerprint");
    char *answer=malloc(ST_RTC_SDP);if(!answer)return fail(r,"Not enough memory for voice offer");
    int ok=st_rtc_answer(sdp,&offer,r->ufrag,r->pwd,st_rtc_tls_fingerprint(r->dtls),r->local_ip,r->local_port,++r->offer_version,answer,ST_RTC_SDP);
    if(!ok){free(answer);return fail(r,"Voice offer could not be answered");}
    cJSON *o=cJSON_CreateObject(),*a=cJSON_AddObjectToObject(o,"answer");cJSON_AddStringToObject(a,"type","answer");cJSON_AddStringToObject(a,"sdp",answer);
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(description,"id");if(cJSON_IsNumber(id))cJSON_AddNumberToObject(a,"id",id->valuedouble);
    free(answer);if((offer.audio||offer.data)&&!r->dtls_ready){r->stats.state=ST_RTC_ICE;r->ice_started=st_millis();r->deadline=st_millis()+30000;}
    return signal_json(r,o);
}
static int event(StRtc *r,const cJSON *o) {
    r->stats.signals++;const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"join");
    if(v) {
        const cJSON *self=cJSON_GetObjectItemCaseSensitive(v,"participant");st_copy(r->stats.self,sizeof(r->stats.self),st_str(self,"identity"));if(!subscription(r,self))return 0;
        const cJSON *a=cJSON_GetObjectItemCaseSensitive(v,"otherParticipants"),*p;cJSON_ArrayForEach(p,a)if(!subscription(r,p))return 0;
        const cJSON *info=cJSON_GetObjectItemCaseSensitive(v,"serverInfo");st_copy(r->stats.server,sizeof(r->stats.server),st_str(info,"version"));
        if(!r->publish_requested&&!publish_request(r))return 0;
    }
    v=cJSON_GetObjectItemCaseSensitive(o,"update");if(v){const cJSON *a=cJSON_GetObjectItemCaseSensitive(v,"participants"),*p;cJSON_ArrayForEach(p,a)if(!subscription(r,p))return 0;map_streams(r,NULL);}
    v=cJSON_GetObjectItemCaseSensitive(o,"offer");if(v&&!media_offer(r,v))return 0;
    v=cJSON_GetObjectItemCaseSensitive(o,"trackPublished");if(v&&!strcmp(st_str(v,"cid"),"psp-microphone")){st_copy(r->published_sid,sizeof(r->published_sid),st_str(cJSON_GetObjectItemCaseSensitive(v,"track"),"sid"));if(!r->publish_offered&&!publisher_offer(r))return 0;}
    v=cJSON_GetObjectItemCaseSensitive(o,"answer");if(v) {
        StRtc *p=r->publisher;if(!st_rtc_sdp(st_str(v,"sdp"),&p->offer,&r->error))return fail(r,r->error.text);
        if(!strstr(st_str(v,"sdp"),"a=setup:active"))return fail(r,"Unsupported publisher DTLS role");
        if(!st_rtc_tls_peer(p->dtls,p->offer.fingerprint))return fail(r,"Invalid publisher certificate");p->ice_started=st_millis();p->controlling=1;
    }
    v=cJSON_GetObjectItemCaseSensitive(o,"trickle");
    if(v){const cJSON *target=cJSON_GetObjectItemCaseSensitive(v,"target");int subscriber=(cJSON_IsNumber(target)&&target->valueint==1)||!strcmp(st_str(v,"target"),"SUBSCRIBER");cJSON *candidate=cJSON_Parse(st_str(v,"candidateInit"));if(candidate){st_rtc_candidate(subscriber?&r->offer:&r->publisher->offer,st_str(candidate,"candidate"));cJSON_Delete(candidate);}}
    v=cJSON_GetObjectItemCaseSensitive(o,"mute");if(v&&!strcmp(st_str(v,"sid"),r->published_sid))r->server_muted=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"muted"));
    if(cJSON_GetObjectItemCaseSensitive(o,"leave"))return fail(r,"Voice server closed the connection");return 1;
}
void st_rtc_controls(StRtc *r,int mic_muted,int deafen) {
    int changed=r->stats.mic_muted!=mic_muted||r->stats.deafen!=deafen;r->stats.mic_muted=mic_muted;r->stats.deafen=deafen;
    if(changed&&*r->published_sid){cJSON *o=cJSON_CreateObject(),*m=cJSON_AddObjectToObject(o,"mute");cJSON_AddStringToObject(m,"sid",r->published_sid);cJSON_AddBoolToObject(m,"muted",mic_muted||deafen);signal_json(r,o);}
}
void st_rtc_volume(StRtc *r,const char *user,int volume){if(volume<0)volume=0;if(volume>200)volume=200;for(unsigned i=0;i<r->stats.member_count;i++)if(!strcmp(r->stats.members[i].user,user))r->stats.members[i].volume=volume;}
int st_rtc_send_opus(StRtc *r,const unsigned char *packet,size_t length) {
    StRtc *p=r->publisher;if(!p||!p->dtls_ready||r->stats.mic_muted||r->stats.deafen||r->server_muted||!length||length>1275)return 0;
    uint64_t now=st_millis();if(p->last_send&&now-p->last_send>100)p->send_timestamp+=(uint32_t)((now-p->last_send-20)*48);p->last_send=now;
    unsigned char data[1500] __attribute__((aligned(16)));data[0]=0x80;data[1]=111;st_rtc_put16(data+2,p->send_sequence++);st_rtc_put32(data+4,p->send_timestamp);p->send_timestamp+=960;st_rtc_put32(data+8,p->local_ssrc);memcpy(data+12,packet,length);int n=(int)length+12;
    if(srtp_protect(p->outbound,data,&n)!=srtp_err_status_ok)return 0;
    if(send(p->udp,data,(size_t)n,MSG_DONTWAIT)!=n){r->stats.mic_dropped++;return 0;}r->stats.mic_packets++;p->sent_packets++;p->sent_octets+=(uint32_t)length;return 1;
}
static int header_value(const char *headers,const char *name,const char *value) {
    const char *line=strstr(headers,"\r\n");size_t n=strlen(name);
    while(line&&line[2]) {
        line+=2;const char *end=strstr(line,"\r\n");if(!end)break;
        if((size_t)(end-line)>n&&!strncasecmp(line,name,n)&&line[n]==':') {
            const char *v=line+n+1;while(v<end&&(*v==' '||*v=='\t'))v++;
            while(end>v&&(end[-1]==' '||end[-1]=='\t'))end--;
            return (size_t)(end-v)==strlen(value)&&(!strcasecmp(name,"Sec-WebSocket-Accept")?!memcmp(v,value,(size_t)(end-v)):!strncasecmp(v,value,(size_t)(end-v)));
        }
        line=end;
    }
    return 0;
}
static int incoming(StRtc *r) {
    for(unsigned messages=0;messages<4&&r->input_size;messages++) {
        if(!r->upgraded) {
            unsigned char *end=NULL;for(size_t i=3;i<r->input_size;i++)if(!memcmp(r->input+i-3,"\r\n\r\n",4)){end=r->input+i+1;break;}
            if(!end){if(r->input_size>8192)return fail(r,"Voice upgrade header limit");return 1;}
            size_t size=(size_t)(end-r->input);char header[8193];memcpy(header,r->input,size);header[size]=0;
            if(strncmp(header,"HTTP/1.1 101 ",13)||!header_value(header,"Sec-WebSocket-Accept",r->accept)||!header_value(header,"Upgrade","websocket")||!header_value(header,"Connection","Upgrade"))return fail(r,"Voice WebSocket upgrade rejected");
            memmove(r->input,r->input+size,r->input_size-size);r->input_size-=size;r->upgraded=1;r->stats.state=ST_RTC_SIGNAL;r->last_receive=st_millis();if(!ping(r))return 0;continue;
        }
        size_t h,n;int opcode,fin;int parsed=st_ws_frame(r->input,r->input_size,&h,&n,&opcode,&fin);if(parsed<0)return fail(r,"Invalid voice WebSocket frame");if(!parsed)return 1;if(n>SIGNAL_MAX)return fail(r,"Voice frame size limit");
        unsigned char *p=r->input+h;r->last_receive=st_millis();
        if(opcode==8)return fail(r,"Voice connection closed");
        if(opcode==9){if(!frame(r,10,p,n))return 0;}
        else if(opcode!=10) {
            if((opcode==0&&!r->assembling)||(opcode!=0&&r->assembling)||n>SIGNAL_MAX-r->message_size)return fail(r,"Invalid fragmented voice message");
            unsigned char *joined=realloc(r->message,r->message_size+n+1);if(!joined)return fail(r,"Voice signaling memory limit");r->message=joined;memcpy(joined+r->message_size,p,n);r->message_size+=n;joined[r->message_size]=0;
            if(fin){int type=r->assembling?r->assembling:opcode;cJSON *o=type==1?cJSON_ParseWithLengthOpts((char *)joined,r->message_size+1,NULL,1):st_rtc_signal(joined,r->message_size);st_wipe(joined,r->message_size);free(joined);r->message=NULL;r->message_size=0;r->assembling=0;if(!o)return fail(r,"Invalid voice signal");int ok=event(r,o);cJSON_Delete(o);if(!ok)return 0;}else if(!r->assembling)r->assembling=opcode;
        }
        memmove(r->input,r->input+h+n,r->input_size-h-n);r->input_size-=h+n;
    }
    return 1;
}
static void fifo_write(StRtc *r,const int16_t *p,size_t n) {
    uint32_t w=r->write_at,used=w-r->read_at;if(n>PCM_COUNT-used){r->stats.pcm_dropped++;return;}
    for(size_t i=0;i<n;i++)r->pcm[(w+(uint32_t)i)&(PCM_COUNT-1)]=p[i];__sync_synchronize();r->write_at=w+(uint32_t)n;
}
size_t st_rtc_pcm(StRtc *r,int16_t *p,size_t n){uint32_t at=r->read_at,w=r->write_at;__sync_synchronize();size_t available=w-at;if(n>available)n=available;for(size_t i=0;i<n;i++)p[i]=r->pcm[(at+(uint32_t)i)&(PCM_COUNT-1)];__sync_synchronize();r->read_at=at+(uint32_t)n;return n;}
static void rtp(StRtc *r,unsigned char *p,int n) {
    srtp_err_status_t err=srtp_unprotect(r->inbound,p,&n);
    if(err!=srtp_err_status_ok){if(err==srtp_err_status_replay_fail||err==srtp_err_status_replay_old)r->stats.late++;else r->stats.auth_errors++;return;}
    if(n<12||(p[0]>>6)!=2)return;unsigned h=12+(p[0]&15)*4;if(h>(unsigned)n)return;
    if(p[0]&16){if(h+4>(unsigned)n)return;unsigned z=4+st_rtc_u16(p+h+2)*4;if(z>(unsigned)n-h)return;h+=z;}
    if(p[0]&32){unsigned pad=p[n-1];if(!pad||pad>(unsigned)n-h)return;n-=(int)pad;}
    VoiceStream *s=NULL;uint32_t ssrc=st_rtc_u32(p+8);for(unsigned i=0;i<ST_RTC_STREAMS;i++)if(r->streams[i]&&r->streams[i]->ssrc==ssrc)s=r->streams[i];if(!s)return;
    unsigned type=p[1]&127;if(type!=(unsigned)s->opus&&type!=(unsigned)s->red)return;
    unsigned char *payload=p+h;size_t size=(size_t)n-h;
    if(type==(unsigned)s->red){size_t headers=0,skip=0;while(headers<size&&(payload[headers]&128)){if(size-headers<4)return;skip+=((payload[headers+2]&3)<<8)|payload[headers+3];headers+=4;}if(headers>=size||skip>size-headers-1||(payload[headers]&127)!=(unsigned)s->opus)return;headers++;payload+=headers+skip;size-=headers+skip;}
    if(!size||size>1275)return;uint64_t now=st_millis();uint16_t seq=(uint16_t)st_rtc_u16(p+2);
    if(s->started&&now-s->last_rtp>250){s->started=0;s->count=s->at=0;memset(s->slots,0,sizeof(s->slots));opus_decoder_ctl(s->decoder,OPUS_RESET_STATE);}
    if(!s->started){s->expected=seq;s->base_seq=s->highest=seq;s->play_at=now+80;s->started=1;}
    int delta=(int16_t)(seq-s->expected);if(delta<0){r->stats.late++;return;}if(delta>=64){memset(s->slots,0,sizeof(s->slots));s->expected=seq;s->play_at=now+80;}
    RtpSlot *slot=s->slots+(seq&63);memcpy(slot->bytes,payload,size);slot->length=(unsigned)size;slot->sequence=seq;slot->full=1;r->stats.received++;s->received++;s->last_rtp=now;
    int high=(int16_t)(seq-(uint16_t)s->highest);if(high>0)s->highest+=(unsigned)high;
}
static void jitter(StRtc *r,uint64_t now) {
    if(!r->mix_at)r->mix_at=now+80;if(now<r->mix_at)return;if(now-r->mix_at>200)r->mix_at=now;
    int32_t mixed[320]={0};
    for(unsigned i=0;i<ST_RTC_STREAMS;i++) {
        VoiceStream *s=r->streams[i];if(!s)continue;StRtcMember *member=r->stats.members+s->member;
        if(!s->started||now<s->play_at)continue;
        if(now-s->last_rtp>250){memset(s->slots,0,sizeof(s->slots));s->started=0;s->count=s->at=0;member->speaking=0;continue;}
        for(unsigned tries=0;s->count-s->at<320&&tries<4;tries++) {
            if(s->at){memmove(s->samples,s->samples+s->at,(s->count-s->at)*2);s->count-=s->at;s->at=0;}
            RtpSlot *slot=s->slots+(s->expected&63);int available=slot->full&&slot->sequence==s->expected;int16_t pcm[1920];
            int n=opus_decode(s->decoder,available?slot->bytes:NULL,available?(int)slot->length:0,pcm,available?1920:320,0);
            if(available){slot->full=0;r->stats.decoded++;}else r->stats.concealed++;s->expected++;
            if(n<=0||s->count+(unsigned)n>4096)break;memcpy(s->samples+s->count,pcm,(size_t)n*2);s->count+=(unsigned)n;
        }
        unsigned count=s->count-s->at;if(count>320)count=320;uint64_t energy=0;
        for(unsigned j=0;j<count;j++){int v=s->samples[s->at+j];energy+=(uint64_t)((int64_t)v*v);if(!r->stats.deafen)mixed[j]+=v*member->volume/100;}
        s->at+=count;member->level=count?(unsigned)(energy/count):0;if(member->level>22500&&!member->muted)s->speaking_until=now+400;member->speaking=now<s->speaking_until&&!member->muted;
    }
    int16_t pcm[320];for(unsigned i=0;i<320;i++){int v=mixed[i];if(v>32767){v=32767;r->stats.mix_clipped++;}if(v<-32768){v=-32768;r->stats.mix_clipped++;}pcm[i]=(int16_t)v;}fifo_write(r,pcm,320);r->mix_at+=20;
}
static int start_srtp(StRtc *r) {
    unsigned char in[30],out[30];if(!st_rtc_tls_keys(r->dtls,in,out))return fail(r,"Voice encryption keys unavailable");
    srtp_policy_t p;memset(&p,0,sizeof(p));srtp_crypto_policy_set_rtp_default(&p.rtp);srtp_crypto_policy_set_rtcp_default(&p.rtcp);p.ssrc.type=ssrc_any_inbound;p.key=in;p.window_size=128;
    int ok=srtp_create(&r->inbound,&p)==srtp_err_status_ok;p.ssrc.type=ssrc_any_outbound;p.key=out;if(ok)ok=srtp_create(&r->outbound,&p)==srtp_err_status_ok;st_wipe(in,sizeof(in));st_wipe(out,sizeof(out));
    if(!ok)return fail(r,"Voice SRTP initialization failed");r->dtls_ready=1;r->stats.state=ST_RTC_LISTEN;return 1;
}
static void rtcp_report(StRtc *r,uint64_t now) {
    if(now-r->report_at<1000)return;r->report_at=now;
    unsigned char p[256] __attribute__((aligned(16)));memset(p,0,sizeof(p));int n=8;p[0]=0x80;p[1]=201;st_rtc_put32(p+4,r->local_ssrc);
    if(r->is_publisher) {
        p[1]=200;n=28;st_rtc_put32(p+8,(uint32_t)time(NULL)+2208988800u);st_rtc_put32(p+12,(uint32_t)((now%1000)*4294967296ULL/1000));
        uint32_t stamp=r->send_timestamp;if(r->last_send){stamp-=960;if(now>r->last_send)stamp+=(uint32_t)((now-r->last_send)*48);}
        st_rtc_put32(p+16,stamp);st_rtc_put32(p+20,r->sent_packets);st_rtc_put32(p+24,r->sent_octets);
    } else for(unsigned i=0;i<ST_RTC_STREAMS;i++) {
        VoiceStream *s=r->streams[i];if(!s||!s->started)continue;unsigned char *b=p+n;st_rtc_put32(b,s->ssrc);
        uint32_t expected=s->highest-s->base_seq+1,lost=expected>s->received?expected-s->received:0;if(lost>0x7fffff)lost=0x7fffff;
        b[0+4]=expected?(unsigned char)((uint64_t)lost*256/expected):0;b[5]=(unsigned char)(lost>>16);b[6]=(unsigned char)(lost>>8);b[7]=(unsigned char)lost;st_rtc_put32(b+8,s->highest);st_rtc_put32(b+16,s->last_sr);st_rtc_put32(b+20,s->last_sr?(uint32_t)((now-s->sr_at)*65536/1000):0);n+=24;p[0]++;
    }
    st_rtc_put16(p+2,(unsigned)(n/4-1));if(srtp_protect_rtcp(r->outbound,p,&n)==srtp_err_status_ok)send(r->udp,p,(size_t)n,MSG_DONTWAIT);
}
static int udp_step(StRtc *r,uint64_t now) {
    if((r->offer.audio||r->offer.data)&&r->offer.candidate_count&&now-r->ice_at>=(r->nominated?5000u:500u)) {
        if(!r->ice_started)r->ice_started=now;
        unsigned index=r->nominated?r->candidate:(unsigned)((now-r->ice_started)/2500)%r->offer.candidate_count;StRtcCandidate *c=r->offer.candidates+index;
        struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)c->port);inet_pton(AF_INET,c->ip,&a.sin_addr);
        if(connect(r->udp,(struct sockaddr *)&a,sizeof(a))<0&&!again())return fail(r,"Voice UDP route unavailable");
        r->candidate=index;r->ice_at=now;if(!st_random(r->transaction,sizeof(r->transaction)))return fail(r,"Voice random source unavailable");unsigned char p[256];size_t n=st_rtc_stun_request(p,r->transaction,r->offer.ufrag,r->ufrag,r->offer.password,r->controlling,r->tie);if(n){send(r->udp,p,n,MSG_DONTWAIT);r->stats.stun_requests++;}
    }
    for(int packets=0;packets<8;packets++) {
        if(r->datagram_size&&!r->dtls_ready) {
            for(unsigned steps=0;steps<32&&r->datagram_size;steps++) {
                r->stats.dtls_steps++;int ok=st_rtc_tls_step(r->dtls,&r->error);
                if(ok<0)return fail(r,r->error.text);
                if(ok){if(!start_srtp(r))return 0;break;}
            }
            if(r->datagram_size)break;
        }
        unsigned char p[1600] __attribute__((aligned(16)));int n=(int)recv(r->udp,p,sizeof(p),MSG_DONTWAIT);if(n<0){if(again()||errno==ECONNREFUSED)break;return fail(r,"Voice UDP receive failed");}if(!n)break;
        if(n>=20&&(p[0]&0xc0)==0&&st_rtc_u32(p+4)==0x2112a442) {
            if(st_rtc_u16(p)==0x101&&!memcmp(p+8,r->transaction,12)&&st_rtc_stun_check(p,(size_t)n,r->offer.password)){r->stats.stun_replies++;r->consent_at=now;if(r->controlling)r->nominated=1;}
            else if(st_rtc_u16(p)==1&&st_rtc_stun_check(p,(size_t)n,r->pwd)) {
                const unsigned char *v;size_t z;char name[200];snprintf(name,sizeof(name),"%s:%s",r->ufrag,r->offer.ufrag);
                if(st_rtc_stun_attr(p,(size_t)n,6,&v,&z)&&z==strlen(name)&&!memcmp(v,name,z)) {
                    StRtcCandidate *c=r->offer.candidates+r->candidate;struct in_addr ip;inet_pton(AF_INET,c->ip,&ip);unsigned char response[256];size_t size=st_rtc_stun_reply(p,(size_t)n,r->pwd,ntohl(ip.s_addr),c->port,response);if(size){send(r->udp,response,size,MSG_DONTWAIT);r->consent_at=now;}
                    if(st_rtc_stun_attr(p,(size_t)n,0x25,&v,&z))r->nominated=1;
                }
            }
        } else if(p[0]>=20&&p[0]<=63) {
            memcpy(r->datagram,p,(size_t)n);r->datagram_size=(size_t)n;
            if(r->dtls_ready){unsigned char ignored[1600];st_rtc_tls_read(r->dtls,ignored,sizeof(ignored));}
        } else if(r->dtls_ready&&n>=12&&(p[0]>>6)==2) {
            if(p[1]>=192&&p[1]<=223){int size=n;if(srtp_unprotect_rtcp(r->inbound,p,&size)==srtp_err_status_ok&&size>=28&&p[1]==200){for(unsigned i=0;i<ST_RTC_STREAMS;i++)if(r->streams[i]&&r->streams[i]->ssrc==st_rtc_u32(p+4)){r->streams[i]->last_sr=st_rtc_u32(p+10);r->streams[i]->sr_at=now;}}}
            else if(!r->is_publisher)rtp(r,p,n);
        }
        if(r->nominated&&!r->dtls_ready){r->stats.state=ST_RTC_DTLS;r->stats.dtls_steps++;int ok=st_rtc_tls_step(r->dtls,&r->error);if(ok<0)return fail(r,r->error.text);if(ok&&!start_srtp(r))return 0;}
    }
    if(r->nominated&&!r->dtls_ready){r->stats.state=ST_RTC_DTLS;r->stats.dtls_steps++;int ok=st_rtc_tls_step(r->dtls,&r->error);if(ok<0)return fail(r,r->error.text);if(ok&&!start_srtp(r))return 0;}
    now=st_millis();
    if(r->dtls_ready){if(!r->is_publisher)jitter(r,now);rtcp_report(r,now);if(now-r->consent_at>30000)return fail(r,"Voice route no longer responds");}
    return 1;
}
StRtc *st_rtc_new(const StConfig *cfg,const char *url,const char *address,const char *user,StError *err) {
    StRtc *r=calloc(1,sizeof(*r));if(!r){st_fail(err,"Voice memory limit");return NULL;}r->tcp=r->udp=-1;r->stats.state=ST_RTC_PREPARE;st_copy(r->user,sizeof(r->user),user);r->tx=malloc(SIGNAL_MAX);r->pcm=malloc(PCM_COUNT*2);if(!r->tx||!r->pcm)goto bad;
    char authority[256];if(strpbrk(url,"?#")||!st_url_split(url,"wss://",authority,sizeof(authority),r->path,sizeof(r->path)))goto bad;
    st_copy(r->authority,sizeof(r->authority),authority);st_copy(r->host,sizeof(r->host),authority);unsigned port=443;char *colon=strchr(r->host,':');if(colon){*colon++=0;char *end=NULL;unsigned long n=strtoul(colon,&end,10);if(!n||n>65535||*end)goto bad;port=(unsigned)n;}
    if(!random_text(r->ufrag,16)||!random_text(r->pwd,32)||!st_random(&r->tie,sizeof(r->tie))||!st_random(&r->local_ssrc,sizeof(r->local_ssrc)))goto bad;
    r->tls=st_rtc_tls_new(0,cfg->ca_file,r->host,err);r->dtls=st_rtc_tls_new(1,NULL,NULL,err);if(!r->tls||!r->dtls)goto bad;
    struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;if(inet_pton(AF_INET,address,&a.sin_addr)!=1)goto bad;a.sin_port=htons((uint16_t)port);
    r->tcp=socket(AF_INET,SOCK_STREAM,0);r->udp=socket(AF_INET,SOCK_DGRAM,0);if(r->tcp<0||r->udp<0)goto bad;
    if(fcntl(r->tcp,F_SETFL,O_NONBLOCK)<0||fcntl(r->udp,F_SETFL,O_NONBLOCK)<0)goto bad;
    if(connect(r->tcp,(struct sockaddr *)&a,sizeof(a))<0&&!again())goto bad;
    if(connect(r->udp,(struct sockaddr *)&a,sizeof(a))<0&&!again())goto bad;
    socklen_t size=sizeof(a);if(getsockname(r->udp,(struct sockaddr *)&a,&size)<0||!inet_ntop(AF_INET,&a.sin_addr,r->local_ip,sizeof(r->local_ip)))goto bad;r->local_port=ntohs(a.sin_port);
    st_rtc_tls_io(r->tls,r,tcp_send,tcp_recv);st_rtc_tls_io(r->dtls,r,udp_send,udp_recv);r->deadline=st_millis()+40000;
    r->publisher=calloc(1,sizeof(*r->publisher));if(!r->publisher)goto bad;StRtc *p=r->publisher;p->tcp=p->udp=-1;p->is_publisher=1;
    if(!random_text(p->ufrag,16)||!random_text(p->pwd,32)||!st_random(&p->tie,sizeof(p->tie))||!st_random(&p->local_ssrc,sizeof(p->local_ssrc))||!st_random(&p->send_sequence,sizeof(p->send_sequence))||!st_random(&p->send_timestamp,sizeof(p->send_timestamp)))goto bad;
    p->dtls=st_rtc_tls_new(2,NULL,NULL,err);p->udp=socket(AF_INET,SOCK_DGRAM,0);if(!p->dtls||p->udp<0||fcntl(p->udp,F_SETFL,O_NONBLOCK)<0)goto bad;
    struct sockaddr_in peer;memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons((uint16_t)port);if(inet_pton(AF_INET,address,&peer.sin_addr)!=1||connect(p->udp,(struct sockaddr *)&peer,sizeof(peer))<0)goto bad;
    socklen_t peer_size=sizeof(peer);if(getsockname(p->udp,(struct sockaddr *)&peer,&peer_size)<0||!inet_ntop(AF_INET,&peer.sin_addr,p->local_ip,sizeof(p->local_ip)))goto bad;p->local_port=ntohs(peer.sin_port);st_rtc_tls_io(p->dtls,p,udp_send,udp_recv);
    if(srtp_init()!=srtp_err_status_ok)goto bad;r->protected=1;return r;
bad:
    st_rtc_free(r);if(!err->text[0])st_fail(err,"Cannot prepare direct IPv4 voice connection");return NULL;
}
int st_rtc_authorize(StRtc *r,const char *token) {
    if(r->stats.state!=ST_RTC_AUTH)return 0;size_t n=strlen(token);if(!n||n>8192)return fail(r,"Invalid voice token");for(size_t i=0;i<n;i++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.",token[i]))return fail(r,"Invalid voice token");
    unsigned char nonce[16];char key[25];size_t used;
    if(!st_random(nonce,16))return fail(r,"Voice random source unavailable");
    /* Base64 from the small standard websocket helper's SHA1 accept input. */
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";used=0;for(int i=0;i<16;i+=3){unsigned v=(unsigned)nonce[i]<<16;if(i+1<16)v|=(unsigned)nonce[i+1]<<8;if(i+2<16)v|=nonce[i+2];key[used++]=alphabet[v>>18];key[used++]=alphabet[(v>>12)&63];key[used++]=i+1<16?alphabet[(v>>6)&63]:'=';key[used++]=i+2<16?alphabet[v&63]:'=';}key[used]=0;
    if(!st_ws_accept(key,r->accept))return fail(r,"Voice upgrade setup failed");
    size_t path=strlen(r->path);while(path&&r->path[path-1]=='/')r->path[--path]=0;
    char *request=malloc(11000);if(!request)return fail(r,"Voice upgrade memory limit");
    int size=snprintf(request,11000,"GET %s/rtc?access_token=%s&protocol=15&sdk=unknown&version=" ST_APP_VERSION "&auto_subscribe=false HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",r->path,token,r->authority,key);
    int ok=size>0&&size<11000&&queue_bytes(r,request,(size_t)size);st_wipe(request,11000);free(request);r->stats.state=ST_RTC_SIGNAL;r->deadline=st_millis()+30000;return ok;
}
int st_rtc_step(StRtc *r) {
    if(r->stats.state>=ST_RTC_FAILED)return 0;uint64_t now=st_millis();
    if(r->stats.state==ST_RTC_PREPARE) {
        if(!r->tcp_ready){int ready=tcp_ready(r->tcp);if(ready<0&&!again())return fail(r,"Voice TCP polling failed");if(ready>0){int error=0;socklen_t size=sizeof(error);if(getsockopt(r->tcp,SOL_SOCKET,SO_ERROR,&error,&size)<0||error)return fail(r,"Voice TCP connection failed");r->tcp_ready=1;}}
        if(r->tcp_ready){int ok=st_rtc_tls_step(r->tls,&r->error);if(ok<0)return fail(r,r->error.text);if(ok){r->stats.state=ST_RTC_AUTH;r->deadline=now+40000;}}
    } else if(r->stats.state!=ST_RTC_AUTH) {
        if(r->tx_at<r->tx_size){if(!r->tx_chunk){r->tx_chunk=r->tx_size-r->tx_at;if(r->tx_chunk>16384)r->tx_chunk=16384;}int n=st_rtc_tls_write(r->tls,r->tx+r->tx_at,r->tx_chunk);
if(n==-2){}else if(n<=0)return fail(r,"Voice signaling send failed");else {r->tx_at+=(size_t)n;r->tx_chunk=0;if(r->tx_at==r->tx_size){st_wipe(r->tx,r->tx_size);r->tx_at=r->tx_size=0;}}}
        if(!incoming(r))return 0;
        unsigned char p[4096];for(int i=0;i<4;i++){int n=st_rtc_tls_read(r->tls,p,sizeof(p));if(n==-2)break;if(n<=0)return fail(r,"Voice signaling connection lost");if((size_t)n>SIGNAL_MAX-r->input_size)return fail(r,"Voice signal size limit");unsigned char *input=realloc(r->input,r->input_size+(size_t)n);if(!input)return fail(r,"Voice signal memory limit");r->input=input;memcpy(input+r->input_size,p,(size_t)n);r->input_size+=(size_t)n;if(!incoming(r))return 0;}
        now=st_millis();
        if(r->upgraded&&now-r->last_ping>=5000&&!ping(r))return 0;
        if((r->offer.audio||r->offer.data)&&!udp_step(r,now))return 0;
        if(r->publisher&&(r->publisher->offer.audio||r->publisher->offer.data)&&!udp_step(r->publisher,st_millis()))return fail(r,r->publisher->stats.error);
        r->stats.publish_ready=r->publisher&&r->publisher->dtls_ready;
        if(r->publish_requested&&!r->stats.publish_ready&&st_millis()>=r->publisher->deadline)return fail(r,"Microphone connection timed out");
        if(r->upgraded&&now-r->last_receive>30000)return fail(r,"Voice signaling timed out");
    }
    if(r->stats.state!=ST_RTC_LISTEN&&now>=r->deadline)return fail(r,r->stats.state==ST_RTC_ICE?"No direct UDP voice route. TURN/TCP is not supported yet.":"Voice connection timed out");return 1;
}
void st_rtc_close(StRtc *r) {
    if(!r)return;
    /* Best effort Leave over the already verified, nonblocking WSS. Never
     * delay UI teardown or retry a partially pending TLS application write. */
    if(r->tcp>=0&&r->upgraded&&r->stats.state!=ST_RTC_FAILED&&r->tx_at==r->tx_size) {
        unsigned char *p=NULL;size_t n;static const char leave[]="{\"leave\":{}}";
        if(st_ws_masked(1,leave,sizeof(leave)-1,&p,&n)){st_rtc_tls_write(r->tls,p,n);free(p);}
    }
    if(r->tcp>=0){close(r->tcp);r->tcp=-1;}
    if(r->udp>=0){close(r->udp);r->udp=-1;}
    if(r->stats.state!=ST_RTC_FAILED)r->stats.state=ST_RTC_CLOSED;
}
void st_rtc_free(StRtc *r){if(!r)return;st_rtc_close(r);st_rtc_free(r->publisher);for(unsigned i=0;i<ST_RTC_STREAMS;i++)stream_free(r->streams[i]);st_rtc_tls_free(r->tls);st_rtc_tls_free(r->dtls);if(r->inbound)srtp_dealloc(r->inbound);if(r->outbound)srtp_dealloc(r->outbound);if(r->protected)srtp_shutdown();if(r->pcm){st_wipe(r->pcm,PCM_COUNT*2);free(r->pcm);}if(r->tx){st_wipe(r->tx,SIGNAL_MAX);free(r->tx);}free(r->input);if(r->message){st_wipe(r->message,r->message_size);free(r->message);}st_wipe(r,sizeof(*r));free(r);}
StRtcStats st_rtc_stats(const StRtc *r){return r->stats;}
enum StRtcState st_rtc_state(const StRtc *r){return r->stats.state;}
int st_rtc_publishing(const StRtc *r){return r->stats.publish_ready;}
