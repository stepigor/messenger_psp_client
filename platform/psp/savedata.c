#include "storage.h"
#include "call_view.h"
#include "ui.h"
#include <psputility.h>
#include <psputility_savedata.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern volatile int app_running;
static unsigned read32(const unsigned char *p){return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);}
static int artwork(FILE *file,const unsigned offsets[8],int index,PspUtilitySavedataFileData *out) {
    unsigned size=offsets[index+1]-offsets[index];
    if(!size||size>128*1024||fseek(file,(long)offsets[index],SEEK_SET))return 0;
    out->buf=malloc(size);if(!out->buf)return 0;out->bufSize=out->size=size;
    return fread(out->buf,1,size,file)==size&&!ferror(file);
}
int st_savedata_prepare(const StStorage *p,unsigned char nonce[16],StError *err) {
    /* An encrypted formal marker gets a fresh firmware-generated IV each
     * launch. Personalize the SDK-seeded DRBG with it, including after XMB deletion. */
    SceUtilitySavedataParam param;memset(&param,0,sizeof(param));param.base.size=sizeof(param);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,&param.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP,&param.base.buttonSwap);
    param.base.graphicsThread=17;param.base.accessThread=19;param.base.fontThread=18;param.base.soundThread=16;
    param.mode=PSP_UTILITY_SAVEDATA_AUTOSAVE;param.overwrite=1;
    st_copy(param.gameName,sizeof(param.gameName),ST_DISC_ID);st_copy(param.saveName,sizeof(param.saveName),ST_SAVE_NAME);
    st_copy(param.fileName,sizeof(param.fileName),"DATA.BIN");
    unsigned char marker[32] __attribute__((aligned(16)))=ST_APP_NAME " data format 1";
    param.dataBuf=marker;param.dataBufSize=param.dataSize=sizeof(marker);
    param.secureVersion=1;memcpy(param.key,"MessengerDataKey",16);
    st_copy(param.sfoParam.title,sizeof(param.sfoParam.title),ST_APP_NAME);
    st_copy(param.sfoParam.savedataTitle,sizeof(param.sfoParam.savedataTitle),ST_SAVE_TITLE);
    st_copy(param.sfoParam.detail,sizeof(param.sfoParam.detail),ST_SAVE_DETAIL);param.sfoParam.parentalLevel=ST_PARENTAL_LEVEL;
    FILE *file=fopen(p->eboot,"rb");unsigned char header[40];unsigned offsets[8];int ok=file!=NULL;
    if(ok)ok=fread(header,1,sizeof(header),file)==sizeof(header)&&!memcmp(header,"\0PBP",4);
    if(ok){for(int i=0;i<8;i++)offsets[i]=read32(header+8+i*4);for(int i=1;i<8;i++)if(offsets[i]<offsets[i-1])ok=0;}
    if(ok)ok=artwork(file,offsets,1,&param.icon0FileData)&&artwork(file,offsets,4,&param.pic1FileData);
    if(file)fclose(file);
    if(!ok){free(param.icon0FileData.buf);free(param.pic1FileData.buf);return st_fail(err,"Cannot read SAVEDATA artwork from EBOOT");}
    StImage icon={0};StError ignored={0};st_profile_image_decode(param.icon0FileData.buf,param.icon0FileData.size,144,80,&icon,&ignored);
    int result=sceUtilitySavedataInitStart(&param);
    if(result>=0) {
        int started=0,shutdown=0;uint64_t begin=st_millis();
        for(;;) {
            ui_begin();ui_startup(&icon,(unsigned)((st_millis()-begin)*1000/700),"Loading saved data...");
            int status=sceUtilitySavedataGetStatus();if(status!=PSP_UTILITY_DIALOG_NONE)started=1;
            if(status==PSP_UTILITY_DIALOG_VISIBLE)sceUtilitySavedataUpdate(1);
            if(status==PSP_UTILITY_DIALOG_QUIT&&!shutdown){sceUtilitySavedataShutdownStart();shutdown=1;}
            ui_end();if(started&&status==PSP_UTILITY_DIALOG_NONE&&st_millis()-begin>=700)break;
        }
        result=param.base.result;
    }
    st_image_free(&icon);free(param.icon0FileData.buf);free(param.pic1FileData.buf);st_wipe(marker,sizeof(marker));
    if(result<0||!app_running){char text[128];snprintf(text,sizeof(text),"Cannot prepare SAVEDATA (0x%08X)",(unsigned)result);return st_fail(err,text);}
    char path[550];snprintf(path,sizeof(path),"%s/DATA.BIN",p->savedata);
    file=fopen(path,"rb");if(!file)return st_fail(err,"Missing SAVEDATA marker");
    ok=fseek(file,0,SEEK_END)==0&&ftell(file)==48&&fseek(file,0,SEEK_SET)==0&&fread(nonce,1,16,file)==16;
    fclose(file);if(!ok)return st_fail(err,"SAVEDATA marker must be encrypted; TLS disabled");
    return 1;
}
