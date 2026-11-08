#include "stoat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int id_valid(const char *id) {
    size_t n=strlen(id);if(!n||n>=64)return 0;
    for(;*id;id++)if(!((*id>='A'&&*id<='Z')||(*id>='a'&&*id<='z')||(*id>='0'&&*id<='9')||*id=='_'||*id=='-'))return 0;
    return 1;
}
int st_fetch_user_profile(StClient *c,const char *id,StError *err) {
    if(!id_valid(id))return st_fail(err,"Invalid user ID");
    char path[160];cJSON *user=NULL,*profile=NULL,*mutual=NULL;snprintf(path,sizeof(path),"/users/%s",id);
    if(!st_http(c,"GET",path,NULL,NULL,&user,err))return 0;
    if(!cJSON_IsObject(user)||strcmp(st_str(user,"_id"),id)){cJSON_Delete(user);return st_fail(err,"Invalid user response");}
    snprintf(path,sizeof(path),"/users/%s/profile",id);
    if(!st_http(c,"GET",path,NULL,NULL,&profile,err)){if(err->status==403){profile=cJSON_CreateObject();cJSON_AddStringToObject(profile,"content","This profile is unavailable.");}else{cJSON_Delete(user);return 0;}}
    int self=!strcmp(id,c->self_id);
    if(self)mutual=cJSON_CreateObject();else {snprintf(path,sizeof(path),"/users/%s/mutual",id);if(!st_http(c,"GET",path,NULL,NULL,&mutual,err)){if(err->status==403)mutual=cJSON_CreateObject();else{cJSON_Delete(user);cJSON_Delete(profile);return 0;}}}
    if(!cJSON_IsObject(profile)||!cJSON_IsObject(mutual)){cJSON_Delete(user);cJSON_Delete(profile);cJSON_Delete(mutual);return st_fail(err,"Invalid profile response");}
    cJSON_Delete(c->profile_user);cJSON_Delete(c->profile);cJSON_Delete(c->mutual);c->profile_user=user;c->profile=profile;c->mutual=mutual;st_copy(c->profile_id,sizeof(c->profile_id),id);memset(err,0,sizeof(*err));return 1;
}
int st_fetch_user_profile_context(StClient *c,const char *id,const char *server,StError *err) {
    if(*server&&!id_valid(server))return st_fail(err,"Invalid profile server");
    if(!st_fetch_user_profile(c,id,err))return 0;
    if(!st_merge_user(c,c->profile_user,err))return 0;
    if(*server&&!st_member(c,server,id)) {
        char path[180];snprintf(path,sizeof(path),"/servers/%s/members/%s",server,id);cJSON *member=NULL;
        if(st_http(c,"GET",path,NULL,NULL,&member,err)) {
            const cJSON *key=cJSON_GetObjectItemCaseSensitive(member,"_id");
            if(strcmp(st_str(key,"server"),server)||strcmp(st_str(key,"user"),id)){cJSON_Delete(member);return st_fail(err,"Profile member identity mismatch");}
            cJSON *list=cJSON_CreateArray();if(!list){cJSON_Delete(member);return st_fail(err,"Out of memory");}
            cJSON_AddItemToArray(list,member);int ok=st_members_merge(c,server,list,0,err);cJSON_Delete(list);if(!ok)return 0;
        } else {cJSON_Delete(member);if(err->status!=403&&err->status!=404)return 0;memset(err,0,sizeof(*err));}
    }
    st_copy(c->profile_server,64,server);return 1;
}
int st_user_action(StClient *c,const char *id,const char *action,StError *err) {
    if(!id_valid(id)||!strcmp(id,c->self_id))return st_fail(err,"Invalid user action");
    const char *endpoint=NULL,*method=NULL;
    if(!strcmp(action,"friend")){endpoint="friend";method="PUT";}else if(!strcmp(action,"unfriend")){endpoint="friend";method="DELETE";}
    else if(!strcmp(action,"block")){endpoint="block";method="PUT";}else if(!strcmp(action,"unblock")){endpoint="block";method="DELETE";}
    if(!endpoint)return st_fail(err,"Unsupported user action");
    char path[160];snprintf(path,sizeof(path),"/users/%s/%s",id,endpoint);cJSON *user=NULL;
    if(!st_http(c,method,path,NULL,NULL,&user,err))return 0;
    if(!cJSON_IsObject(user)||strcmp(st_str(user,"_id"),id)){cJSON_Delete(user);return st_fail(err,"Action succeeded; refresh profile");}
    if(!st_merge_user(c,user,err)){cJSON_Delete(user);return 0;}
    if(!strcmp(c->profile_id,id)){cJSON_Delete(c->profile_user);c->profile_user=user;}else cJSON_Delete(user);
    return 1;
}
int st_open_dm(StClient *c,const char *id,StError *err) {
    if(!id_valid(id))return st_fail(err,"Invalid user ID");
    char path[160];snprintf(path,sizeof(path),"/users/%s/dm",id);cJSON *res=NULL;
    if(!st_http(c,"GET",path,NULL,NULL,&res,err))return 0;
    const char *cid=st_str(res,"_id"),*type=st_str(res,"channel_type");
    if(!id_valid(cid)||(strcmp(type,"DirectMessage")&&strcmp(type,"SavedMessages"))){cJSON_Delete(res);return st_fail(err,"Invalid DM response");}
    size_t i;for(i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,cid))break;
    if(i==c->channel_count){if(i>=2048){cJSON_Delete(res);return st_fail(err,"Channel limit exceeded");}StChannel *p=realloc(c->channels,(i+1)*sizeof(*p));if(!p){cJSON_Delete(res);return st_fail(err,"Out of memory");}c->channels=p;c->channel_count++;}
    StChannel *ch=c->channels+i;memset(ch,0,sizeof(*ch));st_copy(ch->id,64,cid);st_copy(ch->type,sizeof(ch->type),type);st_copy(ch->peer,64,id);st_copy(c->opened_dm,64,cid);cJSON_Delete(res);return 1;
}
int st_report_user(StClient *c,const char *id,const char *reason,const char *text,StError *err) {
    static const char *const reasons[]={"UnsolicitedSpam","SpamAbuse","InappropriateProfile","Impersonation","BanEvasion","Underage","NoneSpecified"};
    int valid=0;for(size_t i=0;i<sizeof(reasons)/sizeof(*reasons);i++)if(!strcmp(reason,reasons[i]))valid=1;
    size_t count;if(!id_valid(id)||!strcmp(id,c->self_id)||!valid||!st_utf8_count(text,&count)||count>2000||(!strcmp(reason,"NoneSpecified")&&!*text))return st_fail(err,"Invalid report; Other requires a description");
    cJSON *body=cJSON_CreateObject(),*res=NULL;if(!body)return st_fail(err,"Out of memory");cJSON *content=cJSON_AddObjectToObject(body,"content");
    cJSON_AddStringToObject(content,"type","User");cJSON_AddStringToObject(content,"id",id);cJSON_AddStringToObject(content,"report_reason",reason);cJSON_AddStringToObject(body,"additional_context",text);
    int ok=st_http(c,"POST","/safety/report",body,NULL,&res,err);cJSON_Delete(body);cJSON_Delete(res);return ok;
}
int st_fetch_sessions(StClient *c,StError *err) {
    cJSON *res=NULL;if(!st_http(c,"GET","/auth/session/all",NULL,NULL,&res,err))return 0;
    int count=cJSON_GetArraySize(res);if(!cJSON_IsArray(res)||count>512){cJSON_Delete(res);return st_fail(err,"Invalid session list");}
    StSession *sessions=calloc(count?count:1,sizeof(*sessions));if(!sessions){cJSON_Delete(res);return st_fail(err,"Out of memory");}
    const cJSON *s;int i=0;cJSON_ArrayForEach(s,res){if(!id_valid(st_str(s,"_id"))){free(sessions);cJSON_Delete(res);return st_fail(err,"Invalid session ID");}st_copy(sessions[i].id,64,st_str(s,"_id"));st_copy(sessions[i++].name,257,st_str(s,"name"));}
    free(c->sessions);c->sessions=sessions;c->session_count=(size_t)count;cJSON_Delete(res);return 1;
}
int st_revoke_sessions(StClient *c,const char *id,int all,const char *password,StError *err) {
    if(!*password||(!all&&!id_valid(id))||all<0||all>2)return st_fail(err,"Password is required to revoke sessions");
    cJSON *body=cJSON_CreateObject(),*res=NULL;if(!body)return st_fail(err,"Out of memory");cJSON_AddStringToObject(body,"password",password);
    int ok=st_http(c,"PUT","/auth/mfa/ticket",body,NULL,&res,err);st_wipe((void *)st_str(body,"password"),strlen(password));cJSON_Delete(body);
    if(!ok){cJSON_Delete(res);return 0;}char ticket[256];st_copy(ticket,sizeof(ticket),st_str(res,"token"));st_wipe((void *)st_str(res,"token"),strlen(st_str(res,"token")));cJSON_Delete(res);res=NULL;
    if(!ticket[0])return st_fail(err,"Invalid reauthentication response");
    char path[160];if(all)snprintf(path,sizeof(path),"/auth/session/all?revoke_self=%s",all==2?"true":"false");else snprintf(path,sizeof(path),"/auth/session/%s",id);
    ok=st_http_ticket(c,"DELETE",path,NULL,ticket,&res,err);st_wipe(ticket,sizeof(ticket));cJSON_Delete(res);
    if(ok&&(all==2||(!all&&!strcmp(id,c->session_id))))st_client_clear(c);
    return ok;
}
