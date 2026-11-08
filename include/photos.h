#ifndef ST_PHOTOS_H
#define ST_PHOTOS_H
#include "stoat.h"
#define ST_PHOTO_ENTRIES 256
#define ST_PHOTO_LIMIT (10u*1024u*1024u)
#define ST_PHOTO_COLUMNS 3
#define ST_PHOTO_PAGE 6
#define ST_PHOTO_WIDTH 140
#define ST_PHOTO_HEIGHT 68
typedef struct {char name[256],key[33];int directory;uint64_t bytes;} StPhotoEntry;
typedef struct {char root[512],path[512],device[16];StPhotoEntry *items;size_t count;int truncated,sources;} StPhotos;
int st_photos_sources(StPhotos *p,const char *device,StError *err);
int st_photos_source(StPhotos *p,int row,StError *err);
int st_photos_scan(StPhotos *p,StError *err);
int st_photos_enter(StPhotos *p,int row,StError *err);
int st_photos_parent(StPhotos *p,StError *err);
void st_photos_clear(StPhotos *p);
int st_photo_info(const char *path,uint64_t *size,const char **mime,StError *err);
int st_photo_key(const char *path,char key[33],StError *err);
int st_photos_file(const StPhotos *p,int row,char *path,size_t capacity,StError *err);
int st_upload_photo(StClient *c,const char *path,char id[64],StError *err);
int st_send_photo(StClient *c,const char *channel,const char *path,StError *err);
#endif
