#include "stoat.h"
#include "media.h"
#include "emoji.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *person(const StClient *c,const cJSON *system,const char *key) {
    const char *id=st_str(system,key);return *id?st_user_name(c,id):"Someone";
}
/* ULID encodes a 48-bit UTC millisecond timestamp in its first ten digits. */
static int64_t ulid_time(const char *id) {
    static const char digits[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    if(strlen(id)!=26||id[0]<'0'||id[0]>'7')return -1;
    int64_t value=0;for(int i=0;i<10;i++){const char *p=strchr(digits,id[i]);if(!p)return -1;value=value*32+(p-digits);}return value;
}
static int leap(int y){return y%4==0&&(y%100!=0||y%400==0);}
int st_timestamp_label(int64_t milliseconds,int offset,char out[24]) {
    out[0]=0;if(milliseconds<0||offset<-720||offset>840)return 0;
    time_t seconds=(time_t)(milliseconds/1000+(int64_t)offset*60);struct tm t;
    if(!gmtime_r(&seconds,&t)||t.tm_year<-1900||t.tm_year>8099)return 0;
    int n=snprintf(out,24,"%02d:%02d %02d.%02d.%04d",t.tm_hour,t.tm_min,t.tm_mday,t.tm_mon+1,t.tm_year+1900);
    return n>0&&n<24;
}
static int decimal(const char *p,int count) {
    int value=0;for(int i=0;i<count;i++){if(p[i]<'0'||p[i]>'9')return -1;value=value*10+p[i]-'0';}return value;
}
static int64_t iso_time(const char *text) {
    size_t length=strlen(text);if(length<20||length>35||text[4]!='-'||text[7]!='-'||text[10]!='T'||text[13]!=':'||text[16]!=':')return -1;
    int y=decimal(text,4),m=decimal(text+5,2),d=decimal(text+8,2),h=decimal(text+11,2),min=decimal(text+14,2),s=decimal(text+17,2);
    if(
        y<1970||y>2199||m<1||m>12||d<1||h<0||h>23||min<0||min>59||s<0||s>59)return -1;
    static const int months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(d>months[m-1]+(m==2&&leap(y)))return -1;
    int64_t days=0;for(int year=1970;year<y;year++)days+=365+leap(year);
    for(int month=1;month<m;month++)days+=months[month-1]+(month==2&&leap(y));
    days+=d-1;
    const char *p=text+19;int ms=0;
    if(*p=='.'){p++;int scale=100;if(*p<'0'||*p>'9')return -1;while(*p>='0'&&*p<='9'){ms+=(*p-'0')*scale;scale/=10;p++;}}
    int offset=0;
    if(*p=='Z'){p++;}else if(*p=='+'||*p=='-'){int sign=*p++=='+'?1:-1;if(strlen(p)!=5||p[2]!=':')return -1;int oh=decimal(p,2),om=decimal(p+3,2);if(oh<0||oh>23||om<0||om>59)return -1;p+=5;offset=sign*(oh*60+om)*60;}
    else return -1;
    if(*p)return -1;
    return ((days*24+h)*3600+min*60+s-offset)*1000+ms;
}
static void system_text(const StClient *c,const cJSON *sys,const char *channel,StMessage *m) {
    const char *type=st_str(sys,"type"),*who=person(c,sys,"id"),*by=person(c,sys,"by");
    st_copy(m->system,sizeof(m->system),*type?type:"unknown");
    int server=0;for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,channel))server=c->channels[i].server[0]!=0;
    if(!strcmp(type,"text"))st_copy(m->content,sizeof(m->content),st_str(sys,"content"));
    else if(!strcmp(type,"user_added"))snprintf(m->content,sizeof(m->content),"%s was added by %s.",who,by);
    else if(!strcmp(type,"user_remove"))snprintf(m->content,sizeof(m->content),"%s was removed by %s.",who,by);
    else if(!strcmp(type,"user_joined"))snprintf(m->content,sizeof(m->content),"%s joined the %s.",who,server?"server":"group");
    else if(!strcmp(type,"user_left"))snprintf(m->content,sizeof(m->content),"%s left the %s.",who,server?"server":"group");
    else if(!strcmp(type,"user_kicked")||!strcmp(type,"user_banned"))snprintf(m->content,sizeof(m->content),"%s was %s from the server.",who,!strcmp(type,"user_kicked")?"kicked":"banned");
    else if(!strcmp(type,"channel_renamed"))snprintf(m->content,sizeof(m->content),"%s renamed the %s to %s.",by,server?"channel":"group",st_str(sys,"name"));
    else if(!strcmp(type,"channel_description_changed"))snprintf(m->content,sizeof(m->content),"%s updated the %s description.",by,server?"channel":"group");
    else if(!strcmp(type,"channel_icon_changed"))snprintf(m->content,sizeof(m->content),"%s updated the %s icon.",by,server?"channel":"group");
    else if(!strcmp(type,"channel_ownership_changed"))snprintf(m->content,sizeof(m->content),"%s transferred group ownership to %s.",person(c,sys,"from"),person(c,sys,"to"));
    else if(!strcmp(type,"message_pinned")||!strcmp(type,"message_unpinned"))snprintf(m->content,sizeof(m->content),"%s %s a message.",by,!strcmp(type,"message_pinned")?"pinned":"unpinned");
    else if(!strcmp(type,"call_started")) {
        const char *finished=st_str(sys,"finished_at");int64_t start=ulid_time(m->id),end=iso_time(finished);
        if(*finished&&start>=0&&end>=start&&end-start<=INT64_C(31536000000)) {
            int64_t seconds=(end-start)/1000;
            snprintf(m->content,sizeof(m->content),"%s started a call. Call ended after %lld:%02lld:%02lld.",by,(long long)(seconds/3600),(long long)(seconds/60%60),(long long)(seconds%60));
        } else snprintf(m->content,sizeof(m->content),"%s started a call.%s",by,*finished?" Call ended.":"");
    } else snprintf(m->content,sizeof(m->content),"System event: %s",*type?type:"unknown");
}
void st_message_parse(const StClient *c,const cJSON *o,const char *channel,StMessage *m) {
    memset(m,0,sizeof(*m));st_copy(m->id,sizeof(m->id),st_str(o,"_id"));st_copy(m->author,sizeof(m->author),st_str(o,"author"));
    m->sent_ms=ulid_time(m->id);
    const cJSON *system=cJSON_GetObjectItemCaseSensitive(o,"system");
    if(cJSON_IsObject(system))system_text(c,system,channel,m);
    else {const char *text=st_str(o,"content");st_copy(m->content,sizeof(m->content),*text?text:"[No text]");}
    const cJSON *reply,*refs=cJSON_GetObjectItemCaseSensitive(o,"replies");
    if(cJSON_IsArray(refs))cJSON_ArrayForEach(reply,refs)if(cJSON_IsString(reply)&&reply->valuestring[0]&&strlen(reply->valuestring)<64) {
        m->reply_total++;if(m->reply_count<ST_REPLY_MAX)st_copy(m->replies[m->reply_count++].id,64,reply->valuestring);
    }
    const cJSON *files=cJSON_GetObjectItemCaseSensitive(o,"attachments"),*file;
    m->attachments=cJSON_GetArraySize(files);m->embeds=cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o,"embeds"));
    const cJSON *embed;cJSON_ArrayForEach(embed,cJSON_GetObjectItemCaseSensitive(o,"embeds")) {
        if(m->preview_count==ST_EMBED_MAX)break;
        const char *type=st_str(embed,"type");if(!strcmp(type,"None")||!*type)continue;
        StEmbed *e=m->previews+m->preview_count++;
        st_copy(e->type,sizeof(e->type),type);st_copy(e->title,sizeof(e->title),st_str(embed,"title"));
        st_copy(e->description,sizeof(e->description),st_str(embed,"description"));st_copy(e->url,sizeof(e->url),st_str(embed,"url"));st_copy(e->site,sizeof(e->site),st_str(embed,"site_name"));
        st_role_colour(st_str(embed,"colour"),&e->colour);
        if(!e->title[0])st_copy(e->title,sizeof(e->title),!strcmp(type,"Image")?"Image preview":!strcmp(type,"Video")?"Video preview":e->site[0]?e->site:"Link preview");
    }
    if(!*st_str(o,"content")&&(m->attachments||m->embeds))m->content[0]=0;
    int i=0;cJSON_ArrayForEach(file,files) {
        if(i==ST_ATTACHMENT_MAX)break;
        StAttachment *a=m->files+i++;st_copy(a->id,sizeof(a->id),st_str(file,"_id"));st_copy(a->name,sizeof(a->name),st_str(file,"filename"));
        st_copy(a->type,sizeof(a->type),st_str(cJSON_GetObjectItemCaseSensitive(file,"metadata"),"type"));
        StMediaRequest image;st_profile_image_ref(file,"attachments",&image);a->image=image.id[0]!=0;a->animated=image.animated;
        if(a->image&&!m->image[0])st_copy(m->image,sizeof(m->image),image.id);
    }
    const cJSON *reaction;cJSON_ArrayForEach(reaction,cJSON_GetObjectItemCaseSensitive(o,"reactions")) {
        size_t used=strlen(m->reactions);if(used<sizeof(m->reactions)-1)snprintf(m->reactions+used,sizeof(m->reactions)-used,"%s%s:%d",used?"  ":"",reaction->string?reaction->string:"?",cJSON_GetArraySize(reaction));
        if(m->reaction_count<ST_REACTION_MAX&&reaction->string&&strlen(reaction->string)<129&&cJSON_IsArray(reaction)) {
            StReaction *r=m->reaction_list+m->reaction_count++;st_copy(r->key,sizeof(r->key),reaction->string);r->index=st_emoji_index(r->key);r->count=(unsigned)cJSON_GetArraySize(reaction);
            const cJSON *u;cJSON_ArrayForEach(u,reaction)if(cJSON_IsString(u)&&!strcmp(u->valuestring,c->self_id))r->mine=1;
        }
    }
}
static void quote_message(StQuote *q,const StClient *c,const StMessage *m) {
    st_copy(q->author,sizeof(q->author),m->author);st_copy(q->name,sizeof(q->name),m->system[0]?"System":st_user_name(c,m->author));
    st_copy(q->content,sizeof(q->content),m->content);q->state=1;q->retry=0;
}
void st_quotes_resolve(StClient *c,StMessage *messages,size_t count,const char *channel) {
    for(size_t i=0;i<count;i++)for(int j=0;j<messages[i].reply_count;j++) {
        StQuote *q=messages[i].replies+j;int found=0;
        for(size_t k=0;k<count;k++)if(!strcmp(messages[k].id,q->id)){quote_message(q,c,messages+k);found=1;break;}
        if(found||strcmp(channel,c->messages_channel))continue;
        for(size_t k=0;k<c->message_count&&!found;k++) {
            for(int r=0;r<c->messages[k].reply_count;r++)if(!strcmp(c->messages[k].replies[r].id,q->id)) {
                *q=c->messages[k].replies[r];found=1;break;
            }
        }
    }
}
void st_quotes_invalidate(StClient *c,const char *id,int deleted) {
    for(size_t i=0;i<c->message_count;i++)for(int j=0;j<c->messages[i].reply_count;j++)if(!strcmp(c->messages[i].replies[j].id,id)) {
        StQuote *q=c->messages[i].replies+j;q->state=deleted?2:0;q->retry=0;q->content[0]=q->name[0]=0;
    }
}
int st_fetch_quote(StClient *c,const char *channel,const char *id,StQuote *q,StError *err) {
    memset(q,0,sizeof(*q));st_copy(q->id,sizeof(q->id),id);
    if(!*id||strlen(id)>=64||strspn(id,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")!=strlen(id)||
       !*channel||strlen(channel)>=64||strspn(channel,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")!=strlen(channel))return st_fail(err,"Invalid quote ID");
    char path[180];snprintf(path,sizeof(path),"/channels/%s/messages/%s",channel,id);cJSON *raw=NULL;
    if(!st_http(c,"GET",path,NULL,NULL,&raw,err)){if(err->status==403||err->status==404){q->state=2;return 1;}return 0;}
    if(!cJSON_IsObject(raw)||strcmp(st_str(raw,"_id"),id)||strcmp(st_str(raw,"channel"),channel)){cJSON_Delete(raw);return st_fail(err,"Invalid quoted message");}
    StMessage message;st_message_parse(c,raw,channel,&message);quote_message(q,c,&message);cJSON_Delete(raw);
    if(q->author[0]&&strspn(q->author,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")==strlen(q->author)&&strcmp(q->name,"System")) {
        snprintf(path,sizeof(path),"/users/%s",q->author);cJSON *user=NULL;StError optional={0};
        if(st_http(c,"GET",path,NULL,NULL,&user,&optional)&&!strcmp(st_str(user,"_id"),q->author))st_copy(q->name,sizeof(q->name),st_str(user,"username"));
        cJSON_Delete(user);if(optional.status==401){*err=optional;return 0;}
    }
    return 1;
}
const StReaction *st_message_reaction(const StMessage *m,int emoji) {
    if(emoji<0)return NULL;
    const StReaction *found=NULL;
    for(int i=0;i<m->reaction_count;i++)if(m->reaction_list[i].index==emoji){if(m->reaction_list[i].mine)return m->reaction_list+i;found=m->reaction_list+i;}
    return found;
}

const StReaction *st_message_reaction_key(const StMessage *m,const char *key) {
    for(int i=0;i<m->reaction_count;i++)if(!strcmp(m->reaction_list[i].key,key))return m->reaction_list+i;
    int index=st_emoji_index(key);return index>=0?st_message_reaction(m,index):NULL;
}
