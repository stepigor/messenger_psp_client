#include "transport.h"
#include "media.h"
#include "emoji.h"
#include "preferences.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void st_client_init(StClient *c,const StConfig *config) { memset(c,0,sizeof(*c)); c->config=*config; }
void st_client_clear(StClient *c) {
    free(c->users); free(c->servers); free(c->channels); free(c->messages); free(c->participants);free(c->emojis);free(c->voice);free(c->roles);st_members_clear(c);
    cJSON_Delete(c->notification_roles);cJSON_Delete(c->notifications);cJSON_Delete(c->self); cJSON_Delete(c->profile);cJSON_Delete(c->invitation);cJSON_Delete(c->profile_user);cJSON_Delete(c->mutual);free(c->sessions);
    StConfig config=c->config; st_wipe(c,sizeof(*c)); c->config=config;
}
static int identifier(const char *s) {
    size_t n=strlen(s); if (!n || n>=64) return 0;
    for(;*s;s++) if (!isalnum((unsigned char)*s) && *s!='_' && *s!='-') return 0;
    return 1;
}
static void parse_user(StUser *u,const cJSON *o) {
    st_copy(u->id,sizeof(u->id),st_str(o,"_id"));
    const char *name=st_str(o,"display_name"); if (!*name) name=st_str(o,"username");
    if(*name)st_copy(u->name,sizeof(u->name),name);
    if(cJSON_HasObjectItem(o,"pronouns"))st_copy(u->pronouns,sizeof(u->pronouns),st_str(o,"pronouns"));
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(o,"relationship")))
        st_copy(u->relationship,sizeof(u->relationship),st_str(o,"relationship"));
    if(cJSON_HasObjectItem(o,"avatar")){StMediaRequest key;st_profile_image_ref(cJSON_GetObjectItemCaseSensitive(o,"avatar"),"avatars",&key);st_copy(u->avatar,sizeof(u->avatar),key.id);st_copy(u->avatar_file,sizeof(u->avatar_file),key.filename);u->avatar_animated=key.animated;}
    const cJSON *online=cJSON_GetObjectItemCaseSensitive(o,"online");
    if (cJSON_IsBool(online)) u->online=cJSON_IsTrue(online);
    if(cJSON_HasObjectItem(o,"status"))u->presence=st_presence_value(st_str(cJSON_GetObjectItemCaseSensitive(o,"status"),"presence"));
}
const char *st_user_name(const StClient *c,const char *id) {
    for(size_t i=0;i<c->user_count;i++) if (!strcmp(c->users[i].id,id)) return *c->users[i].name?c->users[i].name:id;
    return id;
}
const char *st_channel_name(const StClient *c,const StChannel *ch) {
    if (!strcmp(ch->type,"DirectMessage")) return st_user_name(c,ch->peer);
    if (!strcmp(ch->type,"SavedMessages")) return "Saved messages";
    return *ch->name?ch->name:ch->id;
}
int st_probe(StClient *c,StError *err) {
    cJSON *root=NULL; if (!st_http(c,"GET","/",NULL,NULL,&root,err)) return 0;
    const char *version=st_str(root,"revolt"), *ws=st_str(root,"ws");
    char host[512],path[512];
    int ok=*version && strlen(version)<sizeof(c->version) && strlen(ws)<sizeof(c->ws_url) && st_url_split(ws,"wss://",host,sizeof(host),path,sizeof(path));
    if (ok) { st_copy(c->version,sizeof(c->version),version); st_copy(c->ws_url,sizeof(c->ws_url),ws); }
    c->app_url[0]=0;const char *app=st_str(root,"app");
    if(strlen(app)<sizeof(c->app_url)&&st_url_split(app,"https://",host,sizeof(host),path,sizeof(path))&&!strchr(app,'?'))st_copy(c->app_url,sizeof(c->app_url),app);
    const cJSON *features=cJSON_GetObjectItemCaseSensitive(root,"features");
    c->discovered=ok;c->invite_only=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(features,"invite_only"));
    c->email_verification=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(features,"email"));
    c->captcha=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(features,"captcha"),"enabled"));
    const cJSON *autumn=cJSON_GetObjectItemCaseSensitive(features,"autumn");
    const char *assets=st_str(autumn,"url");c->assets_url[0]=0;
    if(ok && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(autumn,"enabled")) &&
       strlen(assets)<sizeof(c->assets_url) && st_url_split(assets,"https://",host,sizeof(host),path,sizeof(path)) && !strchr(assets,'?'))
        st_copy(c->assets_url,sizeof(c->assets_url),assets);
    c->voice_url[0]=c->voice_node[0]=0;
    const cJSON *lk=cJSON_GetObjectItemCaseSensitive(features,"livekit");
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(lk,"enabled"))) {
        const cJSON *nodes=cJSON_GetObjectItemCaseSensitive(lk,"nodes"),*node;
        cJSON_ArrayForEach(node,nodes) {
            const char *url=st_str(node,"public_url"),*name=st_str(node,"name");
            if(*name&&strlen(name)<sizeof(c->voice_node)&&strlen(url)<sizeof(c->voice_url)&&!strpbrk(url,"?#")&&st_url_split(url,"wss://",host,sizeof(host),path,sizeof(path))) {
                st_copy(c->voice_url,sizeof(c->voice_url),url);st_copy(c->voice_node,sizeof(c->voice_node),name);break;
            }
        }
    }
    cJSON_Delete(root);
    return ok?1:st_fail(err,"Endpoint must provide a compatible API with secure WSS");
}
int st_login(StClient *c,const char *email,const char *password,StError *err) {
    if (c->token[0]) return st_fail(err,"Already logged in");
    cJSON *body=cJSON_CreateObject(), *res=NULL;
    if (!body) return st_fail(err,"Out of memory");
    cJSON_AddStringToObject(body,"email",email); cJSON_AddStringToObject(body,"password",password);
    cJSON_AddStringToObject(body,"friendly_name",ST_APP_NAME " " ST_APP_VERSION);
    int ok=st_http(c,"POST","/auth/session/login",body,NULL,&res,err);
    st_wipe((void *)st_str(body,"password"),strlen(st_str(body,"password"))); cJSON_Delete(body);
    if (!ok) return 0;
    const char *result=st_str(res,"result"), *token=st_str(res,"token"), *uid=st_str(res,"user_id");
    if (!strcmp(result,"MFA")) ok=st_fail(err,"MFA is not supported in this build");
    else if (!strcmp(result,"Disabled")) ok=st_fail(err,"Account is disabled");
    else if (strcmp(result,"Success") || !*token || strlen(token)>=sizeof(c->token) || !identifier(uid)) ok=st_fail(err,"Invalid login response");
    else {
        for(const char *p=token;*p;p++) if ((unsigned char)*p<33 || (unsigned char)*p>126) ok=0;
        if (!ok) st_fail(err,"Invalid session token");
        else { st_copy(c->token,sizeof(c->token),token); st_copy(c->self_id,sizeof(c->self_id),uid);const char *sid=st_str(res,"_id");if(identifier(sid))st_copy(c->session_id,sizeof(c->session_id),sid); }
    }
    st_wipe((void *)token,strlen(token)); cJSON_Delete(res); return ok;
}
int st_register(StClient *c,const char *email,const char *password,const char *invite,StError *err) {
    if(!c->discovered)return st_fail(err,"Wait for server configuration");
    if(c->captcha)return st_fail(err,"Registration requires browser hCaptcha on this server");
    if(!*email||!*password)return st_fail(err,"Email and password are required");
    if(c->invite_only&&(!invite||!*invite))return st_fail(err,"Server requires an account invitation code");
    cJSON *body=cJSON_CreateObject(),*res=NULL;if(!body)return st_fail(err,"Out of memory");
    cJSON_AddStringToObject(body,"email",email);cJSON_AddStringToObject(body,"password",password);
    if(c->invite_only)cJSON_AddStringToObject(body,"invite",invite);
    int ok=st_http(c,"POST","/auth/account/create",body,NULL,&res,err);
    st_wipe((void *)st_str(body,"password"),strlen(st_str(body,"password")));cJSON_Delete(body);cJSON_Delete(res);return ok;
}
int st_onboarding(StClient *c,StError *err) {
    cJSON *res=NULL;if(!st_http(c,"GET","/onboard/hello",NULL,NULL,&res,err))return 0;
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(res,"onboarding");int ok=cJSON_IsBool(value);
    if(ok)c->onboarding=cJSON_IsTrue(value);
    cJSON_Delete(res);return ok?1:st_fail(err,"Invalid onboarding response");
}
int st_onboard(StClient *c,const char *username,StError *err) {
    size_t chars;if(!st_utf8_count(username,&chars)||chars<2||chars>32)return st_fail(err,"Username must contain 2-32 characters");
    cJSON *body=cJSON_CreateObject(),*res=NULL;if(!body)return st_fail(err,"Out of memory");
    cJSON_AddStringToObject(body,"username",username);int ok=st_http(c,"POST","/onboard/complete",body,NULL,&res,err);cJSON_Delete(body);
    if(ok&&!strcmp(st_str(res,"_id"),c->self_id)){cJSON_Delete(c->self);c->self=res;c->onboarding=0;return 1;}
    cJSON_Delete(res);return ok?st_fail(err,"Invalid onboarding response"):0;
}
int st_status(StClient *c,const char *text,const char *presence,StError *err) {
    static const char *valid[]={"Online","Idle","Focus","Busy","Invisible"};int found=0;
    for(int i=0;i<5;i++)if(!strcmp(presence,valid[i]))found=1;
    size_t chars;if(!found||!st_utf8_count(text,&chars)||chars>128)return st_fail(err,"Invalid status");
    cJSON *body=cJSON_CreateObject(),*status=cJSON_CreateObject(),*res=NULL;
    if(!body||!status){cJSON_Delete(body);cJSON_Delete(status);return st_fail(err,"Out of memory");}
    cJSON_AddItemToObject(body,"status",status);cJSON_AddStringToObject(status,"text",text);cJSON_AddStringToObject(status,"presence",presence);
    int ok=st_http(c,"PATCH","/users/@me",body,NULL,&res,err);cJSON_Delete(body);
    if(ok&&cJSON_IsObject(res)){cJSON_Delete(c->self);c->self=res;return 1;}cJSON_Delete(res);return ok?st_fail(err,"Invalid status response"):0;
}
int st_load_ready(StClient *c,const cJSON *r,StError *err) {
    const cJSON *ua=cJSON_GetObjectItemCaseSensitive(r,"users"), *sa=cJSON_GetObjectItemCaseSensitive(r,"servers"), *ca=cJSON_GetObjectItemCaseSensitive(r,"channels");
    if (strcmp(st_str(r,"type"),"Ready") || !cJSON_IsArray(ua)||!cJSON_IsArray(sa)||!cJSON_IsArray(ca)) return st_fail(err,"Invalid Ready payload");
    const cJSON *ea=cJSON_GetObjectItemCaseSensitive(r,"emojis");
    if(ea&&!cJSON_IsArray(ea))return st_fail(err,"Invalid emoji catalogue");
    size_t en=(size_t)cJSON_GetArraySize(ea);
    if(en>ST_CUSTOM_EMOJI_MAX)return st_fail(err,"Emoji catalogue is too large");
    size_t un=(size_t)cJSON_GetArraySize(ua),sn=(size_t)cJSON_GetArraySize(sa),cn=(size_t)cJSON_GetArraySize(ca);
    if (un>4096||sn>256||cn>4096) return st_fail(err,"Account exceeds prototype list limits");
    size_t user_capacity=un+(c->message_count<50?c->message_count:50)+16;if(user_capacity>4096)user_capacity=4096;
    StUser *users=calloc(user_capacity?user_capacity:1,sizeof(*users)); StServer *servers=calloc(sn?sn:1,sizeof(*servers)); StChannel *channels=calloc(cn?cn:1,sizeof(*channels));
    if (!users||!servers||!channels) { free(users);free(servers);free(channels);return st_fail(err,"Out of memory"); }
    const cJSON *o; size_t i=0;
    cJSON_ArrayForEach(o,ua) { parse_user(users+i++,o); }
    /* Ready excludes unrelated text-channel authors. Keep names/avatars for
     * the bounded history page, but do not retain obsolete relationships. */
    for(size_t m=0;m<c->message_count&&un<user_capacity;m++) {
        const char *id=c->messages[m].author;int known=0;for(size_t k=0;k<un;k++)if(!strcmp(users[k].id,id))known=1;
        if(known||!*id)continue;
        for(size_t k=0;k<c->user_count;k++)if(!strcmp(c->users[k].id,id)){users[un]=c->users[k];st_copy(users[un++].relationship,24,"None");break;}
    }
    /* Retain bounded, already-resolved mentions across a Ready resnapshot.
     * No network lookup is triggered by history, code blocks or hidden text. */
    size_t kept=0;
    for(size_t k=0;k<c->user_count&&un<user_capacity&&kept<16;k++) {
        const StUser *old=c->users+k;if(strlen(old->id)!=26)continue;
        int known=0;for(size_t j=0;j<un;j++)if(!strcmp(users[j].id,old->id))known=1;if(known)continue;
        char mention[32];snprintf(mention,sizeof(mention),"<@%.26s>",old->id);int used=0;
        for(size_t j=0;j<c->message_count&&!used;j++) {
            if(strstr(c->messages[j].content,mention))used=1;
            for(int q=0;q<c->messages[j].reply_count&&!used;q++)if(strstr(c->messages[j].replies[q].content,mention))used=1;
        }
        if(used){users[un]=*old;st_copy(users[un++].relationship,24,"None");kept++;}
    }
    i=0; cJSON_ArrayForEach(o,sa) { st_copy(servers[i].id,sizeof(servers[i].id),st_str(o,"_id")); st_copy(servers[i].name,sizeof(servers[i].name),st_str(o,"name"));st_image_ref(cJSON_GetObjectItemCaseSensitive(o,"icon"),"icons",servers[i].icon);st_copy(servers[i].owner,sizeof(servers[i].owner),st_str(o,"owner"));st_server_file(cJSON_GetObjectItemCaseSensitive(o,"banner"),"banners",&servers[i].banner); i++; }
    i=0; cJSON_ArrayForEach(o,ca) {
        StChannel *ch=channels+i++;
        st_copy(ch->id,sizeof(ch->id),st_str(o,"_id")); st_copy(ch->name,sizeof(ch->name),st_str(o,"name"));
        st_copy(ch->server,sizeof(ch->server),st_str(o,"server")); st_copy(ch->type,sizeof(ch->type),st_str(o,"channel_type"));st_image_ref(cJSON_GetObjectItemCaseSensitive(o,"icon"),"icons",ch->icon);
        st_copy(ch->last_message,64,st_str(o,"last_message_id"));
        ch->voice=cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(o,"voice"))||!strcmp(ch->type,"VoiceChannel");
        const cJSON *recipient; cJSON_ArrayForEach(recipient,cJSON_GetObjectItemCaseSensitive(o,"recipients"))
            if (cJSON_IsString(recipient) && strcmp(recipient->valuestring,c->self_id)) { st_copy(ch->peer,sizeof(ch->peer),recipient->valuestring); break; }
    }
    const cJSON *va=cJSON_GetObjectItemCaseSensitive(r,"voice_states"),*v,*member;size_t vn=0;
    if(va&&!cJSON_IsArray(va)){free(users);free(servers);free(channels);return st_fail(err,"Invalid voice state list");}
    cJSON_ArrayForEach(v,va) {
        const cJSON *ma=cJSON_GetObjectItemCaseSensitive(v,"participants");
        if(!identifier(st_str(v,"id"))||!cJSON_IsArray(ma)||(size_t)cJSON_GetArraySize(ma)>ST_VOICE_MAX-vn){free(users);free(servers);free(channels);return st_fail(err,"Invalid voice participants");}
        vn+=(size_t)cJSON_GetArraySize(ma);
    }
    StVoiceMember *voice=calloc(vn?vn:1,sizeof(*voice));size_t voices=0;
    if(!voice){free(users);free(servers);free(channels);return st_fail(err,"Out of memory");}
    cJSON_ArrayForEach(v,va) {
        int known=0;for(size_t k=0;k<cn;k++)if(!strcmp(channels[k].id,st_str(v,"id")))known=1;
        if(!known)continue;
        cJSON_ArrayForEach(member,cJSON_GetObjectItemCaseSensitive(v,"participants")) {
            const char *id=st_str(member,"id");if(!identifier(id))continue;int duplicate=0;
            for(size_t k=0;k<voices;k++)if(!strcmp(voice[k].channel,st_str(v,"id"))&&!strcmp(voice[k].user,id))duplicate=1;
            if(duplicate)continue;
            StVoiceMember *m=voice+voices++;st_copy(m->channel,sizeof(m->channel),st_str(v,"id"));st_copy(m->user,sizeof(m->user),id);
            m->receiving=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(member,"is_receiving"));m->publishing=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(member,"is_publishing"));
            m->screensharing=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(member,"screensharing"));m->camera=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(member,"camera"));
        }
    }
    StCustomEmoji *emojis=calloc(en?en:1,sizeof(*emojis));
    if(!emojis){free(voice);free(users);free(servers);free(channels);return st_fail(err,"Out of memory");}
    size_t count=0;
    cJSON_ArrayForEach(o,ea) {
        const cJSON *parent=cJSON_GetObjectItemCaseSensitive(o,"parent");
        if(!st_custom_emoji_id(st_str(o,"_id"))||strcmp(st_str(parent,"type"),"Server")||!identifier(st_str(parent,"id")))continue;
        int known=0;for(size_t k=0;k<sn;k++)if(!strcmp(servers[k].id,st_str(parent,"id")))known=1;
        for(size_t k=0;k<count;k++)if(!strcmp(emojis[k].id,st_str(o,"_id")))known=0;
        if(!known)continue;
        StCustomEmoji *e=emojis+count++;st_copy(e->id,sizeof(e->id),st_str(o,"_id"));st_copy(e->server,sizeof(e->server),st_str(parent,"id"));st_copy(e->name,sizeof(e->name),st_str(o,"name"));e->animated=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"animated"));
    }
    StRole *roles=NULL;size_t role_count=0;
    if(!st_roles_parse(sa,&roles,&role_count,err)){free(voice);free(emojis);free(users);free(servers);free(channels);return 0;}
    free(c->roles);c->roles=roles;c->role_count=role_count;
    free(c->voice);c->voice=voice;c->voice_count=voices;
    free(c->emojis);c->emojis=emojis;c->emoji_count=count;
    free(c->users);free(c->servers);free(c->channels);
    c->users=users;c->user_count=un;c->servers=servers;c->server_count=sn;c->channels=channels;c->channel_count=cn;
    st_preferences_memberships(c,cJSON_GetObjectItemCaseSensitive(r,"members"));
    return 1;
}
int st_channel_can_call(const StChannel *ch){return ch&&(ch->voice||!strcmp(ch->type,"DirectMessage")||!strcmp(ch->type,"Group"));}
int st_voice_count(const StClient *c,const char *channel) {int n=0;for(size_t i=0;i<c->voice_count;i++)if(!strcmp(c->voice[i].channel,channel))n++;return n;}
const StVoiceMember *st_voice_member(const StClient *c,const char *channel,int row) {for(size_t i=0;i<c->voice_count;i++)if(!strcmp(c->voice[i].channel,channel)&&row--==0)return c->voice+i;return NULL;}
int st_refresh_voice(StClient *c,StError *err) {
    if(!c->token[0])return st_fail(err,"Login required");
    cJSON *ready=NULL;if(!st_ws_ready(c,&ready,err))return 0;
    int ok=st_load_ready(c,ready,err);cJSON_Delete(ready);return ok;
}
int st_bootstrap(StClient *c,StError *err) {
    if (!c->token[0]) return st_fail(err,"Login required");
    cJSON *self=NULL,*ready=NULL;
    if (!st_http(c,"GET","/users/@me",NULL,NULL,&self,err)) return 0;
    if (strcmp(st_str(self,"_id"),c->self_id)) { cJSON_Delete(self);st_wipe(c->token,sizeof(c->token));return st_fail(err,"Saved session user mismatch; login required"); }
    if (!st_ws_ready(c,&ready,err)) { cJSON_Delete(self); return 0; }
    int ok=st_load_ready(c,ready,err); cJSON_Delete(ready);
    if (ok) { cJSON_Delete(c->self); c->self=self; } else cJSON_Delete(self);
    return ok;
}
int st_fetch_profile(StClient *c,StError *err) {
    char path[128]; snprintf(path,sizeof(path),"/users/%s/profile",c->self_id);
    cJSON *self=NULL,*profile=NULL;
    if (!st_http(c,"GET","/users/@me",NULL,NULL,&self,err)) return 0;
    if (!st_http(c,"GET",path,NULL,NULL,&profile,err)) { cJSON_Delete(self);return 0; }
    if (!cJSON_IsObject(self)||!cJSON_IsObject(profile)) { cJSON_Delete(self);cJSON_Delete(profile);return st_fail(err,"Invalid profile response"); }
    cJSON_Delete(c->self);cJSON_Delete(c->profile); c->self=self;c->profile=profile;return 1;
}
int st_merge_user(StClient *c,const cJSON *user,StError *err) {
    const char *id=st_str(user,"_id");if(!identifier(id))return st_fail(err,"Invalid user response");
    size_t i;for(i=0;i<c->user_count;i++)if(!strcmp(c->users[i].id,id))break;
    if(i==c->user_count){if(i>=4096)return st_fail(err,"User list limit");StUser *p=realloc(c->users,(i+1)*sizeof(*p));if(!p)return st_fail(err,"Out of memory");c->users=p;memset(p+i,0,sizeof(*p));c->user_count++;}
    parse_user(c->users+i,user);return 1;
}
static int update_users(StClient *c,const cJSON *ua) {
    const cJSON *u;
    cJSON_ArrayForEach(u,ua) {
        const char *id=st_str(u,"_id"); size_t i;
        for(i=0;i<c->user_count;i++) if (!strcmp(c->users[i].id,id)) break;
        if (i==c->user_count) {
            if (i>=4096) return 0;
            StUser *p=realloc(c->users,(i+1)*sizeof(*p));if(!p)return 0;
            c->users=p;memset(p+i,0,sizeof(*p));c->user_count++;
        }
        parse_user(c->users+i,u);
    }
    return 1;
}
static int newest_first(const void *a,const void *b){return strcmp(((const StMessage *)b)->id,((const StMessage *)a)->id);}
static int fetch_messages(StClient *c,const char *channel,const char *before,const char *nearby,StError *err) {
    if (!identifier(channel)|| (before && *before && !identifier(before))||(nearby&&*nearby&&!identifier(nearby))) return st_fail(err,"Invalid channel or message ID");
    int limit=c->config.page_size;if(nearby&&*nearby&&limit>48)limit=48;
    char path[350];snprintf(path,sizeof(path),"/channels/%s/messages?limit=%d&sort=Latest&include_users=true%s%s%s%s",channel,limit,before&&*before?"&before=":"",before?before:"",nearby&&*nearby?"&nearby=":"",nearby?nearby:"");
    cJSON *res=NULL;if(!st_http(c,"GET",path,NULL,NULL,&res,err))return 0;
    const cJSON *ma=cJSON_IsArray(res)?res:cJSON_GetObjectItemCaseSensitive(res,"messages");
    if (!cJSON_IsArray(ma)) { cJSON_Delete(res);return st_fail(err,"Invalid message response"); }
    size_t n=(size_t)cJSON_GetArraySize(ma);
    if(n>50){cJSON_Delete(res);return st_fail(err,"Message page limit exceeded");}
    StMessage *m=calloc(n?n:1,sizeof(*m));if(!m){cJSON_Delete(res);return st_fail(err,"Out of memory");}
    if (!update_users(c,cJSON_GetObjectItemCaseSensitive(res,"users"))) { free(m);cJSON_Delete(res);return st_fail(err,"User list memory limit"); }
    const char *server="";for(size_t k=0;k<c->channel_count;k++)if(!strcmp(c->channels[k].id,channel))server=c->channels[k].server;
    const cJSON *members=cJSON_GetObjectItemCaseSensitive(res,"members");
    if(*server&&members&&!st_members_merge(c,server,members,0,err)){free(m);cJSON_Delete(res);return 0;}
    if(*server&&cJSON_IsArray(members)) {
        const cJSON *message,*member;
        cJSON_ArrayForEach(message,ma) {
            const char *author=st_str(message,"author");int found=0;
            cJSON_ArrayForEach(member,members)if(!strcmp(st_str(cJSON_GetObjectItemCaseSensitive(member,"_id"),"user"),author))found=1;
            if(!found)st_member_remove(c,server,author);
        }
    }
    const cJSON *o;size_t i=0;
    cJSON_ArrayForEach(o,ma)st_message_parse(c,o,channel,m+i++);
    if(nearby&&*nearby)qsort(m,n,sizeof(*m),newest_first);
    st_quotes_resolve(c,m,n,channel);
    cJSON_Delete(res);free(c->messages);c->messages=m;c->message_count=n;st_copy(c->messages_channel,sizeof(c->messages_channel),channel);return 1;
}
int st_fetch_messages(StClient *c,const char *channel,const char *before,StError *err){return fetch_messages(c,channel,before,NULL,err);}
int st_fetch_messages_nearby(StClient *c,const char *channel,const char *nearby,StError *err){return fetch_messages(c,channel,NULL,nearby,err);}
int st_send_reply(StClient *c,const char *channel,const char *content,const char *reply,StError *err) {
    if(!reply)reply="";
    if(*reply&&!identifier(reply))return st_fail(err,"Invalid reply ID");
    size_t chars;
    if (!identifier(channel)|| !*content || strlen(content)>=ST_TEXT_MAX || !st_utf8_count(content,&chars)||chars>2000) return st_fail(err,"Message must contain 1-2000 Unicode characters");
    if (!c->pending_key[0] || strcmp(c->pending_channel,channel)||strcmp(c->pending_text,content)||strcmp(c->pending_reply,reply)) {
        unsigned char random[16];if(!st_random(random,sizeof(random)))return st_fail(err,"Random source unavailable");
        for(size_t i=0;i<sizeof(random);i++) snprintf(c->pending_key+i*2,3,"%02x",random[i]);
        st_copy(c->pending_channel,sizeof(c->pending_channel),channel);st_copy(c->pending_text,sizeof(c->pending_text),content);
        st_copy(c->pending_reply,sizeof(c->pending_reply),reply);
    }
    char path[128];snprintf(path,sizeof(path),"/channels/%s/messages",channel);
    cJSON *body=cJSON_CreateObject(),*res=NULL;if(!body)return st_fail(err,"Out of memory");
    cJSON_AddStringToObject(body,"content",content);
    if(*reply){cJSON *refs=cJSON_AddArrayToObject(body,"replies"),*ref=cJSON_CreateObject();if(refs&&ref){cJSON_AddStringToObject(ref,"id",reply);cJSON_AddBoolToObject(ref,"mention",0);cJSON_AddItemToArray(refs,ref);}else{cJSON_Delete(ref);cJSON_Delete(body);return st_fail(err,"Out of memory preparing reply");}}
    int ok=st_http(c,"POST",path,body,c->pending_key,&res,err);cJSON_Delete(body);cJSON_Delete(res);
    /* v0.15.1 rejects a repeated key with DuplicateNonce rather than replaying
     * the first response. Confirm it against recent history before clearing the draft. */
    if(!ok && err->status==409 && strstr(err->text,"DuplicateNonce")) {
        snprintf(path,sizeof(path),"/channels/%s/messages?limit=100&sort=Latest",channel);
        cJSON *recent=NULL;StError check={0};
        if(st_http(c,"GET",path,NULL,NULL,&recent,&check)) {
            const cJSON *messages=cJSON_IsArray(recent)?recent:cJSON_GetObjectItemCaseSensitive(recent,"messages"),*m;
            cJSON_ArrayForEach(m,messages)if(!strcmp(st_str(m,"nonce"),c->pending_key)&&!strcmp(st_str(m,"author"),c->self_id)&&!strcmp(st_str(m,"content"),content)){ok=1;break;}
        }
        cJSON_Delete(recent);
        if(!ok)st_fail(err,"Send result uncertain. Check history before changing or resending the draft.");
    } else if(!ok && err->status>=400 && err->status<500) {
        /* An explicit rejection can consume the server's key before validation. */
        st_wipe(c->pending_key,sizeof(c->pending_key));
    }
    if(ok){st_wipe(c->pending_key,sizeof(c->pending_key));st_wipe(c->pending_text,sizeof(c->pending_text));c->pending_channel[0]=c->pending_reply[0]=0;}
    return ok;
}
int st_send(StClient *c,const char *channel,const char *content,StError *err){return st_send_reply(c,channel,content,NULL,err);}
int st_react(StClient *c,const char *channel,const char *message,const char *emoji,int remove,StError *err) {
    if(!identifier(channel)||!identifier(message)||!*emoji||strlen(emoji)>128)return st_fail(err,"Invalid reaction");
    CURL *h=curl_easy_init();if(!h)return st_fail(err,"Out of memory");
    char *escaped=curl_easy_escape(h,emoji,0);curl_easy_cleanup(h);if(!escaped)return st_fail(err,"Out of memory");
    char path[650];snprintf(path,sizeof(path),"/channels/%s/messages/%s/reactions/%s",channel,message,escaped);curl_free(escaped);
    cJSON *res=NULL;int ok=st_http(c,remove?"DELETE":"PUT",path,NULL,NULL,&res,err);cJSON_Delete(res);return ok;
}
int st_logout(StClient *c,StError *err) {
    cJSON *res=NULL;int ok=1;
    if(c->token[0])ok=st_http(c,"POST","/auth/session/logout",NULL,NULL,&res,err);
    cJSON_Delete(res);
    if(ok || err->status==401)st_client_clear(c);
    return ok;
}
