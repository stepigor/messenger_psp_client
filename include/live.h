#ifndef ST_LIVE_H
#define ST_LIVE_H
#include "stoat.h"
#include "notices.h"
/* Ordinary transport calls use the serial worker, never the UI thread. */
int st_ws_live(StClient *c,const char *typing,cJSON **events,int *connected,StError *err);
void st_ws_live_close(void);
/* Serial worker prepares; voice worker exclusively pumps the existing socket.
 * No reconnect, DNS, TLS handshake, API request or waiting in the voice pump. */
int st_ws_presence_begin(StClient *client,StError *err);
int st_ws_presence_step(StError *err);
#define ST_TYPING_MAX 16
typedef struct {char user[64];uint64_t until;} StTyping;
typedef struct {
    StNotices notices;
    char channel[64];StTyping typing[ST_TYPING_MAX];
    unsigned revision,applied;int lists_dirty,members_dirty,connected,logged_out;
} StLive;
void st_live_channel(StLive *live,const char *channel);
void st_live_events(StLive *live,StClient *client,const cJSON *events,uint64_t now);
void st_live_typing_text(const StLive *live,const StClient *client,uint64_t now,char *out,size_t cap);
#endif
