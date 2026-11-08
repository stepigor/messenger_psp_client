#include "jobs.h"
#include "preferences.h"
#include "photos.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int clone(StClient *out,const StClient *c,enum StJobType type,StError *err) {
    *out=*c;out->notifications=NULL;out->notification_roles=NULL;out->users=NULL;out->servers=NULL;out->channels=NULL;out->messages=NULL;out->participants=NULL;out->self=NULL;out->profile=NULL;out->profile_user=NULL;out->mutual=NULL;out->sessions=NULL;out->emojis=NULL;out->voice=NULL;out->invitation=NULL;out->roles=NULL;out->members=NULL;out->member_roles=NULL;
    if(type==ST_JOB_LEAVE_GROUP||type==ST_JOB_DELETE_MESSAGE||type==ST_JOB_THUMBNAIL||type==ST_JOB_PREFS_FETCH||type==ST_JOB_PREFS_SET||type==ST_JOB_PHOTO||type==ST_JOB_CALL_PREPARE||type==ST_JOB_CALL_AUTH||type==ST_JOB_MENTION||type==ST_JOB_SOUND||type==ST_JOB_NOTICES||type==ST_JOB_NOTICES_SAVE||type==ST_JOB_ACK||type==ST_JOB_QUOTE||type==ST_JOB_LIVE_CLOSE||type==ST_JOB_LIVE||type==ST_JOB_MEDIA||type==ST_JOB_CACHE_STATS||type==ST_JOB_CACHE_CLEAR) {
        out->user_count=out->server_count=out->channel_count=out->message_count=out->participant_count=out->session_count=out->emoji_count=out->voice_count=out->role_count=out->member_count=out->member_role_count=0;out->members_server[0]=0;return 1;
    }
#define COPY_ARRAY(field,count) do{if(c->count){out->field=malloc(c->count*sizeof(*c->field));if(!out->field)goto failed;memcpy(out->field,c->field,c->count*sizeof(*c->field));}}while(0)
    COPY_ARRAY(users,user_count);COPY_ARRAY(servers,server_count);COPY_ARRAY(channels,channel_count);COPY_ARRAY(messages,message_count);COPY_ARRAY(participants,participant_count);COPY_ARRAY(sessions,session_count);COPY_ARRAY(emojis,emoji_count);COPY_ARRAY(voice,voice_count);COPY_ARRAY(roles,role_count);COPY_ARRAY(members,member_count);COPY_ARRAY(member_roles,member_role_count);
#undef COPY_ARRAY
    if(c->notification_roles&&!(out->notification_roles=cJSON_Duplicate(c->notification_roles,1)))goto failed;
    if(c->notifications&&!(out->notifications=cJSON_Duplicate(c->notifications,1)))goto failed;
    if(c->invitation&&!(out->invitation=cJSON_Duplicate(c->invitation,1)))goto failed;
    if(c->self&&!(out->self=cJSON_Duplicate(c->self,1)))goto failed;
    if(c->profile&&!(out->profile=cJSON_Duplicate(c->profile,1)))goto failed;
    if(c->profile_user&&!(out->profile_user=cJSON_Duplicate(c->profile_user,1)))goto failed;
    if(c->mutual&&!(out->mutual=cJSON_Duplicate(c->mutual,1)))goto failed;
    return 1;
failed:st_client_clear(out);return st_fail(err,"Out of memory preparing request");
}
int st_job_prepare(StJob *j,const StClient *c,enum StJobType type,unsigned generation,const char *a,const char *b,const char *d,int remove,StError *err) {
    memset(j,0,sizeof(*j));j->type=type;j->generation=generation;j->remove=remove;
    st_copy(j->a,sizeof(j->a),a?a:"");st_copy(j->b,sizeof(j->b),b?b:"");st_copy(j->d,sizeof(j->d),d?d:"");st_copy(j->session,sizeof(j->session),c->token);
    return clone(&j->result,c,type,err);
}
void st_job_execute(StJob *j) {
    StClient *c=&j->result;StError *e=&j->error;
    switch(j->type) {
        case ST_JOB_LEAVE_GROUP:j->ok=st_leave_group(c,j->a,e);break;
        case ST_JOB_DELETE_MESSAGE:j->ok=st_delete_message(c,j->a,j->b,e);break;
        case ST_JOB_THUMBNAIL:j->ok=st_photo_thumbnail(j->a,j->media.id,&j->image,e);break;
        case ST_JOB_PREFS_FETCH:j->ok=st_preferences_fetch(c,e);break;
        case ST_JOB_PREFS_SET:j->ok=st_preferences_set(c,j->a,j->remove!=0,(enum StNotify)atoi(j->b),e);break;
        case ST_JOB_PHOTO:j->ok=st_send_photo(c,j->a,j->b,e);break;
        case ST_JOB_CALL_CATCHUP:{cJSON *result=NULL;j->ok=st_call_catchup(c,j->events,&result,&j->remove,e);cJSON_Delete(j->events);j->events=result;break;}
        case ST_JOB_CALL_PREPARE:
            j->ok=st_call_prepare(c,&j->call,e);
            if(j->ok)for(int i=0;i<ST_FX_COUNT&&!st_cancelled();i++){StError ignored={0};st_sound_fetch_effect(c,(enum StEffect)i,j->effects+i,&ignored);}
            if(j->ok&&!st_cancelled())j->ok=st_ws_presence_begin(c,e);
            if(st_cancelled())j->ok=st_fail(e,"Cancelled");
            break;
        case ST_JOB_CALL_AUTH:j->ok=st_call_authorize(c,j->a,j->remove,&j->call,e);break;
        case ST_JOB_MENTION: {
            if(strlen(j->a)!=26||strspn(j->a,"0123456789ABCDEFGHJKMNPQRSTVWXYZ")!=26){st_fail(e,"Invalid mentioned user");break;}
            char path[90];snprintf(path,sizeof(path),"/users/%.26s",j->a);j->ok=st_http(c,"GET",path,NULL,NULL,&j->events,e);break;
        }
        case ST_JOB_SOUND:j->ok=st_sound_fetch(c,j->remove,&j->sound,e);break;
        case ST_JOB_NOTICES:j->ok=st_notices_load(c,&j->events,e);break;
        case ST_JOB_NOTICES_SAVE:j->ok=st_notices_save(c,j->events,e);break;
        case ST_JOB_ACK:j->ok=st_notices_ack(c,j->a,j->b,e);break;
        case ST_JOB_PROBE:j->ok=st_probe(c,e);break;
        case ST_JOB_LOGIN:j->ok=st_login(c,j->a,j->b,e);break;
        case ST_JOB_REGISTER:j->ok=st_register(c,j->a,j->b,j->d,e);break;
        case ST_JOB_HELLO:j->ok=st_onboarding(c,e);break;
        case ST_JOB_ONBOARD:j->ok=st_onboard(c,j->a,e);break;
        case ST_JOB_STATUS:j->ok=st_status(c,j->a,j->b,e);break;
        case ST_JOB_BOOTSTRAP:j->ok=st_bootstrap(c,e);break;
        case ST_JOB_LIVE_CLOSE:st_ws_live_close();j->ok=1;break;
        case ST_JOB_LIVE:j->ok=st_ws_live(c,j->a,&j->events,&j->live_connected,e);break;
        case ST_JOB_FRIEND:j->ok=st_friend_request(c,j->a,e);break;
        case ST_JOB_INVITE:j->ok=st_fetch_invite(c,j->a,e);break;
        case ST_JOB_JOIN:j->ok=st_join_invite(c,j->a,e);break;
        case ST_JOB_LEAVE:j->ok=st_leave_server(c,j->a,e);break;
        case ST_JOB_VOICE:j->ok=st_refresh_voice(c,e);break;
        case ST_JOB_PROFILE:j->ok=st_fetch_profile(c,e);break;
        case ST_JOB_USER:j->ok=st_fetch_user_profile_context(c,j->a,j->b,e);break;
        case ST_JOB_USER_ACTION:j->ok=st_user_action(c,j->a,j->b,e);break;
        case ST_JOB_DM:j->ok=st_open_dm(c,j->a,e);break;
        case ST_JOB_REPORT:j->ok=st_report_user(c,j->a,j->d,j->b,e);break;
        case ST_JOB_SESSIONS:j->ok=st_fetch_sessions(c,e);break;
        case ST_JOB_REVOKE:j->ok=st_revoke_sessions(c,j->a,j->remove,j->b,e);break;
        case ST_JOB_LIVE_HISTORY:j->ok=j->d[0]?st_fetch_messages_nearby(c,j->a,j->d,e):st_fetch_messages(c,j->a,j->b,e);break;
        case ST_JOB_HISTORY:j->ok=st_fetch_messages(c,j->a,j->b,e);break;
        case ST_JOB_SEND:j->ok=st_send_reply(c,j->a,j->b,j->d,e);break;
        case ST_JOB_QUOTE:j->ok=st_fetch_quote(c,j->a,j->b,&j->quote,e);break;
        case ST_JOB_REACT:j->ok=st_react(c,j->a,j->b,j->d,j->remove,e);break;
        case ST_JOB_LOGOUT:j->ok=st_logout(c,e);break;
        case ST_JOB_MEMBERS:j->ok=st_fetch_participants(c,j->a,e);break;
        case ST_JOB_CACHE_STATS:j->ok=st_disk_stats(ST_DISK_DIR,&j->stats,e);break;
        case ST_JOB_CACHE_CLEAR:j->ok=st_disk_clear(ST_DISK_DIR,e);st_disk_stats(ST_DISK_DIR,&j->stats,e);break;
        case ST_JOB_MEDIA:j->ok=st_media_fetch_cached(c,&j->media,&j->image,e);break;
    }
    if(j->type==ST_JOB_LOGIN||j->type==ST_JOB_REGISTER||j->type==ST_JOB_REVOKE)st_wipe(j->b,sizeof(j->b));
}
int st_job_read(enum StJobType t){return t==ST_JOB_THUMBNAIL||t==ST_JOB_PREFS_FETCH||t==ST_JOB_MENTION||t==ST_JOB_SOUND||t==ST_JOB_NOTICES||t==ST_JOB_ACK||t==ST_JOB_QUOTE||t==ST_JOB_INVITE||t==ST_JOB_LIVE_HISTORY||t==ST_JOB_VOICE||t==ST_JOB_HISTORY||t==ST_JOB_MEMBERS||t==ST_JOB_PROFILE||t==ST_JOB_USER||t==ST_JOB_SESSIONS||t==ST_JOB_CACHE_STATS||t==ST_JOB_MEDIA;}
int st_job_apply(StJob *j,StClient *c,unsigned generation,const char *channel) {
    if(strcmp(j->session,c->token))return 0;
    if(st_job_read(j->type)&&j->generation!=generation)return 0;
    if((j->type==ST_JOB_QUOTE||j->type==ST_JOB_HISTORY||j->type==ST_JOB_LIVE_HISTORY||j->type==ST_JOB_MEMBERS)&&strcmp(j->a,channel))return 0;
    StClient *r=&j->result;
    if(j->error.status==401 && j->type!=ST_JOB_LOGIN && j->type!=ST_JOB_REGISTER && (j->type!=ST_JOB_REVOKE||!r->token[0]))st_wipe(c->token,sizeof(c->token));
    if(j->type==ST_JOB_BOOTSTRAP&&!r->token[0])st_wipe(c->token,sizeof(c->token));
    if(j->type==ST_JOB_SEND){st_copy(c->pending_key,sizeof(c->pending_key),r->pending_key);st_copy(c->pending_channel,sizeof(c->pending_channel),r->pending_channel);st_copy(c->pending_text,sizeof(c->pending_text),r->pending_text);st_copy(c->pending_reply,sizeof(c->pending_reply),r->pending_reply);}
    if(j->type==ST_JOB_PHOTO){st_copy(c->photo_signature,sizeof(c->photo_signature),r->photo_signature);st_copy(c->photo_id,sizeof(c->photo_id),r->photo_id);st_copy(c->photo_key,sizeof(c->photo_key),r->photo_key);}
    if(j->type==ST_JOB_PREFS_FETCH||j->type==ST_JOB_PREFS_SET){if(r->notifications){cJSON_Delete(c->notifications);c->notifications=r->notifications;r->notifications=NULL;}}
    if(!j->ok)return 1;
#define TAKE_ARRAY(field,count) do{free(c->field);c->field=r->field;c->count=r->count;r->field=NULL;r->count=0;}while(0)
#define TAKE_JSON(field) do{cJSON_Delete(c->field);c->field=r->field;r->field=NULL;}while(0)
    switch(j->type) {
        case ST_JOB_QUOTE:
            if(!strcmp(c->messages_channel,j->a))for(size_t i=0;i<c->message_count;i++)for(int k=0;k<c->messages[i].reply_count;k++)if(!strcmp(c->messages[i].replies[k].id,j->b))c->messages[i].replies[k]=j->quote;
            break;
        case ST_JOB_PROBE:
            st_copy(c->voice_url,sizeof(c->voice_url),r->voice_url);st_copy(c->voice_node,sizeof(c->voice_node),r->voice_node);
            st_copy(c->app_url,sizeof(c->app_url),r->app_url);st_copy(c->version,sizeof(c->version),r->version);st_copy(c->ws_url,sizeof(c->ws_url),r->ws_url);st_copy(c->assets_url,sizeof(c->assets_url),r->assets_url);
            c->discovered=r->discovered;c->invite_only=r->invite_only;c->captcha=r->captcha;c->email_verification=r->email_verification;break;
        case ST_JOB_LOGIN:st_copy(c->token,sizeof(c->token),r->token);st_copy(c->self_id,sizeof(c->self_id),r->self_id);st_copy(c->session_id,sizeof(c->session_id),r->session_id);break;
        case ST_JOB_HELLO:c->onboarding=r->onboarding;break;
        case ST_JOB_ONBOARD:c->onboarding=0;TAKE_JSON(self);break;
        case ST_JOB_VOICE:TAKE_JSON(notification_roles);TAKE_ARRAY(roles,role_count);TAKE_ARRAY(voice,voice_count);TAKE_ARRAY(emojis,emoji_count);TAKE_ARRAY(users,user_count);TAKE_ARRAY(servers,server_count);TAKE_ARRAY(channels,channel_count);break;
        case ST_JOB_CALL_CATCHUP:case ST_JOB_BOOTSTRAP:TAKE_JSON(notification_roles);TAKE_ARRAY(roles,role_count);TAKE_ARRAY(voice,voice_count);TAKE_ARRAY(emojis,emoji_count);TAKE_ARRAY(users,user_count);TAKE_ARRAY(servers,server_count);TAKE_ARRAY(channels,channel_count);TAKE_JSON(self);break;
        case ST_JOB_PROFILE:TAKE_JSON(self);TAKE_JSON(profile);break;
        case ST_JOB_USER:TAKE_JSON(profile_user);TAKE_JSON(profile);TAKE_JSON(mutual);st_copy(c->profile_id,sizeof(c->profile_id),r->profile_id);
            st_copy(c->profile_server,64,r->profile_server);TAKE_ARRAY(users,user_count);TAKE_ARRAY(members,member_count);TAKE_ARRAY(member_roles,member_role_count);st_copy(c->members_server,64,r->members_server);
            if(!strcmp(c->profile_id,c->self_id)){cJSON *user=cJSON_Duplicate(c->profile_user,1);if(user){cJSON_Delete(c->self);c->self=user;}}break;
        case ST_JOB_FRIEND:TAKE_ARRAY(users,user_count);break;
        case ST_JOB_INVITE:TAKE_JSON(invitation);st_copy(c->invitation_code,64,r->invitation_code);break;
        case ST_JOB_JOIN:st_copy(c->joined_server,64,r->joined_server);st_copy(c->joined_channel,64,r->joined_channel);break;
        case ST_JOB_LEAVE:st_remove_server(c,j->a);break;
        case ST_JOB_LEAVE_GROUP:st_remove_channel(c,j->a);break;
        case ST_JOB_DELETE_MESSAGE:st_remove_message(c,j->a,j->b);break;
        case ST_JOB_USER_ACTION:
            TAKE_ARRAY(users,user_count);
            if(!strcmp(c->profile_id,j->a))TAKE_JSON(profile_user);
            break;
        case ST_JOB_DM:TAKE_ARRAY(channels,channel_count);st_copy(c->opened_dm,sizeof(c->opened_dm),r->opened_dm);break;
        case ST_JOB_SESSIONS:TAKE_ARRAY(sessions,session_count);break;
        case ST_JOB_REVOKE:if(!r->token[0])st_client_clear(c);break;
        case ST_JOB_STATUS:TAKE_JSON(self);break;
        case ST_JOB_LIVE_HISTORY:case ST_JOB_HISTORY:TAKE_ARRAY(messages,message_count);TAKE_ARRAY(users,user_count);TAKE_ARRAY(members,member_count);TAKE_ARRAY(member_roles,member_role_count);st_copy(c->members_server,64,r->members_server);st_copy(c->messages_channel,sizeof(c->messages_channel),r->messages_channel);break;
        case ST_JOB_MEMBERS:TAKE_ARRAY(servers,server_count);TAKE_ARRAY(roles,role_count);TAKE_ARRAY(users,user_count);TAKE_ARRAY(members,member_count);TAKE_ARRAY(member_roles,member_role_count);st_copy(c->members_server,64,r->members_server);TAKE_ARRAY(participants,participant_count);st_copy(c->participants_channel,sizeof(c->participants_channel),r->participants_channel);break;
        case ST_JOB_LOGOUT:st_client_clear(c);break;
        default:break;
    }
#undef TAKE_ARRAY
#undef TAKE_JSON
    st_participants_sort(c);
    return 1;
}
void st_job_clear(StJob *j){for(int i=0;i<ST_FX_COUNT;i++)st_sound_free(j->effects+i);st_sound_free(&j->sound);cJSON_Delete(j->events);st_image_free(&j->image);st_client_clear(&j->result);st_wipe(j,sizeof(*j));}
