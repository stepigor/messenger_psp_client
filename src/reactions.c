#include "reactions.h"
#include "emoji.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define PICKER_MAX (4096+ST_CUSTOM_EMOJI_MAX)
static char recent[ST_RECENT_REACTIONS][129];static int recent_count;
static uint16_t order[PICKER_MAX],positions[PICKER_MAX],groups[PICKER_MAX];
static int initialized,total;
static const StClient *catalog;
static const StCustomEmoji *bound_emojis;static const StServer *bound_servers;static size_t bound_count,bound_server_count;
static const char *const popular[]={"👍","👎","💯","😂","🤣","😭","😢","😡","😠","❤️","🔥","😍","🎉","✅","🤔","🙏","👀","😎","🥳","🤡","💀","🙂","😊","😅","😆","😄","😁","😘","🥰","😋","😜","🤪","🙃","😉","😏","😔","😞","🥺","😱","😳","🤯","🤩","🤗","🤭","🤫","🤷","🤦","🫡","🫠","🤝","👏","🙌","💪","👌","✌️","🤞","🤟","🫶","💔","💕","💖","✨","⭐","🌟","💥","💩","💋","💤","🍿","☕","🍺","🍻","🥂","🍷","🎂","🎁","🚀","🏆","⚡","❗","❓","❌","🆗","🆒"};

const char *st_reactions_key(int e) {
    size_t unicode=st_emoji_count();
    if(e>=0&&(size_t)e<unicode)return st_emoji_text((size_t)e);
    if(e>=0&&catalog&&(size_t)e>=unicode&&(size_t)e-unicode<catalog->emoji_count)return catalog->emojis[(size_t)e-unicode].id;
    return "";
}
static int key_index(const char *key) {
    int e=st_emoji_index(key);if(e>=0)return e;
    if(catalog)for(size_t i=0;i<catalog->emoji_count;i++)if(!strcmp(catalog->emojis[i].id,key))return (int)(st_emoji_count()+i);
    return -1;
}
static void append(int e,int group,unsigned char *seen) {
    if(e<0||e>=PICKER_MAX||seen[e])return;
    order[total]=(uint16_t)e;groups[total]=(uint16_t)group;positions[e]=(uint16_t)total;total++;seen[e]=1;
}
static void rebuild(void) {
    unsigned char seen[PICKER_MAX]={0};total=0;
    for(int i=0;i<recent_count;i++)append(key_index(recent[i]),0,seen);
    if(catalog)for(size_t server=0;server<catalog->server_count;server++)for(size_t i=0;i<catalog->emoji_count;i++)
        if(!strcmp(catalog->emojis[i].server,catalog->servers[server].id))append((int)(st_emoji_count()+i),(int)server+1,seen);
    int standard=catalog?(int)catalog->server_count+1:1;
    for(size_t i=0;i<sizeof(popular)/sizeof(popular[0]);i++)append(st_emoji_index(popular[i]),standard,seen);
    for(size_t i=0;i<st_emoji_count();i++)append((int)i,standard,seen);
    initialized=1;
}
void st_reactions_bind(const StClient *c) {
    catalog=c;
    if(!initialized||bound_emojis!=(c?c->emojis:NULL)||bound_servers!=(c?c->servers:NULL)||bound_count!=(c?c->emoji_count:0)||bound_server_count!=(c?c->server_count:0)) {
        bound_emojis=c?c->emojis:NULL;bound_servers=c?c->servers:NULL;bound_count=c?c->emoji_count:0;bound_server_count=c?c->server_count:0;rebuild();
    }
}
void st_reactions_reset(void){recent_count=0;rebuild();}
int st_reactions_count(void){if(!initialized)rebuild();return total;}
int st_reactions_at(int p){if(!initialized)rebuild();return p>=0&&p<total?order[p]:-1;}
int st_reactions_position(int e){if(!initialized)rebuild();return e>=0&&e<PICKER_MAX&&*st_reactions_key(e)?positions[e]:-1;}
const char *st_reactions_group(int p) {
    if(!initialized)rebuild();
    if(p<0||p>=total)return "";
    unsigned group=groups[p];if(!group)return "Recently used";
    if(catalog&&group<=catalog->server_count)return catalog->servers[group-1].name;
    return "Standard emoji";
}
int st_reactions_group_id(int p){if(!initialized)rebuild();return p>=0&&p<total?groups[p]:-1;}
int st_reactions_move(int position,int columns,int dx,int dy,int page) {
    int count=st_reactions_count();if(!count||columns<1)return 0;
    position+=dx;if(position<0)position=0;if(position>=count)position=count-1;
    if(!dy&&!page)return position;
    int row=0,column=0,last=-1,current_row=0,current_column=0,last_row=0;
    for(int i=0;i<count;i++) {
        int group=st_reactions_group_id(i);
        if(group!=last){if(column)row++;column=0;last=group;}
        if(i==position){current_row=row;current_column=column;}
        last_row=row;
        if(++column==columns){column=0;row++;}
    }
    int target=current_row+dy+page*4;if(target<0)target=0;if(target>last_row)target=last_row;
    row=column=0;last=-1;int selected=position;
    for(int i=0;i<count;i++) {
        int group=st_reactions_group_id(i);
        if(group!=last){if(column)row++;column=0;last=group;}
        if(row>target)break;
        if(row==target&&column<=current_column)selected=i;
        if(++column==columns){column=0;row++;}
    }
    return selected;
}
void st_reactions_used_key(const char *key) {
    int e=st_emoji_index(key);if(e>=0)key=st_emoji_text((size_t)e);
    else if(!st_custom_emoji_id(key))return;
    char value[129];st_copy(value,sizeof(value),key);
    int found=recent_count;for(int i=0;i<recent_count;i++)if(!strcmp(recent[i],value)){found=i;break;}
    if(found==recent_count&&recent_count<ST_RECENT_REACTIONS)recent_count++;
    if(found>=ST_RECENT_REACTIONS)found=ST_RECENT_REACTIONS-1;
    for(int i=found;i>0;i--)st_copy(recent[i],sizeof(recent[i]),recent[i-1]);
    st_copy(recent[0],sizeof(recent[0]),value);rebuild();
}
void st_reactions_used(int e){const char *key=st_reactions_key(e);if(*key)st_reactions_used_key(key);}
int st_reactions_load(const char *path,StError *err) {
    st_reactions_reset();char tmp[600],data[4096];
    if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return st_fail(err,"Recent reactions path too long"),-1;
    FILE *f=fopen(tmp,"rb");if(!f){if(errno!=ENOENT)return st_fail(err,"Cannot read pending recent reactions"),-1;f=fopen(path,"rb");}
    if(!f)return errno==ENOENT?0:(st_fail(err,"Cannot read recent reactions"),-1);
    size_t n=fread(data,1,sizeof(data)-1,f);int ok=!ferror(f)&&n<sizeof(data)-1&&fgetc(f)==EOF;fclose(f);data[n]=0;
    cJSON *r=ok?cJSON_ParseWithLengthOpts(data,n+1,NULL,1):NULL;const cJSON *v=cJSON_GetObjectItemCaseSensitive(r,"format"),*items=cJSON_GetObjectItemCaseSensitive(r,"emoji"),*item;
    ok=cJSON_IsNumber(v)&&(v->valuedouble==1||v->valuedouble==2)&&cJSON_IsArray(items)&&cJSON_GetArraySize(items)<=ST_RECENT_REACTIONS;
    char parsed[ST_RECENT_REACTIONS][129];int count=0;
    if(ok)cJSON_ArrayForEach(item,items) {
        if(!cJSON_IsString(item)){ok=0;break;}
        const char *key=item->valuestring;int e=st_emoji_index(key);if(e>=0)key=st_emoji_text((size_t)e);
        else if(v->valuedouble!=2||!st_custom_emoji_id(key))continue;
        int seen=0;for(int i=0;i<count;i++)if(!strcmp(parsed[i],key))seen=1;
        if(!seen)st_copy(parsed[count++],sizeof(parsed[0]),key);
    }
    if(ok){memcpy(recent,parsed,(size_t)count*sizeof(*recent));recent_count=count;rebuild();}
    cJSON_Delete(r);return ok?1:(st_fail(err,"Invalid recent reactions; defaults restored"),-1);
}
int st_reactions_save(const char *path,StError *err) {
    char tmp[600];if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return st_fail(err,"Recent reactions path too long");
    cJSON *r=cJSON_CreateObject(),*items=cJSON_CreateArray();
    if(!r||!items){cJSON_Delete(r);cJSON_Delete(items);return st_fail(err,"Cannot save recent reactions");}
    cJSON_AddNumberToObject(r,"format",2);cJSON_AddItemToObject(r,"emoji",items);
    for(int i=0;i<recent_count;i++)cJSON_AddItemToArray(items,cJSON_CreateString(recent[i]));
    char *data=cJSON_PrintUnformatted(r);cJSON_Delete(r);if(!data)return st_fail(err,"Cannot save recent reactions");
    FILE *f=fopen(tmp,"wb");size_t n=strlen(data);int ok=f&&fwrite(data,1,n,f)==n;if(f&&fclose(f))ok=0;free(data);
    if(!ok){if(f)remove(tmp);return st_fail(err,"Cannot save recent reactions; retained in memory");}
    if(remove(path)&&errno!=ENOENT)return st_fail(err,"Cannot replace recent reactions; pending copy retained");
    if(rename(tmp,path))return st_fail(err,"Cannot rename recent reactions; pending copy retained");
    return 1;
}
