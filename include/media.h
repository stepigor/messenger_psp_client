#ifndef ST_MEDIA_H
#define ST_MEDIA_H
#include "stoat.h"
/* Four rows of 13 emoji, seven rail icons and an in-flight request. */
#define ST_MEDIA_SLOTS 64
#define ST_MEDIA_BUDGET (1024u*1024u)
#define ST_IMAGE_FRAMES 24
typedef struct { unsigned *pixels;int width,height,frames;unsigned delay[ST_IMAGE_FRAMES],loops;uint64_t started; } StImage;
typedef struct { char tag[16],id[64],filename[129];int animated,revalidate,offline;unsigned fresh_for; } StMediaRequest;
typedef struct {
    StMediaRequest key;StImage image;
    int state,wanted;uint64_t used,retry;
} StMediaSlot;
typedef struct { StMediaSlot slots[ST_MEDIA_SLOTS];size_t bytes;uint64_t tick; } StMediaCache;
void st_image_ref(const cJSON *file,const char *tag,char out[64]);
void st_profile_image_ref(const cJSON *file,const char *tag,StMediaRequest *out);
int st_media_url(const char *base,const StMediaRequest *key,char *out,size_t cap);
int st_image_decode(const unsigned char *data,size_t n,int max_width,int max_height,StImage *out,StError *err);
void st_image_free(StImage *image);
size_t st_image_bytes(const StImage *image);
const unsigned *st_image_pixels(const StImage *image,uint64_t now);
int st_profile_image_decode(const unsigned char *data,size_t n,int width,int height,StImage *out,StError *err);
int st_media_fetch(const StClient *client,const StMediaRequest *key,StImage *out,StError *err);
int st_media_fetch_cached(const StClient *client,StMediaRequest *key,StImage *out,StError *err);
void st_media_clear(StMediaCache *cache);
void st_media_frame(StMediaCache *cache);
const StImage *st_media_get(StMediaCache *cache,const char *tag,const char *id);
const StImage *st_media_get_ref(StMediaCache *cache,const StMediaRequest *key);
int st_media_next(StMediaCache *cache,StMediaRequest *out);
void st_media_commit(StMediaCache *cache,const StMediaRequest *key,StImage *image,int success);
#endif
