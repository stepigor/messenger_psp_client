#include "stoat.h"
#include "media.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define VIEW_CHANNEL (UINT64_C(1)<<20)
typedef struct { const cJSON *role; const char *id; double rank; } RankedRole;

static int valid_id(const char *id) {
    size_t n=strlen(id); if (!n || n>=64) return 0;
    for (;*id;id++) if (!((*id>='a'&&*id<='z') || (*id>='A'&&*id<='Z') ||
                          (*id>='0'&&*id<='9') || *id=='_' || *id=='-')) return 0;
    return 1;
}
static int bit(const cJSON *number,int *value) {
    if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) ||
        floor(number->valuedouble)!=number->valuedouble ||
        number->valuedouble < -1 || number->valuedouble > 4503599627370495.0) return 0;
    *value=(((uint64_t)(int64_t)number->valuedouble)&VIEW_CHANNEL)!=0; return 1;
}
static int apply(int *view,const cJSON *override) {
    if (!override || cJSON_IsNull(override)) return 1;
    int allow,deny;
    if (!cJSON_IsObject(override) ||
        !bit(cJSON_GetObjectItemCaseSensitive(override,"a"),&allow) ||
        !bit(cJSON_GetObjectItemCaseSensitive(override,"d"),&deny)) return 0;
    /* PermissionValue::apply: (old | allow) & ~deny. Deny wins in one override. */
    *view=(*view || allow) && !deny; return 1;
}
static int rank_order(const void *a,const void *b) {
    const RankedRole *x=a,*y=b;
    return x->rank<y->rank?1:x->rank>y->rank?-1:0;
}
int st_member_can_view(const cJSON *server,const cJSON *channel,
                      const cJSON *member,const cJSON *user,StError *err) {
    const char *uid=st_str(user,"_id");
    if (!valid_id(uid) || !cJSON_IsObject(member) || !cJSON_IsObject(server) ||
        !cJSON_IsObject(channel)) goto invalid;
    const cJSON *key=cJSON_GetObjectItemCaseSensitive(member,"_id");
    if (strcmp(st_str(key,"user"),uid) ||
        strcmp(st_str(key,"server"),st_str(server,"_id")) ||
        strcmp(st_str(channel,"server"),st_str(server,"_id"))) goto invalid;
    if (!strcmp(uid,st_str(server,"owner")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(user,"privileged"))) return 1;
    int view;
    if (!bit(cJSON_GetObjectItemCaseSensitive(server,"default_permissions"),&view)) goto invalid;
    const cJSON *roles=cJSON_GetObjectItemCaseSensitive(server,"roles");
    const cJSON *assigned=cJSON_GetObjectItemCaseSensitive(member,"roles");
    if ((roles && !cJSON_IsObject(roles)) || (assigned && !cJSON_IsArray(assigned)) ||
        cJSON_GetArraySize(assigned)>256) goto invalid;
    RankedRole ordered[256]; size_t count=0; const cJSON *id;
    cJSON_ArrayForEach(id,assigned) {
        if (!cJSON_IsString(id) || !valid_id(id->valuestring)) goto invalid;
        const cJSON *role=cJSON_GetObjectItemCaseSensitive(roles,id->valuestring);
        if (!role) continue; /* Backend ignores roles that were deleted. */
        const cJSON *rank=cJSON_GetObjectItemCaseSensitive(role,"rank");
        if (!cJSON_IsNumber(rank) || !isfinite(rank->valuedouble) ||
            floor(rank->valuedouble)!=rank->valuedouble) goto invalid;
        ordered[count++]=(RankedRole){role,id->valuestring,rank->valuedouble};
    }
    qsort(ordered,count,sizeof(*ordered),rank_order);
    /* Backend applies large rank first, small rank last, for server and channel. */
    for(size_t i=0;i<count;i++)
        if (!apply(&view,cJSON_GetObjectItemCaseSensitive(ordered[i].role,"permissions"))) goto invalid;
    if (!apply(&view,cJSON_GetObjectItemCaseSensitive(channel,"default_permissions"))) goto invalid;
    const cJSON *overrides=cJSON_GetObjectItemCaseSensitive(channel,"role_permissions");
    if (overrides && !cJSON_IsObject(overrides)) goto invalid;
    for(size_t i=0;i<count;i++)
        if (!apply(&view,cJSON_GetObjectItemCaseSensitive(overrides,ordered[i].id))) goto invalid;
    /* Timeout, voice mute/deafen do not change the ViewChannel bit in v0.15.1. */
    return view;
invalid:
    st_fail(err,"Invalid channel permission metadata"); return -1;
}
static int participant(StUser *out,const cJSON *user,StError *err) {
    const char *id=st_str(user,"_id");
    if (!cJSON_IsObject(user) || !valid_id(id)) return st_fail(err,"Invalid participant response");
    memset(out,0,sizeof(*out)); st_copy(out->id,sizeof(out->id),id);
    const char *name=st_str(user,"display_name"); if (!*name) name=st_str(user,"username");
    st_copy(out->name,sizeof(out->name),*name?name:id);
    st_copy(out->relationship,sizeof(out->relationship),st_str(user,"relationship"));
    st_copy(out->pronouns,sizeof(out->pronouns),st_str(user,"pronouns"));
    StMediaRequest key;st_profile_image_ref(cJSON_GetObjectItemCaseSensitive(user,"avatar"),"avatars",&key);st_copy(out->avatar,sizeof(out->avatar),key.id);st_copy(out->avatar_file,sizeof(out->avatar_file),key.filename);out->avatar_animated=key.animated;
    out->online=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(user,"online"));
    out->presence=st_presence_value(st_str(cJSON_GetObjectItemCaseSensitive(user,"status"),"presence"));return 1;
}
int st_fetch_participants(StClient *c,const char *id,StError *err) {
    if (!valid_id(id)) return st_fail(err,"Invalid channel ID");
    char path[160]; cJSON *channel=NULL,*res=NULL,*server=NULL;
    StUser *people=NULL; size_t count=0; int ok=0;
    snprintf(path,sizeof(path),"/channels/%s",id);
    if (!st_http(c,"GET",path,NULL,NULL,&channel,err)) goto done;
    if (strcmp(st_str(channel,"_id"),id)) {st_fail(err,"Channel ID mismatch");goto done;}
    const char *type=st_str(channel,"channel_type");
    if (!strcmp(type,"Group")) {
        snprintf(path,sizeof(path),"/channels/%s/members",id);
        if (!st_http(c,"GET",path,NULL,NULL,&res,err)) goto done;
        if (!cJSON_IsArray(res) || cJSON_GetArraySize(res)>4096) {
            st_fail(err,"Invalid group members response");goto done;
        }
        size_t n=(size_t)cJSON_GetArraySize(res);
        people=calloc(n?n:1,sizeof(*people));if(!people)goto memory;
        const cJSON *user;cJSON_ArrayForEach(user,res)
            if (!participant(people+count++,user,err)) goto done;
    } else if (!strcmp(type,"DirectMessage") || !strcmp(type,"SavedMessages")) {
        const cJSON *ids=cJSON_GetObjectItemCaseSensitive(channel,"recipients");
        size_t n=!strcmp(type,"SavedMessages")?1:(size_t)cJSON_GetArraySize(ids);
        if (!n || n>2 || (!strcmp(type,"DirectMessage") && !cJSON_IsArray(ids))) {
            st_fail(err,"Invalid direct message participants");goto done;
        }
        people=calloc(n,sizeof(*people));if(!people)goto memory;
        for(size_t i=0;i<n;i++) {
            const cJSON *item=cJSON_GetArrayItem(ids,(int)i);
            const char *uid=!strcmp(type,"SavedMessages")?st_str(channel,"user"):
                            cJSON_IsString(item)?item->valuestring:"";
            if (!valid_id(uid)) {st_fail(err,"Invalid participant ID");goto done;}
            size_t u;for(u=0;u<c->user_count;u++) if(!strcmp(c->users[u].id,uid))break;
            if (u<c->user_count) people[count++]=c->users[u];
            else {
                cJSON_Delete(res);res=NULL;snprintf(path,sizeof(path),"/users/%s",uid);
                if (!st_http(c,"GET",path,NULL,NULL,&res,err)) goto done;
                if (strcmp(st_str(res,"_id"),uid)) {st_fail(err,"Participant ID mismatch");goto done;}
                if (!participant(people+count++,res,err)) goto done;
            }
        }
    } else if (!strcmp(type,"TextChannel")) {
        const char *sid=st_str(channel,"server");
        if (!valid_id(sid)) {st_fail(err,"Invalid server ID");goto done;}
        snprintf(path,sizeof(path),"/servers/%s",sid);
        if (!st_http(c,"GET",path,NULL,NULL,&server,err)) goto done;
        if (strcmp(st_str(server,"_id"),sid)) {st_fail(err,"Server ID mismatch");goto done;}
        if(!st_server_merge(c,server,err))goto done;
        snprintf(path,sizeof(path),"/servers/%s/members",sid);
        if (!st_http(c,"GET",path,NULL,NULL,&res,err)) goto done;
        const cJSON *users=cJSON_GetObjectItemCaseSensitive(res,"users");
        const cJSON *members=cJSON_GetObjectItemCaseSensitive(res,"members");
        if (!cJSON_IsArray(users) || !cJSON_IsArray(members) || cJSON_GetArraySize(users)>4096 ||
            cJSON_GetArraySize(members)!=cJSON_GetArraySize(users)) {
            st_fail(err,"Invalid server members response");goto done;
        }
        size_t n=(size_t)cJSON_GetArraySize(users);
        people=calloc(n?n:1,sizeof(*people));if(!people)goto memory;
        if(!st_members_merge(c,sid,members,1,err))goto done;
        /* v0.15.1 sorts both arrays by user ID; verify each pair rather than guessing. */
        for(size_t i=0;i<n;i++) {
            const cJSON *user=cJSON_GetArrayItem(users,(int)i),*member=cJSON_GetArrayItem(members,(int)i);
            int visible=st_member_can_view(server,channel,member,user,err);
            if(visible<0)goto done;
            if(!visible)continue;
            if(!participant(people+count,user,err)||!st_merge_user(c,user,err))goto done;
            const char *nick=st_str(member,"nickname");
            if(*nick)st_copy(people[count].name,sizeof(people[count].name),nick);
            count++;
        }
    } else {st_fail(err,"Unsupported channel participants");goto done;}
    free(c->participants);c->participants=people;people=NULL;c->participant_count=count;
    st_copy(c->participants_channel,sizeof(c->participants_channel),id);st_participants_sort(c);ok=1;goto done;
memory:
    st_fail(err,"Out of memory");
done:
    free(people);cJSON_Delete(channel);cJSON_Delete(res);cJSON_Delete(server);return ok;
}
