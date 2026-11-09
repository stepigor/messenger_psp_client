#ifndef ST_STORAGE_H
#define ST_STORAGE_H
#include "stoat.h"
#define ST_SAVE_NAME "DATA"
#define ST_SAVE_DIRECTORY ST_DISC_ID ST_SAVE_NAME
#define ST_SAVE_TITLE "Cache and session"
#define ST_SAVE_DETAIL "Cache, settings and your account session. Do not share this saved data: it may grant access to your account. Delete it in XMB to reset the client."
typedef struct { char game[512],savedata[512],config[512],eboot[512]; } StStorage;
int st_storage_locate(StStorage *paths,const char *boot_file,const char *argument,const char *cwd,StError *err);
int st_savedata_prepare(const StStorage *paths,unsigned char nonce[16],StError *err);
#endif
