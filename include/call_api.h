#ifndef ST_CALL_API_H
#define ST_CALL_API_H
#include "stoat.h"
typedef struct {char url[512],node[128],address[16],token[8193];} StCallTicket;
/* These run only on the existing serial HTTP/DNS worker. Tokens stay in RAM. */
int st_call_prepare(StClient *c,StCallTicket *ticket,StError *error);
int st_call_authorize(StClient *c,const char *channel,int force,StCallTicket *ticket,StError *error);
int st_call_catchup(StClient *client,const cJSON *baseline,cJSON **out,int *notify,StError *error);
#endif
