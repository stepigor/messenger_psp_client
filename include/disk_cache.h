#ifndef ST_DISK_CACHE_H
#define ST_DISK_CACHE_H
#include "transport.h"
#ifdef __PSP__
/* Flat SAVEDATA: XMB can delete every cache file with the single entry. */
#define ST_DISK_DIR "."
#else
#define ST_DISK_DIR "cache"
#endif
#define ST_STATIC_LIMIT (512u*1024u)
typedef struct { uint64_t bytes;size_t files; } StCacheStats;
typedef struct { unsigned char *data;size_t length;StHeaders headers;int64_t checked; } StDiskEntry;
int st_disk_read(const char *dir,const char *key,StDiskEntry *entry);
int st_disk_write(const char *dir,const char *key,const unsigned char *data,size_t n,const StHeaders *headers,StError *err);
void st_disk_free(StDiskEntry *entry);
void st_disk_remove(const char *dir,const char *key);
int st_disk_stats(const char *dir,StCacheStats *stats,StError *err);
int st_disk_clear(const char *dir,StError *err);
#endif
