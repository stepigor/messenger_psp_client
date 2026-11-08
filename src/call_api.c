#include "call_api.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
int st_call_prepare(StClient *c,StCallTicket *t,StError *e) {
    memset(t,0,sizeof(*t));
    if(!c->voice_url[0]||!c->voice_node[0])return st_fail(e,"This server has no supported voice node.");
    char host[256],path[768];
    if(!st_url_split(c->voice_url,"wss://",host,sizeof(host),path,sizeof(path)))return st_fail(e,"Invalid secure voice endpoint.");
    char *port=strchr(host,':');if(port)*port=0;
    struct hostent *resolved=gethostbyname(host);
    if(!resolved||resolved->h_addrtype!=AF_INET||resolved->h_length!=4||!resolved->h_addr_list[0]||!inet_ntop(AF_INET,resolved->h_addr_list[0],t->address,sizeof(t->address)))return st_fail(e,"Cannot resolve voice endpoint.");
    st_copy(t->url,sizeof(t->url),c->voice_url);st_copy(t->node,sizeof(t->node),c->voice_node);return 1;
}
int st_call_authorize(StClient *c,const char *channel,int force,StCallTicket *t,StError *e) {
    st_wipe(t,sizeof(*t));
    if(strlen(channel)!=26||strspn(channel,"0123456789ABCDEFGHJKMNPQRSTVWXYZ")!=26)return st_fail(e,"Invalid voice channel.");
    cJSON *body=cJSON_CreateObject(),*result=NULL;if(!body)return st_fail(e,"Out of memory.");
    cJSON_AddStringToObject(body,"node",c->voice_node);
    /* Only an explicit Reconnect here action may replace another connection. */
    if(force)cJSON_AddBoolToObject(body,"force_disconnect",1);
    /* DM/group voice exists implicitly in 0.15.1. Ask the server to ring the
     * other recipients; it only notifies when this is the first participant. */
    char metadata_path[64];snprintf(metadata_path,sizeof(metadata_path),"/channels/%s",channel);cJSON *metadata=NULL;
    if(!st_http(c,"GET",metadata_path,NULL,NULL,&metadata,e)){cJSON_Delete(body);return 0;}
    const char *kind=st_str(metadata,"channel_type");
    if(!strcmp(kind,"DirectMessage")||!strcmp(kind,"Group")) {
        cJSON *recipients=cJSON_GetObjectItemCaseSensitive(metadata,"recipients"),*recipient,*notify=cJSON_AddArrayToObject(body,"recipients");
        if(!cJSON_IsArray(recipients)||cJSON_GetArraySize(recipients)>1024){cJSON_Delete(metadata);cJSON_Delete(body);return st_fail(e,"Invalid call recipients");}
        cJSON_ArrayForEach(recipient,recipients){const char *id=cJSON_IsString(recipient)?recipient->valuestring:"";if(strlen(id)!=26||strspn(id,"0123456789ABCDEFGHJKMNPQRSTVWXYZ")!=26){cJSON_Delete(metadata);cJSON_Delete(body);return st_fail(e,"Invalid call recipient");}if(strcmp(id,c->self_id))cJSON_AddItemToArray(notify,cJSON_CreateString(id));}
    }
    cJSON_Delete(metadata);
    char path[96];snprintf(path,sizeof(path),"/channels/%s/join_call",channel);
    int ok=st_http(c,"POST",path,body,NULL,&result,e);cJSON_Delete(body);
    if(ok) {
        const char *token=st_str(result,"token"),*url=st_str(result,"url");
        if(strcmp(url,c->voice_url))ok=st_fail(e,"Call uses another voice node. Reconnect support is not available yet.");
        else if(!*token||strlen(token)>=sizeof(t->token))ok=st_fail(e,"Invalid voice ticket.");
        else {st_copy(t->token,sizeof(t->token),token);st_copy(t->url,sizeof(t->url),url);}
    }
    if(result){const char *token=st_str(result,"token");st_wipe((void *)token,strlen(token));cJSON_Delete(result);}return ok;
}
