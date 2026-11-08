#ifndef ST_RTC_TLS_H
#define ST_RTC_TLS_H
#include "stoat.h"
typedef struct StRtcTls StRtcTls;
typedef int (*StRtcSend)(void *,const unsigned char *,size_t);
typedef int (*StRtcReceive)(void *,unsigned char *,size_t);
StRtcTls *st_rtc_tls_new(int datagram,const char *ca,const char *hostname,StError *err);
void st_rtc_tls_free(StRtcTls *t);
void st_rtc_tls_io(StRtcTls *t,void *ctx,StRtcSend send,StRtcReceive receive);
int st_rtc_tls_step(StRtcTls *t,StError *err);
int st_rtc_tls_read(StRtcTls *t,unsigned char *out,size_t n);
int st_rtc_tls_write(StRtcTls *t,const unsigned char *data,size_t n);
const char *st_rtc_tls_fingerprint(const StRtcTls *t);
int st_rtc_tls_peer(StRtcTls *t,const char *fingerprint);
int st_rtc_tls_keys(const StRtcTls *t,unsigned char inbound[30],unsigned char outbound[30]);
#endif
