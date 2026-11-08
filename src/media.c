#include "media.h"
#include "emoji.h"
#include "transport.h"
#include "disk_cache.h"
#include <time.h>
#include <webp/decode.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
static int valid_id(const char *id) {
    size_t n=strlen(id);if(!n||n>=64)return 0;
    for(;*id;id++)if(!((*id>='A'&&*id<='Z')||(*id>='a'&&*id<='z')||(*id>='0'&&*id<='9')||*id=='_'||*id=='-'))return 0;
    return 1;
}
static int valid_tag(const char *tag){return !strcmp(tag,"photos")||!strcmp(tag,"avatars")||!strcmp(tag,"icons")||!strcmp(tag,"attachments")||!strcmp(tag,"backgrounds")||!strcmp(tag,"banners")||!strcmp(tag,"emojis")||!strcmp(tag,"default");}
static int animation_tag(const char *tag){return !strcmp(tag,"avatars")||!strcmp(tag,"backgrounds")||!strcmp(tag,"banners")||!strcmp(tag,"icons")||!strcmp(tag,"attachments");}
static int valid_filename(const char *name){return *name&&strlen(name)<129&&!strchr(name,'/')&&!strchr(name,'\\')&&strcmp(name,".")&&strcmp(name,"..");}
void st_image_ref(const cJSON *file,const char *tag,char out[64]) {
    out[0]=0;
    const cJSON *meta=cJSON_GetObjectItemCaseSensitive(file,"metadata");
    if(!cJSON_IsObject(file)||strcmp(st_str(file,"tag"),tag)||!valid_id(st_str(file,"_id"))||
       strcmp(st_str(meta,"type"),"Image")||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(meta,"animated"))||
       !strcmp(st_str(file,"content_type"),"image/gif"))return;
    st_copy(out,64,st_str(file,"_id"));
}
void st_profile_image_ref(const cJSON *file,const char *tag,StMediaRequest *out) {
    memset(out,0,sizeof(*out));const cJSON *meta=cJSON_GetObjectItemCaseSensitive(file,"metadata");
    if(!animation_tag(tag)||strcmp(st_str(file,"tag"),tag)||!valid_id(st_str(file,"_id"))||strcmp(st_str(meta,"type"),"Image"))return;
    const char *type=st_str(file,"content_type");int gif=!strcmp(type,"image/gif");
    st_copy(out->tag,sizeof(out->tag),tag);st_copy(out->id,sizeof(out->id),st_str(file,"_id"));
    int original=gif||(!strcmp(type,"image/webp")&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(meta,"animated")))||!strcmp(type,"image/png")||!strcmp(type,"image/apng");
    if(original&&valid_filename(st_str(file,"filename"))){out->animated=1;st_copy(out->filename,sizeof(out->filename),st_str(file,"filename"));}
}
int st_media_url(const char *base,const StMediaRequest *key,char *out,size_t cap) {
    char host[512],path[512];
    if(!strcmp(key->tag,"photos"))return 0;
    if(!st_url_split(base,"https://",host,sizeof(host),path,sizeof(path))||strchr(base,'?')||!valid_tag(key->tag)||!valid_id(key->id))return 0;
    size_t n=strlen(base);while(n&&base[n-1]=='/')n--;
    if(!strcmp(key->tag,"default"))return snprintf(out,cap,"%.*s/users/%s/default_avatar",(int)n,base,key->id)<(int)cap;
    if(!strcmp(key->tag,"emojis"))return snprintf(out,cap,"%.*s/emojis/%s",(int)n,base,key->id)<(int)cap;
    if(key->animated) {
        if(!animation_tag(key->tag)||!valid_filename(key->filename))return 0;
        char *escaped=curl_easy_escape(NULL,key->filename,0);if(!escaped)return 0;
        int ok=snprintf(out,cap,"%.*s/%s/%s/%s",(int)n,base,key->tag,key->id,escaped)<(int)cap;curl_free(escaped);return ok;
    }
    return snprintf(out,cap,"%.*s/%s/%s",(int)n,base,key->tag,key->id)<(int)cap;
}
void st_image_free(StImage *image){free(image->pixels);memset(image,0,sizeof(*image));}
int st_image_decode(const unsigned char *data,size_t n,int max_width,int max_height,StImage *out,StError *err) {
    memset(out,0,sizeof(*out));WebPDecoderConfig cfg;
    if(max_width<1||max_width>192||max_height<1||max_height>96||!WebPInitDecoderConfig(&cfg)||
       WebPGetFeatures(data,n,&cfg.input)!=VP8_STATUS_OK||cfg.input.has_animation||
       cfg.input.width<1||cfg.input.height<1||cfg.input.width>1280||cfg.input.height>1280)
        return st_fail(err,"Unsupported or oversized static image");
    int w=cfg.input.width,h=cfg.input.height;
    if(w>max_width){h=h*max_width/w;w=max_width;}if(h>max_height){w=w*max_height/h;h=max_height;}
    if(w<1)w=1;
    if(h<1)h=1;
    size_t size=(size_t)w*(size_t)h*4;unsigned *pixels=malloc(size);if(!pixels)return st_fail(err,"Out of memory");
    cfg.options.use_scaling=1;cfg.options.scaled_width=w;cfg.options.scaled_height=h;cfg.options.use_threads=0;
    cfg.output.colorspace=MODE_RGBA;cfg.output.is_external_memory=1;cfg.output.u.RGBA.rgba=(unsigned char *)pixels;
    cfg.output.u.RGBA.stride=w*4;cfg.output.u.RGBA.size=size;
    VP8StatusCode result=WebPDecode(data,n,&cfg);WebPFreeDecBuffer(&cfg.output);
    if(result!=VP8_STATUS_OK){free(pixels);return st_fail(err,"Invalid static WebP image");}
    *out=(StImage){.pixels=pixels,.width=w,.height=h,.frames=1};return 1;
}
static int decode(const StMediaRequest *key,const unsigned char *data,size_t n,StImage *out,StError *err) {
    int banner=!strcmp(key->tag,"backgrounds")||!strcmp(key->tag,"banners"),large=banner||!strcmp(key->tag,"attachments");
    int universal=!strcmp(key->tag,"emojis")||!strcmp(key->tag,"default");
    if(!strcmp(key->tag,"emojis"))return st_profile_image_decode(data,n,18,18,out,err);
    return (universal||(key->animated&&animation_tag(key->tag)))?st_profile_image_decode(data,n,large?192:32,banner?64:large?96:32,out,err):st_image_decode(data,n,large?192:32,banner?64:large?96:32,out,err);
}
int st_media_fetch(const StClient *c,const StMediaRequest *key,StImage *out,StError *err) {
    memset(out,0,sizeof(*out));char url[1200];
    if(!st_media_url(!strcmp(key->tag,"default")?c->config.api_url:c->assets_url,key,url,sizeof(url)))return st_fail(err,"No secure media endpoint");
    unsigned char *data=NULL;size_t n=0;int ok=st_download(&c->config,url,&data,&n,512*1024,5000,err);
    if(ok && st_cancelled())ok=st_fail(err,"Cancelled");
    if(ok){st_stage("Image decode");ok=decode(key,data,n,out,err);}
    free(data);return ok;
}
static int fetch_emoji(const StClient *c,StMediaRequest *key,StImage *out,StError *err) {
    (void)c;st_stage("Emoji cache read");
    if(!st_emoji_known(key->id))return st_fail(err,"Unsupported emoji");
    char identity[160];st_emoji_identity(key->id,identity,sizeof(identity));StDiskEntry entry;
    int cached=st_disk_read(ST_DISK_DIR,identity,&entry);
    if(cached){
        int ok=st_image_decode(entry.data,entry.length,18,18,out,err);st_disk_free(&entry);
        if(ok){key->fresh_for=UINT32_MAX;return 1;}st_disk_remove(ST_DISK_DIR,identity);
    }
    unsigned char *data=NULL;size_t n=0;
    int ok=st_emoji_read(c->config.emoji_file[0]?c->config.emoji_file:"emoji.pack",key->id,&data,&n,err);
    if(ok&&st_cancelled())ok=st_fail(err,"Cancelled");
    if(ok)ok=st_image_decode(data,n,18,18,out,err);
    if(ok){StHeaders headers={.max_age=300};StError disk={0};st_disk_write(ST_DISK_DIR,identity,data,n,&headers,&disk);key->fresh_for=UINT32_MAX;}
    free(data);return ok;
}
/* Static bytes remain on disk until an explicit clear. */
int st_media_fetch_cached(const StClient *c,StMediaRequest *key,StImage *out,StError *err) {
    memset(out,0,sizeof(*out));memset(err,0,sizeof(*err));
    if(st_cancelled())return st_fail(err,"Cancelled");
    if(!strcmp(key->tag,"emoji"))return fetch_emoji(c,key,out,err);
    char url[1200],identity[1750];
    if(!st_media_url(!strcmp(key->tag,"default")?c->config.api_url:c->assets_url,key,url,sizeof(url)))return st_fail(err,"No secure media endpoint");
    snprintf(identity,sizeof(identity),"%s\n%s",c->config.api_url,url);
    st_stage("Disk cache read");StDiskEntry entry;int cached=st_disk_read(ST_DISK_DIR,identity,&entry);
    if(cached) {
        st_stage("Cached image decode");int ok=decode(key,entry.data,entry.length,out,err);
        if(ok) {
            key->fresh_for=UINT32_MAX;st_disk_free(&entry);return 1;
        }
        st_disk_remove(ST_DISK_DIR,identity);st_disk_free(&entry);cached=0;
    }
    if(key->offline){st_disk_free(&entry);return st_fail(err,"Wi-Fi disconnected; image not cached");}
    StHeaders headers;unsigned char *data=NULL;size_t n=0;
    int ok=st_download_conditional(&c->config,url,NULL,&headers,&data,&n,ST_STATIC_LIMIT,5000,err);
    if(ok&&st_cancelled())ok=st_fail(err,"Cancelled");
    if(ok){st_stage("Image decode");ok=decode(key,data,n,out,err);}
    if(!ok&&!st_cancelled()&&key->animated&&animation_tag(key->tag)&&(!err->status||err->status==200)) {
        /* Use Autumn's bounded still preview if the original exceeds limits. */
        StMediaRequest preview=*key;preview.animated=0;preview.filename[0]=0;
        free(data);data=NULL;n=0;st_image_free(out);
        if(st_media_url(c->assets_url,&preview,url,sizeof(url)))ok=st_download_conditional(&c->config,url,NULL,&headers,&data,&n,ST_STATIC_LIMIT,5000,err)&&decode(&preview,data,n,out,err);
    }
    if(ok) {
        StError disk_error={0};st_disk_write(ST_DISK_DIR,identity,data,n,&headers,&disk_error);
        key->fresh_for=UINT32_MAX;
    } else if(!st_cancelled() && (err->status==401||err->status==403||err->status==404||err->status==410))st_disk_remove(ST_DISK_DIR,identity);
    free(data);st_disk_free(&entry);return ok;
}
static int same(const StMediaRequest *a,const StMediaRequest *b){return !strcmp(a->id,b->id)&&!strcmp(a->tag,b->tag)&&a->animated==b->animated&&!strcmp(a->filename,b->filename);}
static void drop(StMediaCache *cache,StMediaSlot *slot) {
    cache->bytes-=st_image_bytes(&slot->image);st_image_free(&slot->image);memset(slot,0,sizeof(*slot));
}
static StMediaSlot *oldest(StMediaCache *cache) {
    StMediaSlot *found=NULL;
    for(int i=0;i<ST_MEDIA_SLOTS;i++) {
        StMediaSlot *s=cache->slots+i;if(s->state==2||s->wanted)continue;
        if(!s->state)return s;
        if(!found||s->used<found->used)found=s;
    }
    return found;
}
void st_media_clear(StMediaCache *cache){for(int i=0;i<ST_MEDIA_SLOTS;i++)st_image_free(&cache->slots[i].image);memset(cache,0,sizeof(*cache));}
void st_media_frame(StMediaCache *cache){cache->tick++;for(int i=0;i<ST_MEDIA_SLOTS;i++)cache->slots[i].wanted=0;}
const StImage *st_media_get(StMediaCache *cache,const char *tag,const char *id) {
    StMediaRequest key={0};st_copy(key.tag,sizeof(key.tag),tag);st_copy(key.id,sizeof(key.id),id);
    return st_media_get_ref(cache,&key);
}
const StImage *st_media_get_ref(StMediaCache *cache,const StMediaRequest *request) {
    StMediaRequest key=*request;
    if(!strcmp(key.tag,"emoji")?!st_emoji_known(key.id):(!valid_tag(key.tag)||!valid_id(key.id)))return NULL;
    if(!strcmp(key.tag,"emojis"))key.animated=0;
    if(key.animated&&(!animation_tag(key.tag)||!valid_filename(key.filename)))return NULL;
    StMediaSlot *slot=NULL;
    for(int i=0;i<ST_MEDIA_SLOTS;i++)if(cache->slots[i].state&&same(&cache->slots[i].key,&key)){slot=cache->slots+i;break;}
    if(!slot){slot=oldest(cache);if(!slot)return NULL;drop(cache,slot);slot->key=key;slot->state=1;}
    slot->wanted=1;slot->used=cache->tick;
    if(slot->state==4 && st_millis()>=slot->retry){slot->state=1;slot->key.revalidate=slot->image.pixels!=NULL;}
    return slot->image.pixels?&slot->image:NULL;
}
int st_media_next(StMediaCache *cache,StMediaRequest *out) {
    for(int i=0;i<ST_MEDIA_SLOTS;i++)if(cache->slots[i].wanted&&cache->slots[i].state==1){cache->slots[i].state=2;*out=cache->slots[i].key;return 1;}
    return 0;
}
void st_media_commit(StMediaCache *cache,const StMediaRequest *key,StImage *image,int success) {
    StMediaSlot *slot=NULL;
    for(int i=0;i<ST_MEDIA_SLOTS;i++)if(cache->slots[i].state&&same(&cache->slots[i].key,key)){slot=cache->slots+i;break;}
    if(!slot){st_image_free(image);return;}
    size_t bytes=st_image_bytes(image);
    size_t prior=st_image_bytes(&slot->image);
    if(success<0){cache->bytes-=prior;st_image_free(&slot->image);prior=0;success=0;}
    while(success&&cache->bytes-prior+bytes>ST_MEDIA_BUDGET) {
        StMediaSlot *victim=NULL;
        for(int i=0;i<ST_MEDIA_SLOTS;i++){StMediaSlot *s=cache->slots+i;if(s!=slot&&s->image.pixels&&s->state!=2&&!s->wanted&&(!victim||s->used<victim->used))victim=s;}
        if(!victim){success=0;break;}drop(cache,victim);
    }
    if(success){st_image_free(&slot->image);slot->image=*image;slot->image.started=st_millis();cache->bytes=cache->bytes-prior+bytes;memset(image,0,sizeof(*image));slot->state=3;
        slot->retry=0;slot->key.revalidate=0;}
    else {st_image_free(image);slot->state=4;slot->retry=st_millis()+30000;}
}
