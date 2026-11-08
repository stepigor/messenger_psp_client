#include "notices.h"
#include "preferences.h"
#include "disk_cache.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static int valid(const char *s){size_t n=strlen(s);return n>0&&n<64&&strspn(s,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")==n;}
static StNotice *find(const StNotices *n,const char *channel){for(size_t i=0;i<n->count;i++)if(!strcmp(n->items[i].channel,channel))return n->items+i;return NULL;}
static const StChannel *channel_for(const StClient *c,const char *id){for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,id))return c->channels+i;return NULL;}
void st_notices_clear(StNotices *n){free(n->items);memset(n,0,sizeof(*n));}
void st_notices_sync(StNotices *n,const StClient *c) {
    for(size_t i=0;i<n->count;)if(!channel_for(c,n->items[i].channel)){memmove(n->items+i,n->items+i+1,(n->count-i-1)*sizeof(*n->items));n->count--;n->revision++;}else i++;
    for(size_t i=0;i<c->channel_count&&n->count<ST_NOTICE_MAX;i++) {
        const StChannel *ch=c->channels+i;StNotice *item=find(n,ch->id);
        if(!item&&valid(ch->id)) {
            StNotice *all=realloc(n->items,(n->count+1)*sizeof(*all));if(!all)return;n->items=all;item=n->items+n->count++;memset(item,0,sizeof(*item));st_copy(item->channel,64,ch->id);
            st_copy(item->last,64,ch->last_message);st_copy(item->read,64,ch->last_message);
        }else if(item&&valid(ch->last_message)&&strcmp(ch->last_message,item->last)>0) {
            st_copy(item->last,64,ch->last_message);item->unknown=1;n->revision++;
        }
    }
}
void st_notices_read(StNotices *n,const char *channel,const char *last) {
    StNotice *item=find(n,channel);if(!item||!valid(last)||strcmp(last,item->read)<=0)return;
    st_copy(item->read,64,last);item->ack=1;
    if(strcmp(last,item->last)>=0){st_copy(item->last,64,last);item->count=0;item->unknown=0;}else {item->count=0;item->unknown=1;}
    n->revision++;
}
void st_notices_event(StNotices *n,const StClient *c,const cJSON *e) {
    const char *type=st_str(e,"type");
    if(!strcmp(type,"ChannelAck")) {
        if(strcmp(st_str(e,"user"),c->self_id))return;
        StNotice *item=find(n,st_str(e,"id"));const char *id=st_str(e,"message_id");
        if(item&&valid(id)&&strcmp(id,item->read)>0){st_notices_read(n,item->channel,id);item->ack=0;}return;
    }
    if(strcmp(type,"Message"))return;
    const char *channel=st_str(e,"channel"),*id=st_str(e,"_id"),*author=st_str(e,"author");
    StNotice *item=find(n,channel);
    if(!item&&valid(channel)&&valid(id)&&n->count<ST_NOTICE_MAX) {
        StNotice *all=realloc(n->items,(n->count+1)*sizeof(*all));if(!all)return;n->items=all;item=n->items+n->count++;memset(item,0,sizeof(*item));st_copy(item->channel,64,channel);
    }
    if(!item||!valid(id)||strcmp(id,item->last)<=0)return;
    st_copy(item->last,64,id);n->revision++;
    const StChannel *ch=channel_for(c,channel);
    if(!strcmp(author,c->self_id)||(ch&&!strcmp(ch->type,"SavedMessages")))return;
    for(size_t i=0;i<c->user_count;i++)if(!strcmp(c->users[i].id,author)&&!strcmp(c->users[i].relationship,"Blocked"))return;
    if(item->count<UINT_MAX)item->count++;
    if(strcmp(n->visible,channel)&&st_preferences_notify(c,e))n->sounds++;
}
unsigned st_notices_channel(const StNotices *n,const char *channel,int *unknown) {
    StNotice *item=n?find(n,channel):NULL;if(unknown)*unknown=item?item->unknown:0;return item?item->count:0;
}
unsigned st_notices_total(const StNotices *n,const StClient *c,const char *server,int *unknown) {
    unsigned sum=0;int any=0;
    for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].server,server)) {
        int dot=0;unsigned count=st_notices_channel(n,c->channels[i].id,&dot);any|=dot;sum=UINT_MAX-sum<count?UINT_MAX:sum+count;
    }
    if(unknown)*unknown=any;
    return sum;
}
cJSON *st_notices_export(const StNotices *n) {
    cJSON *a=cJSON_CreateArray();if(!a)return NULL;
    for(size_t i=0;i<n->count;i++) {
        const StNotice *item=n->items+i;cJSON *o=cJSON_CreateArray();if(!o){cJSON_Delete(a);return NULL;}
        cJSON_AddItemToArray(o,cJSON_CreateString(item->channel));cJSON_AddItemToArray(o,cJSON_CreateString(item->last));cJSON_AddItemToArray(o,cJSON_CreateString(item->read));
        cJSON_AddItemToArray(o,cJSON_CreateNumber(item->count));cJSON_AddItemToArray(o,cJSON_CreateBool(item->unknown));cJSON_AddItemToArray(o,cJSON_CreateBool(item->ack));cJSON_AddItemToArray(a,o);
    }return a;
}
void st_notices_import(StNotices *n,const StClient *c,const cJSON *root) {
    const cJSON *row;st_notices_sync(n,c);
    cJSON_ArrayForEach(row,cJSON_GetObjectItemCaseSensitive(root,"local")) {
        int compact=cJSON_IsArray(row);
        const cJSON *channel=compact?cJSON_GetArrayItem(row,0):cJSON_GetObjectItemCaseSensitive(row,"channel"),*last_node=compact?cJSON_GetArrayItem(row,1):cJSON_GetObjectItemCaseSensitive(row,"last"),*read_node=compact?cJSON_GetArrayItem(row,2):cJSON_GetObjectItemCaseSensitive(row,"read");
        if(!cJSON_IsString(channel)||!cJSON_IsString(last_node)||!cJSON_IsString(read_node))continue;
        StNotice *item=find(n,channel->valuestring);const char *last=last_node->valuestring,*read=read_node->valuestring;
        const cJSON *count=compact?cJSON_GetArrayItem(row,3):cJSON_GetObjectItemCaseSensitive(row,"count");
        if(!item||!valid(last)||(*read&&!valid(read))||strcmp(read,last)>0||!cJSON_IsNumber(count)||count->valuedouble<0||count->valuedouble>UINT_MAX)continue;
        /* Import happens before polling live events for this account. */
        char current[64];st_copy(current,64,item->last);st_copy(item->last,64,last);st_copy(item->read,64,read);item->restored=1;item->count=(unsigned)count->valuedouble;item->unknown=cJSON_IsTrue(compact?cJSON_GetArrayItem(row,4):cJSON_GetObjectItemCaseSensitive(row,"unknown"));item->ack=compact&&cJSON_IsTrue(cJSON_GetArrayItem(row,5));
        if(strcmp(current,last)>0){st_copy(item->last,64,current);item->unknown=1;}
    }
    cJSON_ArrayForEach(row,cJSON_GetObjectItemCaseSensitive(root,"server")) {
        const cJSON *key=cJSON_GetObjectItemCaseSensitive(row,"_id");if(strcmp(st_str(key,"user"),c->self_id))continue;
        StNotice *item=find(n,st_str(key,"channel"));const char *read=st_str(row,"last_id");if(!item||(*read&&!valid(read)))continue;
        if(!item->restored){st_copy(item->read,64,read);item->count=0;item->unknown=strcmp(item->last,read)>0;}
        else if(strcmp(read,item->read)>0){st_notices_read(n,item->channel,read);item->ack=0;}
        if(strcmp(item->last,item->read)>0&&!item->count)item->unknown=1;
    }
    n->loaded=1;n->revision++;
}
static void cache_key(const StClient *c,char out[700]){snprintf(out,700,"unreads-v1:%s:%s",c->config.api_url,c->self_id);}
int st_notices_load(StClient *c,cJSON **out,StError *err) {
    *out=cJSON_CreateObject();if(!*out)return st_fail(err,"Out of memory");
    char key[700];cache_key(c,key);StDiskEntry entry;
    if(st_disk_read(ST_DISK_DIR,key,&entry)){cJSON *local=cJSON_ParseWithLength((const char *)entry.data,entry.length);if(cJSON_IsArray(local)&&cJSON_GetArraySize(local)<=ST_NOTICE_MAX)cJSON_AddItemToObject(*out,"local",local);else cJSON_Delete(local);st_disk_free(&entry);}
    cJSON *server=NULL;StError ignored={0};
    if(!st_cancelled()&&st_http(c,"GET","/sync/unreads",NULL,NULL,&server,&ignored)&&cJSON_IsArray(server))cJSON_AddItemToObject(*out,"server",server);else cJSON_Delete(server);
    if(ignored.status==401){*err=ignored;cJSON_Delete(*out);*out=NULL;return 0;}
    if(st_cancelled()){cJSON_Delete(*out);*out=NULL;return st_fail(err,"Cancelled");}return 1;
}
int st_notices_save(const StClient *c,const cJSON *snapshot,StError *err) {
    char *text=cJSON_PrintUnformatted(snapshot);if(!text)return st_fail(err,"Out of memory");
    char key[700];cache_key(c,key);int ok=st_disk_write(ST_DISK_DIR,key,(unsigned char *)text,strlen(text),NULL,err);cJSON_free(text);return ok;
}
int st_notices_ack(StClient *c,const char *channel,const char *id,StError *err) {
    if(!valid(channel)||!valid(id))return st_fail(err,"Invalid read marker");
    char path[180];snprintf(path,sizeof(path),"/channels/%s/ack/%s",channel,id);
    cJSON *response=NULL;int ok=st_http(c,"PUT",path,NULL,NULL,&response,err);cJSON_Delete(response);return ok;
}
