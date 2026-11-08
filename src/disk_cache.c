#include "disk_cache.h"
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#define mbedtls_sha256_ret mbedtls_sha256
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
static int path_for(const char *dir,const char *key,char *path,size_t cap) {
    unsigned char digest[32];char hex[65];
    if(mbedtls_sha256_ret((const unsigned char *)key,strlen(key),digest,0))return 0;
    for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    return snprintf(path,cap,"%s/%s.stc",dir,hex)<(int)cap;
}
static int owned(const char *name) {
    size_t n=strlen(name);if(n!=68&&n!=72)return 0;
    for(int i=0;i<64;i++)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return 0;
    return !strcmp(name+64,".stc")||!strcmp(name+64,".stc.tmp");
}
void st_disk_free(StDiskEntry *e){free(e->data);memset(e,0,sizeof(*e));}
void st_disk_remove(const char *dir,const char *key){char p[700];if(path_for(dir,key,p,sizeof(p)))remove(p);}
int st_disk_read(const char *dir,const char *key,StDiskEntry *e) {
    memset(e,0,sizeof(*e));char p[700],line[2048];struct stat st;
    if(!path_for(dir,key,p,sizeof(p))||stat(p,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>(off_t)(ST_STATIC_LIMIT+2048))return 0;
    FILE *f=fopen(p,"rb");if(!f)return 0;
    if(!fgets(line,sizeof(line),f)||!strchr(line,'\n')){fclose(f);remove(p);return 0;}
    cJSON *meta=cJSON_Parse(line);
    int good=cJSON_IsObject(meta)&&!strcmp(st_str(meta,"key"),key);
    long offset=ftell(f);size_t n=offset>0?(size_t)st.st_size-(size_t)offset:0;
    if(!good||!n||n>ST_STATIC_LIMIT){cJSON_Delete(meta);fclose(f);remove(p);return 0;}
    e->data=malloc(n);if(!e->data){cJSON_Delete(meta);fclose(f);return 0;}
    e->length=n;
    size_t read=0;
    while(read<n&&!st_cancelled()) {size_t part=n-read;if(part>16384)part=16384;if(fread(e->data+read,1,part,f)!=part)break;read+=part;}
    good=read==n&&!ferror(f)&&!st_cancelled();fclose(f);cJSON_Delete(meta);
    if(!good){st_disk_free(e);if(!st_cancelled())remove(p);return 0;}
    return 1;
}
int st_disk_stats(const char *dir,StCacheStats *stats,StError *err) {
    memset(stats,0,sizeof(*stats));DIR *d=opendir(dir);if(!d)return errno==ENOENT?1:st_fail(err,"Cannot read cache directory");
    struct dirent *entry;int ok=1;
    while((entry=readdir(d))) {
        if(st_cancelled()){closedir(d);return st_fail(err,"Cancelled");}
        if(!owned(entry->d_name))continue;
        char p[700];struct stat st;snprintf(p,sizeof(p),"%s/%s",dir,entry->d_name);
        if(stat(p,&st)||!S_ISREG(st.st_mode)){ok=0;continue;}
        stats->files++;if(st.st_size>0)stats->bytes+=(uint64_t)st.st_size;
    }
    closedir(d);return ok?1:st_fail(err,"Cannot inspect cache file");
}
int st_disk_clear(const char *dir,StError *err) {
    DIR *d=opendir(dir);if(!d)return errno==ENOENT?1:st_fail(err,"Cannot open cache directory");
    struct dirent *entry;int ok=1;
    while((entry=readdir(d))) {
        if(st_cancelled()){closedir(d);return st_fail(err,"Cancelled");}
        if(!owned(entry->d_name))continue;
        char p[700];snprintf(p,sizeof(p),"%s/%s",dir,entry->d_name);if(remove(p)&&errno!=ENOENT)ok=0;
    }
    closedir(d);return ok?1:st_fail(err,"Cannot remove all cache files; check Memory Stick");
}
int st_disk_write(const char *dir,const char *key,const unsigned char *data,size_t n,const StHeaders *h,StError *err) {
    (void)h;
    st_stage("Disk cache write");
    if(st_cancelled())return st_fail(err,"Cancelled");
    if(!n||n>ST_STATIC_LIMIT)return st_fail(err,"Invalid cache size");
    if(mkdir(dir,0700)&&errno!=EEXIST)return st_fail(err,"Cannot create cache directory");
    char p[700],tmp[704];if(!path_for(dir,key,p,sizeof(p)))return st_fail(err,"Invalid cache key");
    snprintf(tmp,sizeof(tmp),"%s.tmp",p);
    cJSON *meta=cJSON_CreateObject();if(!meta)return st_fail(err,"Out of memory");
    cJSON_AddNumberToObject(meta,"policy",3);cJSON_AddStringToObject(meta,"key",key);
    char *line=cJSON_PrintUnformatted(meta);cJSON_Delete(meta);if(!line)return st_fail(err,"Out of memory");
    size_t len=strlen(line);int ok=len+1<2048;
    FILE *f=ok?fopen(tmp,"wb"):NULL;
    if(f) {
        ok=fwrite(line,1,len,f)==len&&fputc('\n',f)!=EOF;
        size_t written=0;
        while(ok&&written<n&&!st_cancelled()) {size_t part=n-written;if(part>16384)part=16384;if(fwrite(data+written,1,part,f)!=part){ok=0;break;}written+=part;}
        if(written!=n||st_cancelled())ok=0;
        if(fclose(f))ok=0;
    } else ok=0;
    cJSON_free(line);
    if(ok){if(remove(p)&&errno!=ENOENT)ok=0;else if(rename(tmp,p))ok=0;}
    if(!ok){remove(tmp);return st_fail(err,"Cannot save static cache; image kept in RAM");}
    return 1;
}
