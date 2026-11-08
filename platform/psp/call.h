#ifndef ST_PSP_CALL_H
#define ST_PSP_CALL_H
#include "rtc.h"
#include "call_api.h"
int st_call_start(const StConfig *config,const StCallTicket *endpoint,const char *channel,const char *user);
void st_call_poll(void);
int st_call_loopback(void);
void st_call_microphone(int muted);
void st_call_deafen(int deafened);
void st_call_volume(const char *user,int volume);
int st_call_restore_volume(const char *user);
unsigned st_call_mic_peak(void);
int st_call_busy(void);
int st_call_needs_token(void);
int st_call_token(const char *token);
void st_call_error(const char *message);
void st_call_stop(void);
StRtcStats st_call_snapshot(void);
const char *st_call_channel(void);
const char *st_call_user(void);
#endif
