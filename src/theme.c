#include "theme.h"
#include "theme_data.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/* Dark / Blue shares the XMB/SAVEDATA brand background. */
StThemeColors st_theme=ST_THEME_DEFAULT;
int st_theme_dark=1,st_theme_accent=4;
const char *const st_theme_names[ST_THEME_COLORS]={"Orange","Yellow","Green","Mint","Blue","Indigo","Purple"};
const unsigned st_theme_seeds[ST_THEME_COLORS]={0xff3357ffu,0xff2fdcffu,0xff88f09bu,0xffc1ec54u,0xffec9b54u,0xffec7054u,0xffd35f8cu};
void st_theme_apply(int dark,int accent) {
    st_theme_dark=dark?1:0;st_theme_accent=accent>=0&&accent<ST_THEME_COLORS?accent:4;
    st_theme=palettes[st_theme_dark][st_theme_accent];
}
int st_theme_load(const char *path,StError *err) {
    char tmp[600],data[257];if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp)){st_fail(err,"Theme path too long");return -1;}
    FILE *f=fopen(tmp,"rb");if(!f){if(errno!=ENOENT){st_fail(err,"Cannot read pending theme");return -1;}f=fopen(path,"rb");}
    if(!f){if(errno==ENOENT)return 0;st_fail(err,"Cannot read theme settings");return -1;}
    size_t n=fread(data,1,sizeof(data)-1,f);int ok=!ferror(f)&&n<sizeof(data)-1&&fgetc(f)==EOF;fclose(f);data[n]=0;
    cJSON *r=ok?cJSON_ParseWithLengthOpts(data,n+1,NULL,1):NULL;
    const cJSON *format=cJSON_GetObjectItemCaseSensitive(r,"format"),*dark=cJSON_GetObjectItemCaseSensitive(r,"dark"),*accent=cJSON_GetObjectItemCaseSensitive(r,"accent");
    ok=cJSON_IsObject(r)&&cJSON_IsNumber(format)&&format->valuedouble==1&&cJSON_IsBool(dark)&&cJSON_IsNumber(accent)&&
        accent->valuedouble==accent->valueint&&accent->valueint>=0&&accent->valueint<ST_THEME_COLORS;
    if(ok)st_theme_apply(cJSON_IsTrue(dark),accent->valueint);
    cJSON_Delete(r);if(!ok){st_fail(err,"Invalid theme settings");return -1;}return 1;
}
int st_theme_save(const char *path,StError *err) {
    char tmp[600],data[80];if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return st_fail(err,"Theme path too long");
    int n=snprintf(data,sizeof(data),"{\"format\":1,\"dark\":%s,\"accent\":%d}\n",st_theme_dark?"true":"false",st_theme_accent);
    FILE *f=fopen(tmp,"wb");int ok=f&&fwrite(data,1,(size_t)n,f)==(size_t)n;if(f&&fclose(f))ok=0;
    if(!ok){if(f)remove(tmp);return st_fail(err,"Cannot save theme; choice remains in memory");}
    if(remove(path)&&errno!=ENOENT)return st_fail(err,"Cannot replace theme; pending choice retained");
    if(rename(tmp,path))return st_fail(err,"Cannot rename theme; pending choice retained");
    return 1;
}
