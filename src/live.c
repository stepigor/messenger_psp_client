#include "live.h"
#include "preferences.h"
#include <stdio.h>
#include <string.h>
void st_live_channel(StLive *l,const char *channel) {
    if(!strcmp(l->channel,channel))return;
    st_copy(l->channel,sizeof(l->channel),channel);
    memset(l->typing,0,sizeof(l->typing));l->revision=l->applied=0;
}
static void event(StLive *l,StClient *c,const cJSON *e,uint64_t now,int depth,int *budget) {
    if(depth>8||*budget<=0){if(l->channel[0])l->revision++;l->lists_dirty=1;return;}
    --*budget;
    const char *type=st_str(e,"type");
    st_notices_event(&l->notices,c,e);
    st_preferences_event(c,e);
    if(!strcmp(type,"Bulk")){const cJSON *v;cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(e,"v"))event(l,c,v,now,depth+1,budget);return;}
    if(!strcmp(type,"Ready")){StError error={0};if(st_load_ready(c,e,&error)){st_notices_sync(&l->notices,c);l->connected=1;l->lists_dirty=0;if(l->channel[0])l->revision++;}return;}
    if(!strcmp(type,"Logout")){l->logged_out=1;return;}
    const char *channel=st_str(e,!strncmp(type,"Message",7)&&strstr(type,"React")?"channel_id":"channel");
    if(!strcmp(type,"MessageUnreact"))channel=st_str(e,"channel_id");
    if(!strcmp(channel,c->messages_channel)) {
        if(!strcmp(type,"MessageUpdate")||!strcmp(type,"MessageAppend")||!strcmp(type,"MessageDelete"))st_quotes_invalidate(c,st_str(e,"id"),!strcmp(type,"MessageDelete"));
        if(!strcmp(type,"BulkMessageDelete")){const cJSON *id;cJSON_ArrayForEach(id,cJSON_GetObjectItemCaseSensitive(e,"ids"))if(cJSON_IsString(id))st_quotes_invalidate(c,id->valuestring,1);}
    }
    if((!strcmp(type,"Message")||!strcmp(type,"MessageUpdate")||!strcmp(type,"MessageAppend")||!strcmp(type,"MessageDelete")||!strcmp(type,"MessageReact")||!strcmp(type,"MessageUnreact")||!strcmp(type,"MessageRemoveReaction")||!strcmp(type,"BulkMessageDelete"))&&l->channel[0]&&!strcmp(channel,l->channel))l->revision++;
    if(!strcmp(type,"Message")&&!strcmp(channel,l->channel))for(int i=0;i<ST_TYPING_MAX;i++)if(!strcmp(l->typing[i].user,st_str(e,"author")))memset(l->typing+i,0,sizeof(l->typing[i]));
    if(!strcmp(type,"ChannelStartTyping")||!strcmp(type,"ChannelStopTyping")) {
        const char *user=st_str(e,"user");if(strcmp(st_str(e,"id"),l->channel)||!*user||!strcmp(user,c->self_id))return;
        int slot=-1;for(int i=0;i<ST_TYPING_MAX;i++)if(!strcmp(l->typing[i].user,user)){slot=i;break;}
        if(!strcmp(type,"ChannelStopTyping")){if(slot>=0)memset(l->typing+slot,0,sizeof(l->typing[0]));return;}
        if(slot<0)for(int i=0;i<ST_TYPING_MAX;i++)if(l->typing[i].until<=now){slot=i;break;}
        if(slot>=0){st_copy(l->typing[slot].user,64,user);l->typing[slot].until=now+8000;}return;
    }
    if(!strcmp(type,"UserRelationship")) {
        const cJSON *user=cJSON_GetObjectItemCaseSensitive(e,"user");StError error={0};st_merge_user(c,user,&error);
        if(!strcmp(c->profile_id,st_str(user,"_id"))){cJSON *copy=cJSON_Duplicate(user,1);if(copy){cJSON_Delete(c->profile_user);c->profile_user=copy;}}return;
    }
    if(!strcmp(type,"UserUpdate")) {
        const cJSON *data=cJSON_GetObjectItemCaseSensitive(e,"data");cJSON *user=cJSON_Duplicate(data,1);StError error={0};
        if(user){const cJSON *clear;cJSON_ArrayForEach(clear,cJSON_GetObjectItemCaseSensitive(e,"clear"))if(cJSON_IsString(clear)&&!strcmp(clear->valuestring,"Pronouns")){cJSON_DeleteItemFromObjectCaseSensitive(user,"pronouns");cJSON_AddNullToObject(user,"pronouns");}
            cJSON_AddStringToObject(user,"_id",st_str(e,"id"));st_merge_user(c,user,&error);cJSON_Delete(user);}return;
    }
    if(!strcmp(type,"ServerMemberUpdate")) {
        const cJSON *id=cJSON_GetObjectItemCaseSensitive(e,"id");
        if(!strcmp(st_str(id,"server"),c->members_server)) {
            l->members_dirty=1;
            cJSON *m=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(e,"data"),1),*list=cJSON_CreateArray();
            if(m&&list) {
                cJSON_DeleteItemFromObjectCaseSensitive(m,"_id");cJSON_AddItemToObject(m,"_id",cJSON_Duplicate(id,1));
                const cJSON *clear;cJSON_ArrayForEach(clear,cJSON_GetObjectItemCaseSensitive(e,"clear"))if(cJSON_IsString(clear)) {
                    const char *field=!strcmp(clear->valuestring,"Pronouns")?"pronouns":!strcmp(clear->valuestring,"Nickname")?"nickname":!strcmp(clear->valuestring,"Roles")?"roles":NULL;
                    if(field){cJSON_DeleteItemFromObjectCaseSensitive(m,field);if(!strcmp(field,"roles"))cJSON_AddArrayToObject(m,field);else cJSON_AddNullToObject(m,field);}
                }
                cJSON_AddItemToArray(list,m);StError error={0};if(!st_members_merge(c,c->members_server,list,ST_MEMBERS_PATCH,&error))l->members_dirty=1;
            }else cJSON_Delete(m);
            cJSON_Delete(list);
        }
        return;
    }
    if(!strcmp(type,"ServerMemberJoin")||!strcmp(type,"ServerMemberLeave"))l->members_dirty=1;
    if(!strcmp(type,"ServerMemberLeave")) {
        st_member_remove(c,st_str(e,"id"),st_str(e,"user"));
        for(size_t k=0;k<c->channel_count;k++)if(!strcmp(c->channels[k].id,c->participants_channel)&&!strcmp(c->channels[k].server,st_str(e,"id")))
            for(size_t i=0;i<c->participant_count;i++)if(!strcmp(c->participants[i].id,st_str(e,"user"))){memmove(c->participants+i,c->participants+i+1,(c->participant_count-i-1)*sizeof(*c->participants));c->participant_count--;break;}
    }
    if(!strcmp(type,"ServerDelete")||(!strcmp(type,"ServerMemberLeave")&&!strcmp(st_str(e,"user"),c->self_id))){st_remove_server(c,st_str(e,"id"));st_notices_sync(&l->notices,c);return;}
    if(!strcmp(type,"ServerUpdate")||!strncmp(type,"ServerRole",10))l->members_dirty=1;
    if(!strcmp(type,"ServerCreate")||!strcmp(type,"ServerUpdate")||!strncmp(type,"ServerRole",10)||!strcmp(type,"ChannelCreate")||!strcmp(type,"ChannelUpdate")||!strcmp(type,"ChannelDelete")||!strncmp(type,"VoiceChannel",12)||!strcmp(type,"UserVoiceStateUpdate")||!strcmp(type,"EmojiCreate")||!strcmp(type,"EmojiDelete"))l->lists_dirty=1;
}
void st_live_events(StLive *l,StClient *c,const cJSON *events,uint64_t now) {
    int budget=512;const cJSON *e;cJSON_ArrayForEach(e,events)event(l,c,e,now,0,&budget);
    st_participants_sort(c);
}
void st_live_typing_text(const StLive *l,const StClient *c,uint64_t now,char *out,size_t cap) {
    const char *names[2]={0};int n=0;for(int i=0;i<ST_TYPING_MAX;i++)if(l->typing[i].until>now) {
        if(n<2){const char *name=st_user_name(c,l->typing[i].user);if(!strcmp(name,l->typing[i].user)&&!strcmp(c->participants_channel,l->channel))for(size_t k=0;k<c->participant_count;k++)if(!strcmp(c->participants[k].id,l->typing[i].user))name=c->participants[k].name;
            names[n]=!strcmp(name,l->typing[i].user)?"Someone":name;}n++;
    }
    if(!n){if(cap)*out=0;return;}
    if(n==1)snprintf(out,cap,"%s is typing...",names[0]);else if(n==2)snprintf(out,cap,"%s and %s are typing...",names[0],names[1]);else snprintf(out,cap,"Several people are typing...");
}
