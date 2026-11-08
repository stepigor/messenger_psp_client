#define _DEFAULT_SOURCE
#include "photos.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#define mbedtls_sha256_ret mbedtls_sha256
#endif
static int image_name(const char *s){const char *p=strrchr(s,'.');return p&&(!strcasecmp(p,".png")||!strcasecmp(p,".jpg")||!strcasecmp(p,".jpeg")||!strcasecmp(p,".gif")||!strcasecmp(p,".webp")||!strcasecmp(p,".bmp"));}
static int order(const void *a,const void *b){const StPhotoEntry *x=a,*y=b;return x->directory!=y->directory?y->directory-x->directory:strcasecmp(x->name,y->name);}
void st_photos_clear(StPhotos *p){free(p->items);memset(p,0,sizeof(*p));}
static int safe_path(const char *path){
    if(!*path)return 0;
    for(const char *p=path;*p;){while(*p=='/')p++;const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);if((n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')||strchr(p,'\\'))return 0;if(!end)break;p=end+1;}return 1;
}
static int within(const StPhotos *p){size_t n=strlen(p->root);return n&&safe_path(p->root)&&safe_path(p->path)&&!strncmp(p->path,p->root,n)&&(!p->path[n]||p->path[n]=='/');}
int st_photo_key(const char *path,char key[33],StError *err){
    struct stat info;unsigned char hash[32];char identity[1200];
    if(stat(path,&info)||!S_ISREG(info.st_mode))return st_fail(err,"Photo unavailable");
    int n=snprintf(identity,sizeof(identity),"photo-thumb-v1:%s:%llu:%lld",path,(unsigned long long)info.st_size,(long long)info.st_mtime);
    if(n<0||n>=(int)sizeof(identity)||mbedtls_sha256_ret((const unsigned char *)identity,(size_t)n,hash,0))return st_fail(err,"Cannot identify photo");
    for(int i=0;i<16;i++)snprintf(key+i*2,3,"%02x",hash[i]);
    return 1;
}
int st_photos_sources(StPhotos *p,const char *device,StError *err){
    if(strcmp(device,"ms0:")&&strcmp(device,"ef0:"))return st_fail(err,"Unsupported photo device");
    StPhotoEntry *items=calloc(3,sizeof(*items));if(!items)return st_fail(err,"Out of memory");
    const char *names[]={"PICTURE","PSP/PHOTO","PSP/SCREENSHOT"};for(int i=0;i<3;i++){st_copy(items[i].name,sizeof(items[i].name),names[i]);items[i].directory=1;}
    char copy[16];st_copy(copy,sizeof(copy),device);st_photos_clear(p);p->items=items;p->count=3;p->sources=1;st_copy(p->device,sizeof(p->device),copy);return 1;
}
int st_photos_source(StPhotos *p,int row,StError *err){
    if(!p->sources||row<0||row>=3)return st_fail(err,"Select a photo folder");
    StPhotos next={0};st_copy(next.device,sizeof(next.device),p->device);snprintf(next.root,sizeof(next.root),"%s/%s",p->device,p->items[row].name);st_copy(next.path,sizeof(next.path),next.root);
    if(!st_photos_scan(&next,err)){st_photos_clear(&next);return 0;}
    st_photos_clear(p);*p=next;return 1;
}
int st_photos_file(const StPhotos *p,int row,char *path,size_t capacity,StError *err){
    if(p->sources||!within(p)||row<0||(size_t)row>=p->count||p->items[row].directory||strchr(p->items[row].name,'/')||strchr(p->items[row].name,'\\')||!strcmp(p->items[row].name,".."))return st_fail(err,"Select a photo");
    if(snprintf(path,capacity,"%s/%s",p->path,p->items[row].name)>=(int)capacity)return st_fail(err,"Photo path too long");
    return 1;
}
int st_photos_scan(StPhotos *p,StError *err){
    if(p->sources||!within(p))return st_fail(err,"Outside photo folders");
    DIR *d=opendir(p->path);if(!d)return st_fail(err,"Cannot open photo folder");
    StPhotoEntry *items=calloc(ST_PHOTO_ENTRIES,sizeof(*items));if(!items){closedir(d);return st_fail(err,"Out of memory");}
    size_t n=0;int truncated=0;struct dirent *e;
    while((e=readdir(d))){
        if(e->d_name[0]=='.'||strchr(e->d_name,'/')||strlen(e->d_name)>=256)continue;
        char path[1024];struct stat s;if(snprintf(path,sizeof(path),"%s/%s",p->path,e->d_name)>=(int)sizeof(path)||lstat(path,&s))continue;
        int directory=S_ISDIR(s.st_mode);if(!directory&&(!S_ISREG(s.st_mode)||!image_name(e->d_name)))continue;
        if(n==ST_PHOTO_ENTRIES){truncated=1;continue;}st_copy(items[n].name,sizeof(items[n].name),e->d_name);items[n].directory=directory;items[n].bytes=(uint64_t)s.st_size;
        if(!directory){StError ignored={0};st_photo_key(path,items[n].key,&ignored);}n++;
    }
    closedir(d);qsort(items,n,sizeof(*items),order);free(p->items);p->items=items;p->count=n;p->truncated=truncated;return 1;
}
int st_photos_enter(StPhotos *p,int row,StError *err){
    if(p->sources||!within(p)||row<0||(size_t)row>=p->count||!p->items[row].directory||strchr(p->items[row].name,'/')||strchr(p->items[row].name,'\\')||!strcmp(p->items[row].name,".."))return st_fail(err,"Select a folder");
    char old[512],next[512];st_copy(old,sizeof(old),p->path);
    if(snprintf(next,sizeof(next),"%s/%s",old,p->items[row].name)>=(int)sizeof(next))return st_fail(err,"Photo path too long");
    st_copy(p->path,sizeof(p->path),next);if(st_photos_scan(p,err))return 1;
    st_copy(p->path,sizeof(p->path),old);return 0;
}
int st_photos_parent(StPhotos *p,StError *err){
    if(p->sources||!within(p)||!strcmp(p->root,p->path))return 0;
    char old[512];st_copy(old,sizeof(old),p->path);char *s=strrchr(p->path,'/');if(!s)return 0;*s=0;
    if(st_photos_scan(p,err))return 1;
    st_copy(p->path,sizeof(p->path),old);return 0;
}
int st_photo_info(const char *path,uint64_t *size,const char **mime,StError *err){
    struct stat s;if(stat(path,&s)||!S_ISREG(s.st_mode)||s.st_size<1||(uint64_t)s.st_size>ST_PHOTO_LIMIT)return st_fail(err,"Photo unavailable or larger than 10 MB");
    FILE *f=fopen(path,"rb");if(!f)return st_fail(err,"Cannot read photo");unsigned char h[12]={0};size_t n=fread(h,1,sizeof(h),f);fclose(f);
    *mime=n>=8&&!memcmp(h,"\x89PNG\r\n\x1a\n",8)?"image/png":n>=3&&h[0]==255&&h[1]==216&&h[2]==255?"image/jpeg":n>=6&&(!memcmp(h,"GIF87a",6)||!memcmp(h,"GIF89a",6))?"image/gif":n==12&&!memcmp(h,"RIFF",4)&&!memcmp(h+8,"WEBP",4)?"image/webp":n>=2&&!memcmp(h,"BM",2)?"image/bmp":NULL;
    if(!*mime)return st_fail(err,"Unsupported photo format");
    *size=(uint64_t)s.st_size;return 1;
}
int st_send_photo(StClient *c,const char *channel,const char *path,StError *err){
    if(!*channel||strlen(channel)>=64||strspn(channel,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")!=strlen(channel))return st_fail(err,"Invalid channel");
    uint64_t bytes;const char *mime;if(!st_photo_info(path,&bytes,&mime,err))return 0;(void)mime;
    struct stat s;if(stat(path,&s))return st_fail(err,"Photo disappeared");
    char signature[1024];if(snprintf(signature,sizeof(signature),"%s:%llu:%lld:%s",channel,(unsigned long long)bytes,(long long)s.st_mtime,path)>=(int)sizeof(signature))return st_fail(err,"Photo path too long");
    if(strcmp(signature,c->photo_signature)){
        unsigned char random[16];if(!st_random(random,sizeof(random)))return st_fail(err,"Random source unavailable");
        memset(c->photo_id,0,sizeof(c->photo_id));for(size_t i=0;i<sizeof(random);i++)snprintf(c->photo_key+i*2,3,"%02x",random[i]);st_copy(c->photo_signature,sizeof(c->photo_signature),signature);
    }
    if(!c->photo_id[0]&&!st_upload_photo(c,path,c->photo_id,err))return 0;
    if(st_cancelled())return st_fail(err,"Cancelled");
    cJSON *body=cJSON_CreateObject(),*r=NULL;if(!body)return st_fail(err,"Out of memory");cJSON *a=cJSON_AddArrayToObject(body,"attachments");
    if(!a||!cJSON_AddItemToArray(a,cJSON_CreateString(c->photo_id))){cJSON_Delete(body);return st_fail(err,"Out of memory");}
    char endpoint[160];snprintf(endpoint,sizeof(endpoint),"/channels/%s/messages",channel);
    int ok=st_http(c,"POST",endpoint,body,c->photo_key,&r,err);cJSON_Delete(body);cJSON_Delete(r);
    if(!ok&&err->status==409&&strstr(err->text,"DuplicateNonce")){
        snprintf(endpoint,sizeof(endpoint),"/channels/%s/messages?limit=100&sort=Latest",channel);StError check={0};
        if(st_http(c,"GET",endpoint,NULL,NULL,&r,&check)){
            const cJSON *m,*file,*messages=cJSON_IsArray(r)?r:cJSON_GetObjectItemCaseSensitive(r,"messages");
            cJSON_ArrayForEach(m,messages)if(!strcmp(st_str(m,"nonce"),c->photo_key)&&!strcmp(st_str(m,"author"),c->self_id))cJSON_ArrayForEach(file,cJSON_GetObjectItemCaseSensitive(m,"attachments"))if(!strcmp(st_str(file,"_id"),c->photo_id))ok=1;
        }cJSON_Delete(r);
        if(!ok)st_fail(err,"Send result uncertain. Check this chat before retrying.");
    }
    if(ok){c->photo_signature[0]=c->photo_id[0]=c->photo_key[0]=0;}
    else if(err->status>=400&&err->status<500&&err->status!=409)c->photo_signature[0]=0;
    return ok;
}
