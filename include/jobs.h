#ifndef ST_JOBS_H
#define ST_JOBS_H
#include "media.h"
#include "thumbnail.h"
#include "live.h"
#include "sound.h"
#include "disk_cache.h"
#include "call_api.h"
enum StJobType { ST_JOB_PROBE,ST_JOB_LOGIN,ST_JOB_BOOTSTRAP,ST_JOB_PROFILE,ST_JOB_HISTORY,ST_JOB_SEND,ST_JOB_REACT,ST_JOB_LOGOUT,ST_JOB_MEMBERS,ST_JOB_REGISTER,ST_JOB_HELLO,ST_JOB_ONBOARD,ST_JOB_STATUS,ST_JOB_CACHE_STATS,ST_JOB_CACHE_CLEAR,ST_JOB_MEDIA,ST_JOB_USER,ST_JOB_USER_ACTION,ST_JOB_DM,ST_JOB_REPORT,ST_JOB_SESSIONS,ST_JOB_REVOKE,ST_JOB_VOICE,ST_JOB_LIVE,ST_JOB_LIVE_HISTORY,ST_JOB_FRIEND,ST_JOB_INVITE,ST_JOB_JOIN,ST_JOB_LEAVE,ST_JOB_LIVE_CLOSE,ST_JOB_QUOTE,ST_JOB_SOUND,ST_JOB_NOTICES,ST_JOB_NOTICES_SAVE,ST_JOB_ACK,ST_JOB_MENTION,ST_JOB_CALL_PREPARE,ST_JOB_CALL_AUTH,ST_JOB_CALL_CATCHUP,ST_JOB_PREFS_FETCH,ST_JOB_PREFS_SET,ST_JOB_PHOTO,ST_JOB_THUMBNAIL,ST_JOB_LEAVE_GROUP,ST_JOB_DELETE_MESSAGE };
typedef struct {
    enum StJobType type;StClient result;StError error;int ok,remove;
    unsigned generation;char a[ST_TEXT_MAX],b[ST_TEXT_MAX],d[256],session[256];
    StMediaRequest media;StImage image;StCacheStats stats;
    cJSON *events;int live_connected;unsigned revision;
    StQuote quote;StSound sound,effects[ST_FX_COUNT];StCallTicket call;
} StJob;
int st_job_prepare(StJob *job,const StClient *c,enum StJobType type,unsigned generation,const char *a,const char *b,const char *d,int remove,StError *err);
void st_job_execute(StJob *job);
/* Returns 0 when the result belongs to a departed view/account. Mutations still
 * retain their idempotency state when the user navigates elsewhere. */
int st_job_apply(StJob *job,StClient *c,unsigned generation,const char *channel);
int st_job_read(enum StJobType type);
void st_job_clear(StJob *job);
#endif
