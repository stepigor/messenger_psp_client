#include "preferences.h"
#include "disk_cache.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static int valid(const char *id){size_t n=strlen(id);return n&&n<64&&strspn(id,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")==n;}
static void identity(const StClient *c,char key[700]){snprintf(key,700,"notification-options:%s:%s",c->config.api_url,c->self_id);}
static void cache(const StClient *c){char key[700];identity(c,key);char *s=cJSON_PrintUnformatted(c->notifications);if(s){StError ignored={0};st_disk_write(ST_DISK_DIR,key,(const unsigned char *)s,strlen(s),NULL,&ignored);cJSON_free(s);}}
static cJSON *decode(const cJSON *root){
    if(!cJSON_IsObject(root))return NULL;
    const cJSON *v=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"notifications"),1);
    if(!v)return cJSON_CreateObject();
    if(!cJSON_IsString(v)||strlen(v->valuestring)>128*1024)return NULL;
    cJSON *o=cJSON_Parse(v->valuestring);if(!cJSON_IsObject(o)){cJSON_Delete(o);return NULL;}return o;
}
int st_preferences_fetch(StClient *c,StError *err){
    cJSON *body=cJSON_Parse("{\"keys\":[\"notifications\"]}"),*r=NULL;if(!body)return st_fail(err,"Out of memory");
    int ok=st_http(c,"POST","/sync/settings/fetch",body,NULL,&r,err);cJSON_Delete(body);
    cJSON *o=ok?decode(r):NULL;cJSON_Delete(r);
    if(ok&&!o)return st_fail(err,"Invalid notification settings");
    if(!ok&&!c->notifications){char key[700];identity(c,key);StDiskEntry e;if(st_disk_read(ST_DISK_DIR,key,&e)){o=cJSON_ParseWithLength((const char *)e.data,e.length);st_disk_free(&e);if(!cJSON_IsObject(o)){cJSON_Delete(o);o=NULL;}}}
    if(o){cJSON_Delete(c->notifications);c->notifications=o;if(ok)cache(c);}return ok;
}
enum StNotify st_preferences_mode(const StClient *c,const char *id,int server){
    const char *mode=st_str(cJSON_GetObjectItemCaseSensitive(c->notifications,server?"server":"channel"),id);
    if(!strcmp(mode,"all"))return ST_NOTIFY_ALL;
    if(!strcmp(mode,"mention"))return ST_NOTIFY_MENTION;
    if(!strcmp(mode,"none")||!strcmp(mode,"muted"))return ST_NOTIFY_NONE;
    if(server)return ST_NOTIFY_MENTION;
    for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,id)&&c->channels[i].server[0])return st_preferences_mode(c,c->channels[i].server,1);
    return ST_NOTIFY_ALL;
}
int st_preferences_set(StClient *c,const char *id,int server,enum StNotify mode,StError *err){
    if(!valid(id)||mode<0||mode>ST_NOTIFY_NONE)return st_fail(err,"Invalid notification setting");
    /* Fetch before merging: preserve other devices' targets and mute options. */
    if(!st_preferences_fetch(c,err))return 0;
    cJSON *o=cJSON_Duplicate(c->notifications,1);if(!o)return st_fail(err,"Out of memory");
    const char *section=server?"server":"channel";cJSON *map=cJSON_GetObjectItemCaseSensitive(o,section);
    if(!cJSON_IsObject(map)){cJSON_DeleteItemFromObjectCaseSensitive(o,section);map=cJSON_AddObjectToObject(o,section);}
    static const char *values[]={"all","mention","none"};
    cJSON_DeleteItemFromObjectCaseSensitive(map,id);if(!cJSON_AddStringToObject(map,id,values[mode])){cJSON_Delete(o);return st_fail(err,"Out of memory");}
    /* Choosing a state explicitly also unmutes this target's desktop mute. */
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(o,server?"server_mutes":"channel_mutes"),id);
    char *s=cJSON_PrintUnformatted(o);cJSON *body=cJSON_CreateObject(),*r=NULL;
    if(!s||!body||!cJSON_AddStringToObject(body,"notifications",s)){cJSON_free(s);cJSON_Delete(body);cJSON_Delete(o);return st_fail(err,"Out of memory");}
    int ok=st_http(c,"POST","/sync/settings/set",body,NULL,&r,err);cJSON_free(s);cJSON_Delete(body);cJSON_Delete(r);
    if(ok){cJSON_Delete(c->notifications);c->notifications=o;cache(c);}else cJSON_Delete(o);return ok;
}
static int muted(const StClient *c,const char *id,int server){
    const cJSON *m=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c->notifications,server?"server_mutes":"channel_mutes"),id);
    if(!cJSON_IsObject(m))return 0;
    const cJSON *until=cJSON_GetObjectItemCaseSensitive(m,"until");
    return !cJSON_IsNumber(until)||until->valuedouble>(double)time(NULL)*1000;
}
int st_preferences_notify(const StClient *c,const cJSON *m){
    const char *id=st_str(m,"channel");const StChannel *ch=NULL;
    for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,id)){ch=c->channels+i;break;}
    if(muted(c,id,0)||(ch&&ch->server[0]&&muted(c,ch->server,1)))return 0;
    const cJSON *flags=cJSON_GetObjectItemCaseSensitive(m,"flags");if(cJSON_IsNumber(flags)&&(flags->valueint&1))return 0;
    enum StNotify mode=st_preferences_mode(c,id,0);if(mode!=ST_NOTIFY_MENTION)return mode==ST_NOTIFY_ALL;
    const cJSON *u;cJSON_ArrayForEach(u,cJSON_GetObjectItemCaseSensitive(m,"mentions"))if(cJSON_IsString(u)&&!strcmp(u->valuestring,c->self_id))return 1;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m,"mention_everyone"))||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m,"mention_online")))return 1;
    const cJSON *roles=ch?cJSON_GetObjectItemCaseSensitive(c->notification_roles,ch->server):NULL,*role;
    cJSON_ArrayForEach(u,cJSON_GetObjectItemCaseSensitive(m,"role_mentions"))cJSON_ArrayForEach(role,roles)if(cJSON_IsString(u)&&cJSON_IsString(role)&&!strcmp(u->valuestring,role->valuestring))return 1;
    return 0;
}
static void membership(StClient *c,const cJSON *m){
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(m,"_id");const char *server=st_str(id,"server");
    if(strcmp(st_str(id,"user"),c->self_id)||!valid(server))return;
    const cJSON *roles=cJSON_GetObjectItemCaseSensitive(m,"roles");if(!cJSON_IsArray(roles)||cJSON_GetArraySize(roles)>256)return;
    cJSON *copy=cJSON_Duplicate(roles,1);if(!copy)return;
    if(!c->notification_roles)c->notification_roles=cJSON_CreateObject();
    if(!c->notification_roles){cJSON_Delete(copy);return;}
    cJSON_DeleteItemFromObjectCaseSensitive(c->notification_roles,server);cJSON_AddItemToObject(c->notification_roles,server,copy);
}
void st_preferences_memberships(StClient *c,const cJSON *members){
    if(!cJSON_IsArray(members))return;
    cJSON_Delete(c->notification_roles);c->notification_roles=NULL;const cJSON *m;cJSON_ArrayForEach(m,members)membership(c,m);
}
int st_preferences_event(StClient *c,const cJSON *e){
    if(!strcmp(st_str(e,"type"),"ServerMemberUpdate")){
        const cJSON *data=cJSON_GetObjectItemCaseSensitive(e,"data"),*id=cJSON_GetObjectItemCaseSensitive(e,"id");
        cJSON *m=cJSON_IsObject(data)?cJSON_Duplicate(data,1):cJSON_CreateObject();if(m){
            cJSON_DeleteItemFromObjectCaseSensitive(m,"_id");cJSON_AddItemToObject(m,"_id",cJSON_Duplicate(id,1));
            const cJSON *clear;cJSON_ArrayForEach(clear,cJSON_GetObjectItemCaseSensitive(e,"clear"))if(cJSON_IsString(clear)&&!strcmp(clear->valuestring,"Roles")){cJSON_DeleteItemFromObjectCaseSensitive(m,"roles");cJSON_AddArrayToObject(m,"roles");}
            membership(c,m);cJSON_Delete(m);
        }return 0;
    }
    if(!strcmp(st_str(e,"type"),"ServerMemberJoin"))membership(c,cJSON_GetObjectItemCaseSensitive(e,"member"));
    if(!strcmp(st_str(e,"type"),"ServerDelete")||(!strcmp(st_str(e,"type"),"ServerMemberLeave")&&!strcmp(st_str(e,"user"),c->self_id)))cJSON_DeleteItemFromObjectCaseSensitive(c->notification_roles,st_str(e,"id"));
    if(strcmp(st_str(e,"type"),"UserSettingsUpdate")||strcmp(st_str(e,"id"),c->self_id))return 0;
    const cJSON *settings=cJSON_GetObjectItemCaseSensitive(e,"update");if(!cJSON_HasObjectItem(settings,"notifications"))return 0;
    cJSON *o=decode(settings);if(!o)return 0;cJSON_Delete(c->notifications);c->notifications=o;return 1;
}
