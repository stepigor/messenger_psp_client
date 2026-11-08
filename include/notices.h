#ifndef ST_NOTICES_H
#define ST_NOTICES_H
#include "stoat.h"
#define ST_NOTICE_MAX 4096
typedef struct {char channel[64],last[64],read[64];unsigned count;int unknown,ack,restored;} StNotice;
typedef struct {StNotice *items;size_t count;unsigned revision,saved,sounds;int loaded;char visible[64];} StNotices;
void st_notices_clear(StNotices *n);
void st_notices_sync(StNotices *n,const StClient *c);
void st_notices_event(StNotices *n,const StClient *c,const cJSON *event);
void st_notices_read(StNotices *n,const char *channel,const char *last);
unsigned st_notices_channel(const StNotices *n,const char *channel,int *unknown);
unsigned st_notices_total(const StNotices *n,const StClient *c,const char *server,int *unknown);
cJSON *st_notices_export(const StNotices *n);
void st_notices_import(StNotices *n,const StClient *c,const cJSON *snapshot);
int st_notices_load(StClient *c,cJSON **out,StError *err);
int st_notices_save(const StClient *c,const cJSON *snapshot,StError *err);
int st_notices_ack(StClient *c,const char *channel,const char *id,StError *err);
#endif
