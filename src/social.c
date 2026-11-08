#include "stoat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static int valid(const char *s){size_t n=strlen(s);if(!n||n>=64)return 0;for(;*s;s++)if(!((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return 0;return 1;}
int st_friend_request(StClient *c,const char *username,StError *err) {
    const char *hash=strchr(username,'#');size_t count;char name[129];
    if(!hash||hash==username||(size_t)(hash-username)>=sizeof(name)||strlen(hash+1)!=4)return st_fail(err,"Enter username#0000");
    memcpy(name,username,(size_t)(hash-username));name[hash-username]=0;
    if(!st_utf8_count(name,&count)||count<2||count>32||strchr(name,'\r')||strchr(name,'\n'))return st_fail(err,"Invalid username");
    for(int i=1;i<=4;i++)if(hash[i]<'0'||hash[i]>'9')return st_fail(err,"Discriminator must contain four digits");
    cJSON *body=cJSON_CreateObject(),*res=NULL;if(!body)return st_fail(err,"Out of memory");cJSON_AddStringToObject(body,"username",username);
    int ok=st_http(c,"POST","/users/friend",body,NULL,&res,err);cJSON_Delete(body);
    if(ok)ok=st_merge_user(c,res,err);
    cJSON_Delete(res);return ok;
}
int st_invite_links(const StClient *c,const char *text,StInviteLink out[ST_INVITE_LINK_MAX]) {
    char own[512],base[512];if(!st_url_split(c->app_url[0]?c->app_url:c->config.api_url,"https://",own,sizeof(own),base,sizeof(base)))return 0;
    if(!c->app_url[0])base[0]=0;else {size_t n=strlen(base);while(n&&base[n-1]=='/')base[--n]=0;}
    int count=0;const char *p=text;
    while(count<ST_INVITE_LINK_MAX&&(p=strstr(p,"https://"))) {
        const char *end=p;while(*end&&(unsigned char)*end>32&&!strchr("<>\"')]} `",*end))end++;
        size_t n=(size_t)(end-p);char url[1200],host[512],path[700];if(n>=sizeof(url)){p=end;continue;}
        memcpy(url,p,n);url[n]=0;p=end;
        if(!st_url_split(url,"https://",host,sizeof(host),path,sizeof(path)))continue;
        const char *code=NULL;char prefix[600];snprintf(prefix,sizeof(prefix),"%s/invite/",base);
        if(!strcasecmp(host,own)&&!strncmp(path,prefix,strlen(prefix)))code=path+strlen(prefix);
        else if((!strcasecmp(host,"rvlt.gg")||!strcasecmp(host,"stt.gg"))&&path[0]=='/')code=path+1;
        if(!code)continue;
        size_t length=0;while(code[length]&&strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",code[length]))length++;
        if(!length||length>=64||(code[length]&&!strchr("?#.,;:!",code[length])))continue;
        char value[64];memcpy(value,code,length);value[length]=0;int duplicate=0;for(int i=0;i<count;i++)if(!strcmp(out[i].code,value))duplicate=1;
        if(!duplicate)st_copy(out[count++].code,64,value);
    }
    return count;
}
int st_fetch_invite(StClient *c,const char *code,StError *err) {
    if(!valid(code))return st_fail(err,"Invalid invitation code");
    char path[100];snprintf(path,sizeof(path),"/invites/%s",code);cJSON *res=NULL;
    if(!st_http(c,"GET",path,NULL,NULL,&res,err))return 0;
    if(strcmp(st_str(res,"type"),"Server")||strcmp(st_str(res,"code"),code)||!valid(st_str(res,"server_id"))||!valid(st_str(res,"channel_id"))){cJSON_Delete(res);return st_fail(err,"This is not a server invitation");}
    cJSON_Delete(c->invitation);c->invitation=res;st_copy(c->invitation_code,64,code);return 1;
}
int st_join_invite(StClient *c,const char *code,StError *err) {
    if(!valid(code)||strcmp(c->invitation_code,code)||!c->invitation)return st_fail(err,"Load the invitation first");
    if(strcmp(st_str(c->invitation,"type"),"Server"))return st_fail(err,"Only server invitations are supported");
    char path[100];snprintf(path,sizeof(path),"/invites/%s",code);cJSON *res=NULL;
    if(!st_http(c,"POST",path,NULL,NULL,&res,err))return 0;
    const char *id=st_str(cJSON_GetObjectItemCaseSensitive(res,"server"),"_id");
    st_copy(c->joined_server,64,valid(id)?id:st_str(c->invitation,"server_id"));st_copy(c->joined_channel,64,st_str(c->invitation,"channel_id"));
    cJSON_Delete(res);return 1;
}
void st_remove_server(StClient *c,const char *id) {
    int messages=0,participants=0;
    for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].server,id)) {
        if(!strcmp(c->channels[i].id,c->messages_channel))messages=1;
        if(!strcmp(c->channels[i].id,c->participants_channel))participants=1;
        if(!strcmp(c->channels[i].id,c->pending_channel)){st_wipe(c->pending_key,sizeof(c->pending_key));st_wipe(c->pending_channel,sizeof(c->pending_channel));st_wipe(c->pending_text,sizeof(c->pending_text));}
    }
    size_t n=0;for(size_t i=0;i<c->voice_count;i++){int removed=0;for(size_t k=0;k<c->channel_count;k++)if(!strcmp(c->channels[k].server,id)&&!strcmp(c->channels[k].id,c->voice[i].channel))removed=1;if(!removed)c->voice[n++]=c->voice[i];}c->voice_count=n;
    n=0;for(size_t i=0;i<c->channel_count;i++)if(strcmp(c->channels[i].server,id))c->channels[n++]=c->channels[i];c->channel_count=n;
    n=0;for(size_t i=0;i<c->server_count;i++)if(strcmp(c->servers[i].id,id))c->servers[n++]=c->servers[i];c->server_count=n;
    n=0;for(size_t i=0;i<c->emoji_count;i++)if(strcmp(c->emojis[i].server,id))c->emojis[n++]=c->emojis[i];c->emoji_count=n;
    n=0;for(size_t i=0;i<c->role_count;i++)if(strcmp(c->roles[i].server,id))c->roles[n++]=c->roles[i];c->role_count=n;
    if(!strcmp(c->members_server,id))st_members_clear(c);
    if(messages){free(c->messages);c->messages=NULL;c->message_count=0;c->messages_channel[0]=0;}
    if(participants){free(c->participants);c->participants=NULL;c->participant_count=0;c->participants_channel[0]=0;}
}
int st_leave_server(StClient *c,const char *id,StError *err) {
    if(!valid(id))return st_fail(err,"Invalid server ID");
    char path[100];snprintf(path,sizeof(path),"/servers/%s",id);cJSON *server=NULL;
    if(!st_http(c,"GET",path,NULL,NULL,&server,err))return 0;
    const char *owner=st_str(server,"owner");int verified=!strcmp(st_str(server,"_id"),id)&&valid(owner),owns=!strcmp(owner,c->self_id);
    cJSON_Delete(server);if(!verified)return st_fail(err,"Cannot verify the server owner");
    if(owns)return st_fail(err,"Server owners must transfer ownership before leaving");
    cJSON *res=NULL;int ok=st_http(c,"DELETE",path,NULL,NULL,&res,err);cJSON_Delete(res);if(ok)st_remove_server(c,id);return ok;
}
void st_remove_channel(StClient *c,const char *id) {
    char copy[64];st_copy(copy,sizeof(copy),id);id=copy;
    size_t n=0;for(size_t i=0;i<c->channel_count;i++)if(strcmp(c->channels[i].id,id))c->channels[n++]=c->channels[i];c->channel_count=n;
    n=0;for(size_t i=0;i<c->voice_count;i++)if(strcmp(c->voice[i].channel,id))c->voice[n++]=c->voice[i];c->voice_count=n;
    if(!strcmp(c->messages_channel,id)){free(c->messages);c->messages=NULL;c->message_count=0;c->messages_channel[0]=0;}
    if(!strcmp(c->participants_channel,id)){free(c->participants);c->participants=NULL;c->participant_count=0;c->participants_channel[0]=0;}
    if(!strcmp(c->pending_channel,id)){st_wipe(c->pending_channel,sizeof(c->pending_channel));st_wipe(c->pending_key,sizeof(c->pending_key));st_wipe(c->pending_text,sizeof(c->pending_text));st_wipe(c->pending_reply,sizeof(c->pending_reply));}
    if(!strcmp(c->opened_dm,id))c->opened_dm[0]=0;
}
int st_leave_group(StClient *c,const char *id,StError *err) {
    if(!valid(id))return st_fail(err,"Invalid group ID");
    char path[96];snprintf(path,sizeof(path),"/channels/%s",id);cJSON *raw=NULL;
    if(!st_http(c,"GET",path,NULL,NULL,&raw,err))return 0;
    int group=!strcmp(st_str(raw,"_id"),id)&&!strcmp(st_str(raw,"channel_type"),"Group");cJSON_Delete(raw);
    /* The same DELETE route can delete server channels: verify fresh type. */
    if(!group)return st_fail(err,"Only group conversations can be left here");
    int ok=st_http(c,"DELETE",path,NULL,NULL,&raw,err);cJSON_Delete(raw);if(ok)st_remove_channel(c,id);return ok;
}
void st_remove_message(StClient *c,const char *channel,const char *id) {
    if(strcmp(channel,c->messages_channel))return;
    st_quotes_invalidate(c,id,1);
    for(size_t i=0;i<c->message_count;i++)if(!strcmp(c->messages[i].id,id)){memmove(c->messages+i,c->messages+i+1,(c->message_count-i-1)*sizeof(*c->messages));c->message_count--;break;}
}
int st_delete_message(StClient *c,const char *channel,const char *id,StError *err) {
    if(!valid(channel)||!valid(id)||!c->self_id[0])return st_fail(err,"Invalid message");
    char path[160];snprintf(path,sizeof(path),"/channels/%s/messages/%s",channel,id);cJSON *raw=NULL;
    if(!st_http(c,"GET",path,NULL,NULL,&raw,err)){
        if(err->status==404){memset(err,0,sizeof(*err));st_remove_message(c,channel,id);return 1;}return 0;
    }
    int own=!strcmp(st_str(raw,"_id"),id)&&!strcmp(st_str(raw,"channel"),channel)&&!strcmp(st_str(raw,"author"),c->self_id)&&(!cJSON_HasObjectItem(raw,"system")||cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(raw,"system")));
    cJSON_Delete(raw);raw=NULL;if(!own)return st_fail(err,"You can only delete your own messages");
    int ok=st_http(c,"DELETE",path,NULL,NULL,&raw,err);cJSON_Delete(raw);
    if(!ok&&err->status==404){memset(err,0,sizeof(*err));ok=1;}
    if(ok)st_remove_message(c,channel,id);
    return ok;
}
