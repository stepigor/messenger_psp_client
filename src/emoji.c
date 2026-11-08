#include "emoji.h"
#include "emoji_data.h"
#include "emoji_picker_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
_Static_assert(sizeof(emoji_texts)/sizeof(*emoji_texts)==ST_EMOJI_COUNT,"Regenerate emoji_picker_data.h with the emoji trie");
static int child(unsigned node,uint32_t cp) {
    unsigned lo=emoji_nodes[node].first,hi=lo+emoji_nodes[node].count;
    while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(emoji_edges[mid].cp<cp)lo=mid+1;else hi=mid;}
    unsigned end=emoji_nodes[node].first+emoji_nodes[node].count;
    return lo<end&&emoji_edges[lo].cp==cp?emoji_edges[lo].child:-1;
}
int st_emoji_match(const char **text,char id[64]) {
    const char *cursor=*text,*best=NULL;int index=-1;unsigned node=0;
    for(int i=0;*cursor&&i<32;i++) {
        const char *before=cursor;uint32_t cp=st_utf8_next(&cursor);
        /* Explicit text presentation must never become a coloured pictograph. */
        if(cp==0xfe0e)return 0;
        int next=child(node,cp);
        if(cp==0xfe0f){if(best==before)best=cursor;if(next<0)continue;}
        if(next<0)break;
        node=(unsigned)next;
        if(emoji_nodes[node].emoji>=0){index=emoji_nodes[node].emoji;best=cursor;}
    }
    if(index<0)return 0;
    snprintf(id,64,"%d",index);*text=best;return 1;
}
static int record_index(const char *id) {
    if(!*id||strlen(id)>5)return -1;
    for(const char *p=id;*p;p++)if(*p<'0'||*p>'9')return -1;
    unsigned index=(unsigned)strtoul(id,NULL,10);return index<ST_EMOJI_COUNT?(int)index:-1;
}
int st_emoji_known(const char *id){return record_index(id)>=0;}
size_t st_emoji_count(void){return ST_EMOJI_COUNT;}
const char *st_emoji_text(size_t index){return index<ST_EMOJI_COUNT?emoji_texts[index]:"";}
int st_emoji_index(const char *text){const char *p=text;char id[64];return st_emoji_match(&p,id)&&!*p?(int)strtoul(id,NULL,10):-1;}
void st_emoji_identity(const char *id,char *out,size_t cap){snprintf(out,cap,"twemoji:%s:%s",ST_EMOJI_DIGEST,id);}
int st_emoji_read(const char *path,const char *id,unsigned char **data,size_t *length,StError *err) {
    *data=NULL;*length=0;int index=record_index(id);struct stat st;
    if(index<0)return st_fail(err,"Unsupported emoji");
    if(stat(path,&st)||!S_ISREG(st.st_mode)||st.st_size!=ST_EMOJI_PACK_BYTES)return st_fail(err,"Missing or incompatible emoji.pack");
    FILE *f=fopen(path,"rb");if(!f)return st_fail(err,"Cannot open emoji.pack");
    unsigned char header[44];
    int ok=fread(header,1,sizeof(header),f)==sizeof(header)&&!memcmp(header,"STEMOJI1",8)&&
        ((uint32_t)header[8]|((uint32_t)header[9]<<8)|((uint32_t)header[10]<<16)|((uint32_t)header[11]<<24))==ST_EMOJI_COUNT&&
        !memcmp(header+12,emoji_digest,32);
    EmojiRecord r=emoji_records[index];
    if(ok)ok=r.offset>=44&&r.offset<=ST_EMOJI_PACK_BYTES&&r.length<=ST_EMOJI_PACK_BYTES-r.offset&&!fseek(f,(long)r.offset,SEEK_SET);
    unsigned char *blob=ok?malloc(r.length):NULL;
    if(blob)ok=fread(blob,1,r.length,f)==r.length&&!ferror(f);else ok=0;
    fclose(f);
    if(!ok){free(blob);return st_fail(err,"Invalid emoji.pack; copy the matching pack again");}
    *data=blob;*length=r.length;return 1;
}

/* Custom emoji markup uses :ULID:. */
int st_custom_emoji_id(const char *id) {
    if(strlen(id)!=26)return 0;
    for(int i=0;i<26;i++)if(!strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ",id[i]))return 0;
    return id[0]<='7';
}
int st_custom_emoji_match(const char **text,char id[64]) {
    const char *s=*text;if(*s!=':'||strlen(s)<28||s[27]!=':')return 0;
    char candidate[27];memcpy(candidate,s+1,26);candidate[26]=0;
    if(!st_custom_emoji_id(candidate))return 0;
    st_copy(id,64,candidate);*text=s+28;return 1;
}
