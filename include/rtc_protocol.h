#ifndef ST_RTC_PROTOCOL_H
#define ST_RTC_PROTOCOL_H
#include "stoat.h"
#define ST_RTC_SDP 32768
#define ST_RTC_CANDIDATES 16
#define ST_RTC_STREAMS 4
typedef struct {char mid[32],track[64];int opus,red;uint32_t ssrc;} StRtcMedia;
typedef struct { char ip[16];unsigned port,priority; } StRtcCandidate;
typedef struct {
    char mid[32],ufrag[128],password[256],fingerprint[96];
    int opus,red,lite,audio;char data_mid[32];int data;
    StRtcMedia media[ST_RTC_STREAMS];unsigned media_count;
    StRtcCandidate candidates[ST_RTC_CANDIDATES];unsigned candidate_count;
} StRtcOffer;
int st_rtc_candidate(StRtcOffer *o,const char *candidate);
int st_rtc_sdp(const char *sdp,StRtcOffer *o,StError *err);
int st_rtc_sdp_incoming(const char *sdp,StRtcOffer *o,StError *err);
int st_rtc_answer(const char *offer,const StRtcOffer *o,const char *ufrag,const char *password,const char *fingerprint,const char *ip,unsigned port,unsigned version,char *answer,size_t cap);
/* Only the first responses need protobuf: sending a JSON Ping switches the
 * LiveKit connection to JSON. Unknown protobuf fields are skipped with bounds. */
cJSON *st_rtc_signal(const unsigned char *data,size_t n);
size_t st_rtc_stun_request(unsigned char out[256],const unsigned char id[12],const char *remote,const char *local,const char *password,int controlling,uint64_t tie);
int st_rtc_stun_check(const unsigned char *p,size_t n,const char *password);
size_t st_rtc_stun_reply(const unsigned char *request,size_t n,const char *password,uint32_t ip,unsigned port,unsigned char out[256]);
int st_rtc_stun_attr(const unsigned char *p,size_t n,unsigned type,const unsigned char **value,size_t *size);
unsigned st_rtc_u16(const unsigned char *p);
uint32_t st_rtc_u32(const unsigned char *p);
void st_rtc_put16(unsigned char *p,unsigned n);
void st_rtc_put32(unsigned char *p,uint32_t n);
#endif
