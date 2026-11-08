/* Reconcile messages missed while normal API polling was paused. This runs
 * after voice teardown, never concurrently with audio/TLS voice work. */
#include "call_api.h"
#include "notices.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int by_id(const void *a,const void *b){return strcmp(st_str(*(const cJSON *const *)a,"_id"),st_str(*(const cJSON *const *)b,"_id"));}
int st_call_catchup(StClient *c,const cJSON *baseline,cJSON **out,int *notify,StError *error) {
    *out=NULL;*notify=0;StNotices notices={0};cJSON *local=cJSON_CreateObject();if(!local)return st_fail(error,"Out of memory");
    cJSON_AddItemToObject(local,"local",cJSON_Duplicate(baseline,1));st_notices_import(&notices,c,local);cJSON_Delete(local);
    if(!st_bootstrap(c,error)){st_notices_clear(&notices);return 0;}
    unsigned requests=0,notified_count=0;char notified[6][64];
    for(size_t i=0;i<c->channel_count&&!st_cancelled();i++) {
        const StChannel *channel=c->channels+i;const char *after="";for(size_t j=0;j<notices.count;j++)if(!strcmp(notices.items[j].channel,channel->id)){after=notices.items[j].last;break;}
        if(!*channel->last_message||strcmp(channel->last_message,after)<=0||requests>=6)continue;
        char path[256];snprintf(path,sizeof(path),"/channels/%s/messages?limit=100&sort=Oldest%s%s",channel->id,*after?"&after=":"",after);cJSON *messages=NULL;StError ignored={0};requests++;
        unsigned sounds=notices.sounds;
        if(st_http(c,"GET",path,NULL,NULL,&messages,&ignored)) {
            cJSON *array=cJSON_IsArray(messages)?messages:cJSON_GetObjectItemCaseSensitive(messages,"messages");int count=cJSON_GetArraySize(array);const cJSON *ordered[100];if(count>100)count=100;
            for(int j=0;j<count;j++)ordered[j]=cJSON_GetArrayItem(array,j);
            qsort(ordered,(size_t)count,sizeof(*ordered),by_id);
            for(int j=0;j<count;j++){cJSON *message=cJSON_Duplicate(ordered[j],1);if(message){cJSON_DeleteItemFromObjectCaseSensitive(message,"type");cJSON_AddStringToObject(message,"type","Message");st_notices_event(&notices,c,message);cJSON_Delete(message);}}
        }
        if(notices.sounds!=sounds&&notified_count<6)st_copy(notified[notified_count++],64,channel->id);
        cJSON_Delete(messages);
    }
    st_notices_sync(&notices,c);cJSON *server=NULL;StError ignored={0};
    if(!st_cancelled()&&st_http(c,"GET","/sync/unreads",NULL,NULL,&server,&ignored)) {cJSON *root=cJSON_CreateObject();cJSON_AddItemToObject(root,"server",server);server=NULL;st_notices_import(&notices,c,root);cJSON_Delete(root);}cJSON_Delete(server);
    for(size_t i=0;i<notices.count;i++) {
        const StNotice *item=notices.items+i;const char *last="";unsigned count=0;
        const cJSON *row;cJSON_ArrayForEach(row,baseline) {
            const cJSON *key=cJSON_GetArrayItem(row,0);if(!cJSON_IsString(key)||strcmp(key->valuestring,item->channel))continue;
            const cJSON *v=cJSON_GetArrayItem(row,1),*n=cJSON_GetArrayItem(row,3);last=cJSON_IsString(v)?v->valuestring:"";count=cJSON_IsNumber(n)?(unsigned)n->valuedouble:0;break;
        }
        for(unsigned j=0;j<notified_count;j++)if(!strcmp(notified[j],item->channel)&&strcmp(item->last,last)>0&&(item->count>count||item->unknown))*notify=1;
    }
    *out=st_notices_export(&notices);st_notices_clear(&notices);if(!*out)return st_fail(error,"Out of memory");return !st_cancelled();
}
