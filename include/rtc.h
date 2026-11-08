#ifndef ST_RTC_H
#define ST_RTC_H
#include "rtc_protocol.h"
enum StRtcState { ST_RTC_PREPARE,ST_RTC_AUTH,ST_RTC_SIGNAL,ST_RTC_ICE,ST_RTC_DTLS,ST_RTC_LISTEN,ST_RTC_FAILED,ST_RTC_CLOSED };
#define ST_RTC_MEMBERS 32
typedef struct StRtc StRtc;
typedef struct {char user[64],sid[64],name[129],avatar[64],track[64];int connected,muted,volume,subscribed,speaking;unsigned level;} StRtcMember;
typedef struct {
    enum StRtcState state;
    unsigned received,decoded,concealed,late,auth_errors,pcm_dropped;
    unsigned dtls_steps,stun_requests,stun_replies,signals;
    unsigned mic_packets,mic_dropped,mix_clipped;int publish_ready,mic_muted,deafen;
    StRtcMember members[ST_RTC_MEMBERS];unsigned member_count;char self[64];
    char error[128],server[64];
} StRtcStats;
/* Address is resolved by the existing serial network worker BEFORE tokens are
 * minted. Prepare TLS/DTLS before requesting the API's ten-second join token. */
StRtc *st_rtc_new(const StConfig *cfg,const char *url,const char *address,const char *user,StError *err);
int st_rtc_authorize(StRtc *r,const char *token);
int st_rtc_step(StRtc *r);
void st_rtc_controls(StRtc *r,int mic_muted,int deafen);
void st_rtc_volume(StRtc *r,const char *user,int volume);
int st_rtc_send_opus(StRtc *r,const unsigned char *packet,size_t length);
void st_rtc_close(StRtc *r);
void st_rtc_free(StRtc *r);
StRtcStats st_rtc_stats(const StRtc *r);
enum StRtcState st_rtc_state(const StRtc *r);
int st_rtc_publishing(const StRtc *r);
/* One producer (network worker), one consumer (audio worker), bounded PCM FIFO. */
size_t st_rtc_pcm(StRtc *r,int16_t *out,size_t count);
#endif
