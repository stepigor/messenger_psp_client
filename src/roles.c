#include "stoat.h"
#include "media.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>

static int id_valid(const char *s){size_t n=strlen(s);if(!n||n>=64)return 0;for(;*s;s++)if(!isalnum((unsigned char)*s)&&*s!='_'&&*s!='-')return 0;return 1;}
void st_server_file(const cJSON *file,const char *tag,StFile *out) {
    StMediaRequest key;st_profile_image_ref(file,tag,&key);memset(out,0,sizeof(*out));
    st_copy(out->id,sizeof(out->id),key.id);st_copy(out->filename,sizeof(out->filename),key.filename);out->animated=key.animated;
}
int st_roles_parse(const cJSON *servers,StRole **out,size_t *count,StError *err) {
    *out=NULL;*count=0;const cJSON *s,*r;size_t n=0;
    cJSON_ArrayForEach(s,servers) {
        const cJSON *roles=cJSON_GetObjectItemCaseSensitive(s,"roles");
        if(roles&&!cJSON_IsObject(roles))return st_fail(err,"Invalid server roles");
        if(cJSON_GetArraySize(roles)>256)return st_fail(err,"Server role limit exceeded");
        n+=(size_t)cJSON_GetArraySize(roles);if(n>ST_ROLES_MAX)return st_fail(err,"Account role limit exceeded");
    }
    StRole *result=calloc(n?n:1,sizeof(*result));if(!result)return st_fail(err,"Out of memory loading roles");
    size_t i=0;
    cJSON_ArrayForEach(s,servers)cJSON_ArrayForEach(r,cJSON_GetObjectItemCaseSensitive(s,"roles")) {
        const cJSON *rank=cJSON_GetObjectItemCaseSensitive(r,"rank");
        if(!r->string||!id_valid(r->string)||!id_valid(st_str(s,"_id"))||!cJSON_IsObject(r)||
           (rank&&(!cJSON_IsNumber(rank)||!isfinite(rank->valuedouble)||floor(rank->valuedouble)!=rank->valuedouble||fabs(rank->valuedouble)>9007199254740991.0))) {
            free(result);return st_fail(err,"Invalid role metadata");
        }
        StRole *role=result+i++;st_copy(role->id,64,r->string);st_copy(role->server,64,st_str(s,"_id"));
        st_copy(role->name,sizeof(role->name),st_str(r,"name"));st_copy(role->colour,sizeof(role->colour),st_str(r,"colour"));
        role->rank=rank?(int64_t)rank->valuedouble:0;role->hoist=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"hoist"));
        st_server_file(cJSON_GetObjectItemCaseSensitive(r,"icon"),"icons",&role->icon);
    }
    *out=result;*count=i;return 1;
}
void st_members_clear(StClient *c){free(c->members);free(c->member_roles);c->members=NULL;c->member_roles=NULL;c->member_count=c->member_role_count=0;c->members_server[0]=0;}
int st_server_merge(StClient *c,const cJSON *server,StError *err) {
    const char *id=st_str(server,"_id");if(!id_valid(id))return st_fail(err,"Invalid server metadata");
    if(cJSON_HasObjectItem(server,"roles")) {
        cJSON *array=cJSON_CreateArray();if(!array)return st_fail(err,"Out of memory");
        if(!cJSON_AddItemReferenceToArray(array,(cJSON *)server)){cJSON_Delete(array);return st_fail(err,"Out of memory");}
        StRole *fresh=NULL;size_t n=0;int ok=st_roles_parse(array,&fresh,&n,err);cJSON_Delete(array);if(!ok)return 0;
        size_t old=0;for(size_t i=0;i<c->role_count;i++)if(strcmp(c->roles[i].server,id))old++;
        if(old+n>ST_ROLES_MAX){free(fresh);return st_fail(err,"Account role limit exceeded");}
        StRole *roles=calloc(old+n?old+n:1,sizeof(*roles));if(!roles){free(fresh);return st_fail(err,"Out of memory");}
        size_t at=0;for(size_t i=0;i<c->role_count;i++)if(strcmp(c->roles[i].server,id))roles[at++]=c->roles[i];
        if(n)memcpy(roles+at,fresh,n*sizeof(*fresh));
        free(fresh);free(c->roles);c->roles=roles;c->role_count=old+n;
    }
    for(size_t i=0;i<c->server_count;i++)if(!strcmp(c->servers[i].id,id)) {
        StServer *s=c->servers+i;
        if(cJSON_HasObjectItem(server,"name"))st_copy(s->name,sizeof(s->name),st_str(server,"name"));
        if(cJSON_HasObjectItem(server,"banner"))st_server_file(cJSON_GetObjectItemCaseSensitive(server,"banner"),"banners",&s->banner);
        break;
    }
    return 1;
}
const StMember *st_member(const StClient *c,const char *server,const char *user) {
    if(!*server||strcmp(server,c->members_server))return NULL;
    for(size_t i=0;i<c->member_count;i++)if(!strcmp(c->members[i].user,user))return c->members+i;
    return NULL;
}
int st_members_merge(StClient *c,const char *server,const cJSON *members,int mode,StError *err) {
    if(!id_valid(server)||!cJSON_IsArray(members)||cJSON_GetArraySize(members)>4096)return st_fail(err,"Invalid server members");
    char server_id[64];st_copy(server_id,64,server);server=server_id;
    /* Validate before altering the cache, including role-pool bounds. */
    const cJSON *m,*id;size_t added=0;
    cJSON_ArrayForEach(m,members) {
        const cJSON *key=cJSON_GetObjectItemCaseSensitive(m,"_id"),*roles=cJSON_GetObjectItemCaseSensitive(m,"roles");
        if(strcmp(st_str(key,"server"),server)||!id_valid(st_str(key,"user"))||(roles&&!cJSON_IsArray(roles))||cJSON_GetArraySize(roles)>256)return st_fail(err,"Invalid member identity or roles");
        cJSON_ArrayForEach(id,roles)if(!cJSON_IsString(id)||!id_valid(id->valuestring))return st_fail(err,"Invalid member role ID");
        added+=(size_t)cJSON_GetArraySize(roles);if(added>ST_MEMBER_ROLES_MAX)return st_fail(err,"Member role limit exceeded");
    }
    if(mode==ST_MEMBERS_REPLACE||strcmp(c->members_server,server))st_members_clear(c);
    st_copy(c->members_server,64,server);
    cJSON_ArrayForEach(m,members) {
        const char *user=st_str(cJSON_GetObjectItemCaseSensitive(m,"_id"),"user");size_t index=0;
        for(;index<c->member_count;index++)if(!strcmp(c->members[index].user,user))break;
        if(index==c->member_count) {
            if(index==4096)return st_fail(err,"Member limit exceeded");
            StMember *p=realloc(c->members,(index+1)*sizeof(*p));if(!p)return st_fail(err,"Out of memory loading members");
            c->members=p;memset(p+index,0,sizeof(*p));p[index].role_start=c->member_role_count;c->member_count++;
            st_copy(p[index].user,64,user);
        }
        StMember *member=c->members+index;
        if(cJSON_HasObjectItem(m,"nickname")||mode!=ST_MEMBERS_PATCH)st_copy(member->nickname,sizeof(member->nickname),st_str(m,"nickname"));
        if(cJSON_HasObjectItem(m,"pronouns")||mode!=ST_MEMBERS_PATCH){const cJSON *value=cJSON_GetObjectItemCaseSensitive(m,"pronouns");member->has_pronouns=cJSON_IsString(value);st_copy(member->pronouns,sizeof(member->pronouns),st_str(m,"pronouns"));}
        const cJSON *roles=cJSON_GetObjectItemCaseSensitive(m,"roles");if(!roles&&mode==ST_MEMBERS_PATCH)continue;
        size_t old=member->role_count,n=(size_t)cJSON_GetArraySize(roles),start=member->role_start,total=c->member_role_count-old+n;
        if(total>ST_MEMBER_ROLES_MAX)return st_fail(err,"Member role limit exceeded");
        if(n>old){void *p=realloc(c->member_roles,total*sizeof(*c->member_roles));if(!p)return st_fail(err,"Out of memory loading member roles");c->member_roles=p;}
        if(c->member_role_count>start+old)memmove(c->member_roles+start+n,c->member_roles+start+old,(c->member_role_count-start-old)*sizeof(*c->member_roles));
        for(size_t i=0;i<c->member_count;i++)if(i!=index&&c->members[i].role_start>=start+old)c->members[i].role_start=c->members[i].role_start-old+n;
        member->role_count=n;c->member_role_count=total;size_t i=0;
        cJSON_ArrayForEach(id,roles)st_copy(c->member_roles[start+i++],64,id->valuestring);
    }
    return 1;
}
void st_member_remove(StClient *c,const char *server,const char *user) {
    if(strcmp(server,c->members_server))return;
    for(size_t i=0;i<c->member_count;i++)if(!strcmp(c->members[i].user,user)) {
        size_t start=c->members[i].role_start,n=c->members[i].role_count;
        if(c->member_role_count>start+n)memmove(c->member_roles+start,c->member_roles+start+n,(c->member_role_count-start-n)*sizeof(*c->member_roles));
        c->member_role_count-=n;
        for(size_t j=0;j<c->member_count;j++)if(j!=i&&c->members[j].role_start>=start+n)c->members[j].role_start-=n;
        memmove(c->members+i,c->members+i+1,(c->member_count-i-1)*sizeof(*c->members));c->member_count--;break;
    }
}
static int role_order(const void *a,const void *b) {
    const StRole *x=*(const StRole *const *)a,*y=*(const StRole *const *)b;
    return x->rank<y->rank?-1:x->rank>y->rank?1:strcmp(x->id,y->id);
}
int st_member_roles(const StClient *c,const char *server,const char *user,const StRole *out[256]) {
    const StMember *m=st_member(c,server,user);int n=0;if(!m)return 0;
    for(size_t i=0;i<m->role_count;i++)for(size_t j=0;j<c->role_count;j++) {
        const StRole *r=c->roles+j;if(strcmp(r->server,server)||strcmp(r->id,c->member_roles[m->role_start+i]))continue;
        int duplicate=0;for(int k=0;k<n;k++)if(out[k]==r)duplicate=1;
        if(!duplicate&&n<256)out[n++]=r;
        break;
    }
    qsort(out,(size_t)n,sizeof(*out),role_order);return n;
}
/* 0 = colour, 1 = icon, 2 = hoisted role. The three are selected independently. */
const StRole *st_member_role(const StClient *c,const char *server,const char *user,int field) {
    const StRole *roles[256];int n=st_member_roles(c,server,user,roles);
    for(int i=0;i<n;i++)if(field==0?roles[i]->colour[0]!=0:field==1?roles[i]->icon.id[0]!=0:roles[i]->hoist)return roles[i];
    return NULL;
}
const char *st_context_name(const StClient *c,const char *server,const char *user){const StMember *m=st_member(c,server,user);return m&&m->nickname[0]?m->nickname:st_user_name(c,user);}
const char *st_context_pronouns(const StClient *c,const char *server,const char *user) {
    const StMember *m=st_member(c,server,user);if(m&&m->has_pronouns)return m->pronouns;
    for(size_t i=0;i<c->user_count;i++)if(!strcmp(c->users[i].id,user))return c->users[i].pronouns;
    return "";
}
static int person_order(const void *a,const void *b) {
    const StUser *x=a,*y=b;int gx=x->member_group>0?0:x->member_group==-1?1:2,gy=y->member_group>0?0:y->member_group==-1?1:2;
    if(gx!=gy)return gx-gy;
    if(x->member_rank!=y->member_rank)return x->member_rank<y->member_rank?-1:1;
    if(x->member_group!=y->member_group)return x->member_group<y->member_group?-1:1;
    int names=strcasecmp(x->name,y->name);return names?names:strcmp(x->id,y->id);
}
void st_participants_sort(StClient *c) {
    const char *server="";for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,c->participants_channel))server=c->channels[i].server;
    for(size_t i=0;i<c->participant_count;i++) {
        StUser *u=c->participants+i;const StMember *m=st_member(c,server,u->id);
        for(size_t j=0;j<c->user_count;j++)if(!strcmp(c->users[j].id,u->id)){*u=c->users[j];break;}
        if(m&&m->nickname[0])st_copy(u->name,sizeof(u->name),m->nickname);
        u->member_group=0;u->member_rank=0;
        if(*server) {
            const StRole *role=u->online?st_member_role(c,server,u->id,2):NULL;
            u->member_group=role?(int)(role-c->roles)+1:u->online?-1:-2;u->member_rank=role?role->rank:0;
        }
    }
    if(*server&&c->participant_count>1)qsort(c->participants,c->participant_count,sizeof(*c->participants),person_order);
}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static double clamp01(double n){return n<0?0:n>1?1:n;}
static unsigned rgba(double r,double g,double b,double a){return (unsigned)(clamp01(r)*255+.5)|((unsigned)(clamp01(g)*255+.5)<<8)|((unsigned)(clamp01(b)*255+.5)<<16)|((unsigned)(clamp01(a)*255+.5)<<24);}
static int solid_colour(const char *text,unsigned *colour) {
    while(isspace((unsigned char)*text))text++;
    size_t n=strlen(text);while(n&&isspace((unsigned char)text[n-1]))n--;
    if(n>128)return 0;
    char normalized[129];for(size_t i=0;i<n;i++)normalized[i]=(char)tolower((unsigned char)text[i]);normalized[n]=0;text=normalized;
    unsigned r=0,g=0,b=0,a=255;
    if(*text=='#'&&(n==4||n==5||n==7||n==9)) {
        for(size_t i=1;i<n;i++)if(hex(text[i])<0)return 0;
        if(n<6){r=(unsigned)hex(text[1])*17;g=(unsigned)hex(text[2])*17;b=(unsigned)hex(text[3])*17;}
        else {r=(unsigned)(hex(text[1])*16+hex(text[2]));g=(unsigned)(hex(text[3])*16+hex(text[4]));b=(unsigned)(hex(text[5])*16+hex(text[6]));}
        if(n==5)a=(unsigned)hex(text[4])*17;
        if(n==9)a=(unsigned)(hex(text[7])*16+hex(text[8]));
    } else {
        static const struct {const char *name;unsigned value;} names[]={
#include "css_colours.inc"
        };
        for(size_t i=0;i<sizeof(names)/sizeof(*names);i++)if(strlen(names[i].name)==n&&!strncasecmp(text,names[i].name,n)){*colour=names[i].value;return 1;}
        if(n==11&&!strncasecmp(text,"transparent",n)){*colour=0;return 1;}
        int hsl=!strncasecmp(text,"hsl(",4)||!strncasecmp(text,"hsla(",5),rgb=!strncasecmp(text,"rgb(",4)||!strncasecmp(text,"rgba(",5);
        if(!hsl&&!rgb)return 0;
        const char *p=strchr(text,'(')+1;double values[4]={0,0,0,1};int percent[4]={0},count=0;
        while(count<4) {
            while(isspace((unsigned char)*p))p++;
            char *end;double v=strtod(p,&end);if(end==p||!isfinite(v))return 0;p=end;
            if(hsl&&count==0){if(!strncmp(p,"deg",3))p+=3;else if(!strncmp(p,"turn",4)){v=fmod(v,1)*360;p+=4;}else if(!strncmp(p,"rad",3)){v=fmod(v,2*3.141592653589793)*180/3.141592653589793;p+=3;}else if(!strncmp(p,"grad",4)){v=fmod(v,400)*.9;p+=4;}}
            if(*p=='%'){percent[count]=1;p++;}values[count++]=v;
            const char *space=p;while(isspace((unsigned char)*p))p++;
            if(*p==')'){p++;break;}
            if(*p==','||*p=='/')p++;else if(p==space)return 0;
        }
        while(isspace((unsigned char)*p))p++;
        if(*p||text[n-1]!=')'||count<3||count>4)return 0;
        double alpha=values[3]/(percent[3]?100:1);
        if(rgb){*colour=rgba(values[0]/(percent[0]?100:255),values[1]/(percent[1]?100:255),values[2]/(percent[2]?100:255),alpha);return 1;}
        if(!percent[1]||!percent[2]||percent[0])return 0;
        double h=fmod(values[0],360);if(h<0)h+=360;
        double s=clamp01(values[1]/100),l=clamp01(values[2]/100),c=(1-fabs(2*l-1))*s,x=c*(1-fabs(fmod(h/60,2)-1)),m=l-c/2;
        double rr=0,gg=0,bb=0;
        if(h<60){rr=c;gg=x;}else if(h<120){rr=x;gg=c;}else if(h<180){gg=c;bb=x;}else if(h<240){gg=x;bb=c;}else if(h<300){rr=x;bb=c;}else{rr=c;bb=x;}
        *colour=rgba(rr+m,gg+m,bb+m,alpha);return 1;
    }
    *colour=(a<<24)|r|(g<<8)|(b<<16);return 1;
}
int st_role_gradient(const char *text,StRoleGradient *out) {
    memset(out,0,sizeof(*out));while(isspace((unsigned char)*text))text++;
    if(strncasecmp(text,"linear-gradient(",16)||strlen(text)>128)return 0;
    char buffer[129];st_copy(buffer,sizeof(buffer),text+16);size_t len=strlen(buffer);while(len&&isspace((unsigned char)buffer[len-1]))buffer[--len]=0;
    if(!len||buffer[len-1]!=')')return 0;
    buffer[--len]=0;
    for(size_t i=0;i<len;i++)buffer[i]=(char)tolower((unsigned char)buffer[i]);
    char *parts[9];int n=1,depth=0;parts[0]=buffer;
    for(char *p=buffer;*p;p++){if(*p=='(')depth++;if(*p==')'&&--depth<0)return 0;if(*p==','&&!depth){if(n==9)return 0;*p=0;parts[n++]=p+1;}}
    if(depth||n<2)return 0;
    out->dy=1;int first=0;
    char *direction=parts[0];while(isspace((unsigned char)*direction))direction++;
    size_t ds=strlen(direction);while(ds&&isspace((unsigned char)direction[ds-1]))direction[--ds]=0;
    if(!strncasecmp(direction,"to ",3)) {
        out->dx=strstr(direction,"right")?1:strstr(direction,"left")?-1:0;out->dy=strstr(direction,"bottom")?1:strstr(direction,"top")?-1:0;
        if(!out->dx&&!out->dy)return 0;
        first=1;
    }else {
        char *end;double angle=strtod(direction,&end);while(isspace((unsigned char)*end))end++;
        if(end!=direction&&(!strcmp(end,"deg")||!strcmp(end,"turn")||!strcmp(end,"rad"))) {
            if(!isfinite(angle))return 0;
            if(!strcmp(end,"turn"))angle=fmod(angle,1)*360;else if(!strcmp(end,"rad"))angle=fmod(angle,2*3.141592653589793)*180/3.141592653589793;else angle=fmod(angle,360);
            out->dx=(float)sin(angle*3.141592653589793/180);out->dy=(float)-cos(angle*3.141592653589793/180);first=1;
        }
    }
    for(int i=first;i<n;i++) {
        char *p=parts[i];while(isspace((unsigned char)*p))p++;size_t size=strlen(p);while(size&&isspace((unsigned char)p[size-1]))p[--size]=0;
        float at=-1;char *space=strrchr(p,' ');
        if(space&&size&&p[size-1]=='%'){char *end;double value=strtod(space+1,&end);if(end!=space+1&&*end=='%'&&!end[1]&&isfinite(value)){at=(float)clamp01(value/100);*space=0;}}
        int index=out->count;if(index==8||!solid_colour(p,out->colours+index))return 0;
        out->stops[index]=at;out->count++;
    }
    if(out->count<2)return 0;
    if(out->stops[0]<0)out->stops[0]=0;
    if(out->stops[out->count-1]<0)out->stops[out->count-1]=1;
    int previous=0;
    for(int i=1;i<out->count;i++)if(out->stops[i]>=0) {
        if(out->stops[i]<out->stops[previous])out->stops[i]=out->stops[previous];
        for(int j=previous+1;j<i;j++)out->stops[j]=out->stops[previous]+(out->stops[i]-out->stops[previous])*(float)(j-previous)/(float)(i-previous);
        previous=i;
    }
    return 1;
}
int st_role_colour(const char *text,unsigned *colour){StRoleGradient g;if(st_role_gradient(text,&g)){*colour=g.colours[0];return 1;}return solid_colour(text,colour);}
