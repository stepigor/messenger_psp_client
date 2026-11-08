#include "storage.h"
#include <stdio.h>
#include <string.h>
static int join(char *out,size_t cap,const char *base,const char *name,StError *err){return snprintf(out,cap,"%s/%s",base,name)<(int)cap?1:st_fail(err,"Storage path too long");}
int st_storage_paths(StStorage *p,const char *game,StError *err) {
    memset(p,0,sizeof(*p));size_t n=strlen(game);
    if(!n||n>=sizeof(p->game)-32)return st_fail(err,"Application path too long");
    st_copy(p->game,sizeof(p->game),game);while(n&&p->game[n-1]=='/')p->game[--n]=0;
    const char *psp=strstr(p->game,"/PSP/GAME/");
    if(!psp)return st_fail(err,"Install in PSP/GAME/" ST_APP_SLUG);
    size_t root=(size_t)(psp-p->game);
    if(snprintf(p->savedata,sizeof(p->savedata),"%.*s/PSP/SAVEDATA/%s",(int)root,p->game,ST_SAVE_DIRECTORY)>=(int)sizeof(p->savedata))return st_fail(err,"SAVEDATA path too long");
    return join(p->config,sizeof(p->config),p->game,"config.ini",err)&&
        join(p->eboot,sizeof(p->eboot),p->game,"EBOOT.PBP",err);
}
