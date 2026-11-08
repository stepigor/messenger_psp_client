#include "transport.h"
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <mbedtls/version.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int st_ws_accept(const char *key,char out[29]) {
    char source[128]; unsigned char hash[20]; size_t n;
    if (snprintf(source,sizeof(source),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key)>=(int)sizeof(source)) return 0;
#if MBEDTLS_VERSION_MAJOR >= 3
    if (mbedtls_sha1((unsigned char *)source,strlen(source),hash)) return 0;
#else
    if (mbedtls_sha1_ret((unsigned char *)source,strlen(source),hash)) return 0;
#endif
    return mbedtls_base64_encode((unsigned char *)out,29,&n,hash,20)==0;
}
int st_ws_frame(const unsigned char *b,size_t n,size_t *header,size_t *length,int *opcode,int *fin) {
    if (n<2) return 0;
    *opcode=b[0]&15; *fin=!!(b[0]&128);
    if ((b[0]&0x70) || (b[1]&128) || !(*opcode==0||*opcode==1||*opcode==2||*opcode==8||*opcode==9||*opcode==10)) return -1;
    size_t h=2; uint64_t len=b[1]&127;
    if (len==126) { if (n<4) return 0; len=((unsigned)b[2]<<8)|b[3]; h=4; if (len<126) return -1; }
    else if (len==127) {
        if (n<10) return 0;
        len=0; for(int i=2;i<10;i++) len=(len<<8)|b[i]; h=10;
        if (len<65536) return -1;
    }
    if (len>ST_BODY_MAX || (*opcode>=8 && (!*fin || len>125))) return -1;
    *header=h; *length=(size_t)len;
    return n>=h+(size_t)len?1:0;
}
int st_ws_masked(int opcode,const void *data,size_t n,unsigned char **out,size_t *size) {
    if (n>ST_BODY_MAX || (opcode>=8 && n>125)) return 0;
    size_t h=n<126?2:n<65536?4:10; *size=h+4+n;
    unsigned char *b=malloc(*size); if (!b) return 0;
    b[0]=(unsigned char)(128|opcode);
    if (h==2) b[1]=(unsigned char)(128|n);
    else if (h==4) { b[1]=128|126; b[2]=(unsigned char)(n>>8); b[3]=(unsigned char)n; }
    else { b[1]=128|127; for(int i=0;i<8;i++) b[2+i]=(unsigned char)((uint64_t)n>>(56-i*8)); }
    if (!st_random(b+h,4)) { free(b); return 0; }
    for(size_t i=0;i<n;i++) b[h+4+i]=((const unsigned char *)data)[i]^b[h+i%4];
    *out=b; return 1;
}
static int transfer(CURL *h,void *data,size_t n,int sending,uint64_t deadline) {
    unsigned char *p=data;
    while (n && !st_cancelled() && st_millis()<deadline) {
        size_t count=0;
        CURLcode r=sending?curl_easy_send(h,p,n,&count):curl_easy_recv(h,p,n,&count);
        if (r==CURLE_AGAIN) { st_pause(); continue; }
        if (r!=CURLE_OK || !count) return 0;
        p+=count; n-=count;
    }
    return n==0;
}
static int send_frame(CURL *h,int opcode,const void *data,size_t n,uint64_t deadline) {
    unsigned char *buf=NULL; size_t size=0;
    if (!st_ws_masked(opcode,data,n,&buf,&size)) return 0;
    int ok=transfer(h,buf,size,1,deadline); st_wipe(buf,size); free(buf); return ok;
}
static int header_is(const char *headers,const char *name,const char *value) {
    const char *p=strstr(headers,"\r\n"); size_t n=strlen(name);
    while (p && p[2]) {
        p+=2; const char *end=strstr(p,"\r\n"); if (!end) break;
        if ((size_t)(end-p)>n && !strncasecmp(p,name,n) && p[n]==':') {
            const char *v=p+n+1; while (v<end && (*v==' '||*v=='\t')) ++v;
            while(end>v && (end[-1]==' '||end[-1]=='\t')) --end;
            if((size_t)(end-v)!=strlen(value))return 0;
            return !strcmp(name,"Sec-WebSocket-Accept") ? !memcmp(v,value,(size_t)(end-v)) : !strncasecmp(v,value,(size_t)(end-v));
        }
        p=end;
    }
    return 0;
}
static int open_ws(StClient *c,CURL **handle,CURLM **multi,uint64_t deadline,StError *err) {
    *handle=NULL;*multi=NULL; memset(err,0,sizeof(*err));
    char host[512],path[700],url[1200],request[1800],key[25],accept[29];
    unsigned char random[16]; size_t count;
    if (!st_url_split(c->ws_url,"wss://",host,sizeof(host),path,sizeof(path))) return st_fail(err,"API returned invalid WSS endpoint");
    size_t used=strlen(path);
    if(snprintf(path+used,sizeof(path)-used,"%cversion=1&format=json&ready=users&ready=servers&ready=channels&ready=emojis&ready=voice_states",strchr(path,'?')?'&':'?')>=(int)(sizeof(path)-used))return st_fail(err,"WSS endpoint path is too long");
    snprintf(url,sizeof(url),"https://%s%s",host,path);
    if (!st_random(random,sizeof(random)) || mbedtls_base64_encode((unsigned char *)key,sizeof(key),&count,random,sizeof(random)) || !st_ws_accept(key,accept)) return st_fail(err,"Random source unavailable");
    StTlsDiag diag;
    CURL *h=st_curl_new(&c->config,&diag,err); if (!h) return 0;
    char curl_error[CURL_ERROR_SIZE]={0};
    curl_easy_setopt(h,CURLOPT_URL,url);
    curl_easy_setopt(h,CURLOPT_CONNECT_ONLY,1L);
    curl_easy_setopt(h,CURLOPT_ERRORBUFFER,curl_error);
    st_stage("WSS / TLS");
    CURLM *owner=NULL;CURLcode result=st_curl_connect(h,&owner,deadline);
    if (result!=CURLE_OK) {
        st_curl_error(err,"WSS connect",result,curl_error,&diag);
        st_curl_free(h,owner); return 0;
    }
    st_stage("WSS / Ready");
    snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",path,host,key);
    if (!transfer(h,request,strlen(request),1,deadline)) goto done;
    /* Read exactly the HTTP header, preserving frames in the TLS stream. */
    char headers[8192]; size_t hn=0;
    while (hn<sizeof(headers)-1) {
        if (!transfer(h,headers+hn,1,0,deadline)) goto done;
        ++hn; headers[hn]=0;
        if (hn>=4 && !memcmp(headers+hn-4,"\r\n\r\n",4)) break;
    }
    if (strncmp(headers,"HTTP/1.1 101 ",13) || !header_is(headers,"Upgrade","websocket") || !header_is(headers,"Connection","Upgrade") || !header_is(headers,"Sec-WebSocket-Accept",accept)) {
        st_fail(err,"WSS upgrade rejected or invalid"); goto done;
    }
    cJSON *auth=cJSON_CreateObject(); if (!auth) goto done;
    cJSON_AddStringToObject(auth,"type","Authenticate"); cJSON_AddStringToObject(auth,"token",c->token);
    char *text=cJSON_PrintUnformatted(auth);
    st_wipe((void *)st_str(auth,"token"),strlen(st_str(auth,"token"))); cJSON_Delete(auth);
    if (!text) goto done;
    int sent=send_frame(h,1,text,strlen(text),deadline); st_wipe(text,strlen(text)); cJSON_free(text);
    if (!sent) goto done;
    curl_easy_setopt(h,CURLOPT_ERRORBUFFER,NULL);
    *handle=h;*multi=owner;return 1;
done:
    st_curl_free(h,owner);if(!err->text[0])st_fail(err,st_cancelled()?"Cancelled":"Event connection unavailable");return 0;
}
int st_ws_ready(StClient *c,cJSON **out,StError *err) {
    *out=NULL;CURL *h=NULL;CURLM *owner=NULL;uint64_t deadline=st_millis()+40000;int good=0;
    if(!open_ws(c,&h,&owner,deadline,err))return 0;
    unsigned char *message=NULL; size_t ml=0; int assembling=0;
    for (int frames=0;frames<512 && st_millis()<deadline;frames++) {
        unsigned char fh[10]; size_t hs=2, len=0, ignored; int op=0,fin=0;
        if (!transfer(h,fh,2,0,deadline)) break;
        if ((fh[1]&127)==126) hs=4;
        if ((fh[1]&127)==127) hs=10;
        if (hs>2 && !transfer(h,fh+2,hs-2,0,deadline)) break;
        if (st_ws_frame(fh,hs,&ignored,&len,&op,&fin)<0) { st_fail(err,"Invalid WSS frame"); break; }
        unsigned char *payload=malloc(len+1); if (!payload) break;
        if (!transfer(h,payload,len,0,deadline)) { free(payload); break; }
        payload[len]=0;
        if (op==8) { free(payload); st_fail(err,"WSS closed before Ready"); break; }
        if (op==9) { int ok=send_frame(h,10,payload,len,deadline); free(payload); if (!ok) break; continue; }
        if (op==10) { free(payload); continue; }
        if (op==2 || (op==0 && !assembling) || (op==1 && assembling) || len>ST_BODY_MAX-ml) { free(payload); st_fail(err,"Invalid fragmented WSS message"); break; }
        unsigned char *joined=realloc(message,ml+len+1);
        if (!joined) { free(payload); break; }
        message=joined; memcpy(message+ml,payload,len); ml+=len; message[ml]=0; free(payload);
        if (!fin) { assembling=1; continue; }
        assembling=0;
        cJSON *event=cJSON_ParseWithLengthOpts((char *)message,ml+1,NULL,1);
        free(message); message=NULL; ml=0;
        if (!event) { st_fail(err,"Invalid WSS JSON"); break; }
        const char *type=st_str(event,"type");
        if (!strcmp(type,"Ready")) { *out=event; good=1; break; }
        if (!strcmp(type,"Error")) { st_fail(err,"WSS authentication rejected"); cJSON_Delete(event); break; }
        cJSON_Delete(event);
    }
    free(message);
    if (good) { unsigned char close[2]={3,232}; send_frame(h,8,close,2,st_millis()+1000); }
    st_curl_free(h,owner);
    if (!good && !err->text[0]) st_fail(err,st_cancelled()?"Cancelled":"WSS timeout, connection lost or memory limit");
    return good;
}

#include "live.h"
typedef struct {
    CURL *handle;CURLM *owner;unsigned char *input,*message;
    size_t length,message_length;int assembling,ready;
    uint64_t opened,last_receive,last_ping,last_typing;
    char token[256],url[512],writing[64];
    unsigned char presence_tx[256];size_t presence_size,presence_at;
} LiveSocket;
static LiveSocket live_socket;
void st_ws_live_close(void) {
#ifdef __PSP__
    st_network_protect(-1);
#endif
    if(live_socket.handle)st_curl_free(live_socket.handle,live_socket.owner);
    if(live_socket.input){st_wipe(live_socket.input,live_socket.length);free(live_socket.input);}
    if(live_socket.message){st_wipe(live_socket.message,live_socket.message_length);free(live_socket.message);}
    st_wipe(&live_socket,sizeof(live_socket));
}
static int live_json(const char *type,const char *channel,StError *err) {
    cJSON *o=cJSON_CreateObject();if(!o)return st_fail(err,"Out of memory");cJSON_AddStringToObject(o,"type",type);
    if(channel)cJSON_AddStringToObject(o,"channel",channel);else cJSON_AddNumberToObject(o,"data",(double)(st_millis()&0x7fffffff));
    char *text=cJSON_PrintUnformatted(o);cJSON_Delete(o);if(!text)return st_fail(err,"Out of memory");
    int ok=send_frame(live_socket.handle,1,text,strlen(text),st_millis()+100);free(text);return ok?1:st_fail(err,"Event connection interrupted");
}
static int live_frames(cJSON *events,StError *err,uint64_t deadline) {
    LiveSocket *s=&live_socket;
    while(s->length&&cJSON_GetArraySize(events)<16&&st_millis()<deadline) {
        size_t h,n;int op,fin;int decoded=st_ws_frame(s->input,s->length,&h,&n,&op,&fin);
        if(decoded<0)return st_fail(err,"Invalid event frame");
        if(!decoded)return 1;
        unsigned char *payload=s->input+h;s->last_receive=st_millis();
        if(op==8)return st_fail(err,"Event connection closed");
        if(op==9){if(!send_frame(s->handle,10,payload,n,deadline))return st_fail(err,"Event pong failed");}
        else if(op!=10) {
            if(op==2||(op==0&&!s->assembling)||(op==1&&s->assembling)||n>ST_BODY_MAX-s->message_length)return st_fail(err,"Invalid event fragments");
            unsigned char *joined=realloc(s->message,s->message_length+n+1);if(!joined)return st_fail(err,"Out of memory");s->message=joined;
            memcpy(joined+s->message_length,payload,n);s->message_length+=n;joined[s->message_length]=0;s->assembling=!fin;
            if(fin) {
                cJSON *event=cJSON_ParseWithLengthOpts((char *)s->message,s->message_length+1,NULL,1);
                st_wipe(s->message,s->message_length);free(s->message);s->message=NULL;s->message_length=0;
                if(!cJSON_IsObject(event)){cJSON_Delete(event);return st_fail(err,"Invalid event JSON");}
                if(!strcmp(st_str(event,"type"),"Ready"))s->ready=1;
                if(!strcmp(st_str(event,"type"),"Error")) {
                    const char *type=st_str(cJSON_GetObjectItemCaseSensitive(event,"data"),"type");int invalid=!strcmp(type,"InvalidSession")||!strcmp(type,"NotAuthenticated");
                    cJSON_Delete(event);if(invalid)err->status=401;return st_fail(err,invalid?"Session expired":"Chat connection unavailable");
                }
                cJSON_AddItemToArray(events,event);
            }
        }
        memmove(s->input,s->input+h+n,s->length-h-n);s->length-=h+n;
    }
    return 1;
}
int st_ws_live(StClient *c,const char *typing,cJSON **events,int *connected,StError *err) {
    *events=cJSON_CreateArray();*connected=0;memset(err,0,sizeof(*err));if(!*events)return st_fail(err,"Out of memory");LiveSocket *s=&live_socket;
    if(!c->token[0]){st_ws_live_close();return st_fail(err,"Login required");}
    if(s->handle&&(strcmp(s->token,c->token)||strcmp(s->url,c->ws_url)))st_ws_live_close();
    if(!s->handle) {
        if(!open_ws(c,&s->handle,&s->owner,st_millis()+40000,err))return 0;
        /* No stack-backed curl options survive the handshake. */
        curl_easy_setopt(s->handle,CURLOPT_ERRORBUFFER,NULL);
        st_copy(s->token,sizeof(s->token),c->token);st_copy(s->url,sizeof(s->url),c->ws_url);s->opened=s->last_receive=s->last_ping=st_millis();
#ifdef __PSP__
        curl_socket_t fd=CURL_SOCKET_BAD;curl_easy_getinfo(s->handle,CURLINFO_ACTIVESOCKET,&fd);st_network_protect((int)fd);
#endif
    }
    uint64_t deadline=st_millis()+50;int ok=1;
    do {
        if(st_cancelled()){ok=st_fail(err,"Cancelled");break;}
        if(!live_frames(*events,err,deadline)){ok=0;break;}if(cJSON_GetArraySize(*events)>=16)break;
        size_t room=ST_BODY_MAX+14-s->length,want=room<4096?room:4096;if(!want){ok=st_fail(err,"Event buffer limit");break;}
        unsigned char bytes[4096];size_t n=0;CURLcode result=curl_easy_recv(s->handle,bytes,want,&n);
        if(result==CURLE_AGAIN){if(cJSON_GetArraySize(*events))break;st_pause();continue;}
        if(result!=CURLE_OK||!n){ok=st_fail(err,"Event connection interrupted");break;}
        unsigned char *input=realloc(s->input,s->length+n);if(!input){ok=st_fail(err,"Out of memory");break;}
        s->input=input;memcpy(input+s->length,bytes,n);s->length+=n;
    }while(st_millis()<deadline);
    if(ok)ok=live_frames(*events,err,deadline);
    uint64_t now=st_millis();
    if(ok&&((!s->ready&&now-s->opened>=15000)||now-s->last_receive>=60000))ok=st_fail(err,"Event connection timed out");
    if(ok&&s->ready) {
        if(strcmp(s->writing,typing)) {if(s->writing[0])ok=live_json("EndTyping",s->writing,err);s->writing[0]=0;s->last_typing=0;}
        if(ok&&*typing&&(!s->writing[0]||now-s->last_typing>=5000)){ok=live_json("BeginTyping",typing,err);if(ok){st_copy(s->writing,sizeof(s->writing),typing);s->last_typing=now;}}
        if(ok&&now-s->last_ping>=20000){ok=live_json("Ping",NULL,err);s->last_ping=now;}
    }
    if(!ok)st_ws_live_close();else *connected=s->ready;
    return ok;
}


static int presence_queue(int opcode,const void *payload,size_t size,StError *err) {
    LiveSocket *s=&live_socket;unsigned char *data=NULL;size_t n=0;
    if(s->presence_size)return st_fail(err,"Chat connection is busy");
    if(!st_ws_masked(opcode,payload,size,&data,&n)||n>sizeof(s->presence_tx)){free(data);return st_fail(err,"Invalid chat keepalive");}
    memcpy(s->presence_tx,data,n);free(data);s->presence_size=n;s->presence_at=0;return 1;
}
int st_ws_presence_begin(StClient *c,StError *err) {
    uint64_t deadline=st_millis()+45000;int connected=0;
    do {cJSON *events=NULL;int ok=st_ws_live(c,"",&events,&connected,err);cJSON_Delete(events);if(!ok)return 0;}while(!connected&&!st_cancelled()&&st_millis()<deadline);
    return connected?1:st_fail(err,"Chat connection is unavailable");
}
int st_ws_presence_step(StError *err) {
    LiveSocket *s=&live_socket;if(!s->handle||!s->ready)return st_fail(err,"Chat connection is unavailable");
    /* One bounded TLS read/write per turn. Never sleep or reopen in realtime. */
    if(s->presence_size) {
        size_t n=0;CURLcode code=curl_easy_send(s->handle,s->presence_tx+s->presence_at,s->presence_size-s->presence_at,&n);
        if(code!=CURLE_OK&&code!=CURLE_AGAIN)return st_fail(err,"Chat connection was lost");
        s->presence_at+=n;if(s->presence_at==s->presence_size)s->presence_at=s->presence_size=0;
    }
    for(unsigned frames=0;frames<8&&!s->presence_size;frames++) {
        size_t h,n;int op,fin;int result=st_ws_frame(s->input,s->length,&h,&n,&op,&fin);
        if(result<0)return st_fail(err,"Invalid chat data");
        if(!result)break;
        unsigned char *payload=s->input+h;s->last_receive=st_millis();
        if(op==8)return st_fail(err,"Chat connection was closed");
        if(op==9&&!presence_queue(10,payload,n,err))return 0;
        /* Application events are reconciled after the call. An account logout
         * must stop capture promptly, even though ordinary UI updates pause. */
        if(op==1&&fin&&n<4096){cJSON *event=cJSON_ParseWithLength((const char *)payload,n);const char *type=st_str(event,"type");int invalid=!strcmp(type,"Logout")||(!strcmp(type,"Error")&&(!strcmp(st_str(cJSON_GetObjectItemCaseSensitive(event,"data"),"type"),"InvalidSession")||!strcmp(st_str(cJSON_GetObjectItemCaseSensitive(event,"data"),"type"),"NotAuthenticated")));cJSON_Delete(event);if(invalid){err->status=401;return st_fail(err,"Session expired");}}
        memmove(s->input,s->input+h+n,s->length-h-n);s->length-=h+n;
    }
    if(s->length>=ST_BODY_MAX)return st_fail(err,"Chat data exceeds memory limit");
    unsigned char bytes[4096];size_t n=0,want=ST_BODY_MAX-s->length;if(want>sizeof(bytes))want=sizeof(bytes);
    CURLcode code=curl_easy_recv(s->handle,bytes,want,&n);
    if(code==CURLE_OK&&!n)return st_fail(err,"Chat connection was lost");
    if(code!=CURLE_OK&&code!=CURLE_AGAIN)return st_fail(err,"Chat connection was lost");
    if(n){unsigned char *p=realloc(s->input,s->length+n);if(!p)return st_fail(err,"Out of memory");s->input=p;memcpy(p+s->length,bytes,n);s->length+=n;}
    uint64_t now=st_millis();if(now-s->last_receive>=60000)return st_fail(err,"Chat connection timed out");
    if(!s->presence_size&&now-s->last_ping>=20000){char ping[80];int size=snprintf(ping,sizeof(ping),"{\"type\":\"Ping\",\"data\":%u}",(unsigned)(now&0x7fffffff));if(!presence_queue(1,ping,(size_t)size,err))return 0;s->last_ping=now;}
    return 1;
}
