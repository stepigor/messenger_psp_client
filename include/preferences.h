#ifndef ST_PREFERENCES_H
#define ST_PREFERENCES_H
#include "stoat.h"
/* Values use the desktop client's sync/notifications store. */
enum StNotify { ST_NOTIFY_ALL,ST_NOTIFY_MENTION,ST_NOTIFY_NONE };
int st_preferences_fetch(StClient *c,StError *err);
int st_preferences_set(StClient *c,const char *id,int server,enum StNotify mode,StError *err);
enum StNotify st_preferences_mode(const StClient *c,const char *id,int server);
int st_preferences_notify(const StClient *c,const cJSON *message);
int st_preferences_event(StClient *c,const cJSON *event);
/* Ready already supplies the account's memberships across servers. */
void st_preferences_memberships(StClient *c,const cJSON *members);
#endif
