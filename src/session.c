#define _POSIX_C_SOURCE 200809L
#include "stoat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int identity(const char *s) {
    size_t n=strlen(s);if(!n||n>=64)return 0;
    for(;*s;s++)if(!((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return 0;
    return 1;
}
static int token_valid(const char *s) {
    size_t n=strlen(s);if(!n||n>=256)return 0;
    for(;*s;s++)if((unsigned char)*s<33||(unsigned char)*s>126)return 0;
    return 1;
}
static int temporary(const char *path,char out[600],StError *err) {
    return snprintf(out,600,"%s.tmp",path)<600?1:st_fail(err,"Session path too long");
}
int st_session_load(StClient *c,const char *path,StError *err) {
    char tmp[600];if(!temporary(path,tmp,err))return -1;
    /* PSP rename cannot replace a file. A complete pending replacement wins. */
    FILE *f=fopen(tmp,"rb");
    if(!f){if(errno!=ENOENT){st_fail(err,"Cannot read saved session");return -1;}f=fopen(path,"rb");}
    if(!f){if(errno==ENOENT)return 0;st_fail(err,"Cannot read saved session");return -1;}
    char data[2049]={0};size_t n=fread(data,1,sizeof(data)-1,f);
    int valid=!ferror(f)&&n<sizeof(data)-1&&fgetc(f)==EOF;fclose(f);
    cJSON *r=valid?cJSON_ParseWithLengthOpts(data,n+1,NULL,1):NULL;
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(r,"format");
    const char *url=st_str(r,"api_url"),*token=st_str(r,"token"),*uid=st_str(r,"user_id");
    int result=-1;
    if(!cJSON_IsObject(r)||!cJSON_IsNumber(v)||v->valuedouble!=1||
       !*url||!token_valid(token)||!identity(uid))st_fail(err,"Invalid saved session; log out to remove it");
    else if(strcmp(url,c->config.api_url))result=0; /* Never send it to a different endpoint. */
    else {const char *sid=st_str(r,"session_id");if(*sid&&!identity(sid))st_fail(err,"Invalid saved session ID");else {st_copy(c->token,sizeof(c->token),token);st_copy(c->self_id,sizeof(c->self_id),uid);st_copy(c->session_id,sizeof(c->session_id),sid);result=1;}}
    if(r)st_wipe((void *)token,strlen(token));
    cJSON_Delete(r);st_wipe(data,sizeof(data));return result;
}
int st_session_save(const StClient *c,const char *path,StError *err) {
    if(!token_valid(c->token)||!identity(c->self_id))return st_fail(err,"No valid session to save");
    char tmp[600];if(!temporary(path,tmp,err))return 0;
    cJSON *r=cJSON_CreateObject();if(!r)return st_fail(err,"Out of memory");
    cJSON_AddNumberToObject(r,"format",1);cJSON_AddStringToObject(r,"api_url",c->config.api_url);
    cJSON_AddStringToObject(r,"token",c->token);cJSON_AddStringToObject(r,"user_id",c->self_id);
    if(c->session_id[0])cJSON_AddStringToObject(r,"session_id",c->session_id);
    char *data=cJSON_PrintUnformatted(r);st_wipe((void *)st_str(r,"token"),strlen(st_str(r,"token")));cJSON_Delete(r);
    if(!data)return st_fail(err,"Out of memory");
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600);FILE *f=fd<0?NULL:fdopen(fd,"wb");
    if(!f&&fd>=0)close(fd);
    size_t n=strlen(data);int ok=f&&fwrite(data,1,n,f)==n;
    if(f&&fclose(f))ok=0;
    st_wipe(data,n);cJSON_free(data);
    if(!ok){remove(tmp);return st_fail(err,"Cannot write session.json; session remains in RAM");}
    if(remove(path)&&errno!=ENOENT)return st_fail(err,"Cannot replace session.json; pending session retained");
    if(rename(tmp,path))return st_fail(err,"Cannot rename session.json.tmp; pending session retained");
    return 1;
}
int st_session_forget(const char *path,StError *err) {
    char tmp[600];if(!temporary(path,tmp,err))return 0;
    int ok=1;if(remove(tmp)&&errno!=ENOENT)ok=0;if(remove(path)&&errno!=ENOENT)ok=0;
    return ok?1:st_fail(err,"Cannot remove saved session from card");
}
