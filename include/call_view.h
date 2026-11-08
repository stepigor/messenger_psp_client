#ifndef ST_CALL_VIEW_H
#define ST_CALL_VIEW_H
#include "rtc.h"
#include "ui_draw.h"
typedef struct {const StRtcStats *stats;const StClient *client;const char *channel,*status;int cursor,controls,button,member_settings,loopback,reconnect;unsigned mic_peak;} StCallView;
void st_call_view_draw(const StCallView *view,const char *confirm,const char *cancel);
void ui_startup(const StImage *icon,unsigned progress,const char *status);
#endif
