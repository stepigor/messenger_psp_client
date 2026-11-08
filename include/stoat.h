#ifndef STOAT_H
#define STOAT_H
#include <stddef.h>
#include <stdint.h>
#include <cjson/cJSON.h>

#define ST_BODY_MAX (2u * 1024u * 1024u)
#define ST_TEXT_MAX 8001
#define ST_APP_NAME "Messenger"
#define ST_APP_SLUG "messenger_psp_client"
#define ST_APP_VERSION "1.0.0"
#define ST_PARENTAL_LEVEL 5
#define ST_DISC_ID "MSGP00001"
#define ST_API_VERSION "0.15.1"
typedef struct {
    char api_url[512], ca_file[512], font_file[512],emoji_file[512];
    int page_size;
} StConfig;
typedef struct { char text[256]; long status; } StError;
enum StPresence { ST_ONLINE,ST_IDLE,ST_FOCUS,ST_BUSY,ST_INVISIBLE,ST_OFFLINE };
typedef struct { char id[64], name[257], relationship[24], avatar[64],avatar_file[129],pronouns[129]; int avatar_animated,online;enum StPresence presence;int member_group;int64_t member_rank; } StUser;
typedef struct { char id[64],name[257]; } StSession;
#define ST_VOICE_MAX 4096
typedef struct { char channel[64],user[64];int receiving,publishing,screensharing,camera; } StVoiceMember;
typedef struct { char key[129];int index,mine;unsigned count; } StReaction;
#define ST_REACTION_MAX 64
typedef struct {char id[64],filename[129];int animated;} StFile;
typedef struct { char id[64], name[257], icon[64],owner[64];StFile banner; } StServer;
#define ST_ROLES_MAX 4096
#define ST_MEMBER_ROLES_MAX 16384
typedef struct {char id[64],server[64],name[129],colour[129];StFile icon;int64_t rank;int hoist;} StRole;
typedef struct {char user[64],nickname[129],pronouns[129];int has_pronouns;size_t role_start,role_count;} StMember;
#define ST_CUSTOM_EMOJI_MAX 4096
typedef struct { char id[27],server[64],name[33];int animated; } StCustomEmoji;
typedef struct { char id[64], name[257], server[64], type[24], peer[64], icon[64],last_message[64];int voice; } StChannel;
typedef struct {
    char id[64],name[129],type[24];int image,animated;
} StAttachment;
#define ST_ATTACHMENT_MAX 16
#define ST_REPLY_MAX 5
#define ST_EMBED_MAX 3
typedef struct {char type[16],title[129],description[513],url[257],site[65];unsigned colour;} StEmbed;
typedef struct { char id[64],author[64],name[129],content[513];int state;uint64_t retry; } StQuote;
typedef struct {
    char id[64], author[64], content[ST_TEXT_MAX], reactions[512], image[64];
    char system[40];
    int64_t sent_ms;
    StAttachment files[ST_ATTACHMENT_MAX];
    int attachments,embeds;
    StEmbed previews[ST_EMBED_MAX];int preview_count;
    StReaction reaction_list[ST_REACTION_MAX];int reaction_count;
    StQuote replies[ST_REPLY_MAX];int reply_count,reply_total;
} StMessage;
typedef struct {
    StConfig config;
    cJSON *notifications,*notification_roles;
    char photo_signature[1024],photo_id[64],photo_key[65];
    char token[256], session_id[64],self_id[64], ws_url[512], assets_url[512],app_url[512], version[32];
    char voice_url[512],voice_node[128];
    StUser *users; size_t user_count;
    StServer *servers; size_t server_count;
    StRole *roles;size_t role_count;
    StMember *members;size_t member_count;
    char (*member_roles)[64];size_t member_role_count;char members_server[64];
    StCustomEmoji *emojis; size_t emoji_count;
    StChannel *channels; size_t channel_count;
    StVoiceMember *voice;size_t voice_count;
    StMessage *messages; size_t message_count;
    StUser *participants; size_t participant_count;
    char participants_channel[64], messages_channel[64];
    cJSON *self, *profile,*invitation;
    char invitation_code[64],joined_server[64],joined_channel[64];
    cJSON *profile_user,*mutual;
    char profile_id[64],profile_server[64],opened_dm[64];
    StSession *sessions;size_t session_count;
    int invite_only, captcha, email_verification, discovered, onboarding;
    /* Kept after an ambiguous POST failure so a retry uses the same key. */
    char pending_key[65], pending_channel[64], pending_text[ST_TEXT_MAX],pending_reply[64];
} StClient;

void st_copy(char *dst, size_t cap, const char *src);
void st_wipe(void *p, size_t n);
const char *st_str(const cJSON *obj, const char *key);
enum StPresence st_presence_value(const char *value);
const char *st_presence_label(enum StPresence value);
int st_fail(StError *err, const char *text);
int st_config_load(const char *path, StConfig *config, StError *err);
int st_config_load_assets(const char *path,const char *assets,StConfig *config,StError *err);
int st_url_split(const char *url, const char *scheme, char *host, size_t hc,
                 char *path, size_t pc);
uint32_t st_utf8_next(const char **text);
size_t st_utf8_to_utf16(const char *text, uint16_t *out, size_t cap);
int st_utf16_to_utf8(const uint16_t *text, char *out, size_t cap);
int st_utf8_count(const char *text, size_t *count);

/* Platform hooks: cancellation, monotonic clock, delay, secure random bytes. */
int st_cancelled(void);
uint64_t st_millis(void);
void st_pause(void);
int st_random(void *out, size_t length);
void st_stage(const char *stage);
void st_network_abort(void);

/* Plaintext bearer token on removable storage, bound to the API URL. */
int st_session_load(StClient *c, const char *path, StError *err);
int st_session_save(const StClient *c, const char *path, StError *err);
int st_session_forget(const char *path, StError *err);

int st_http(StClient *c, const char *method, const char *path, cJSON *body,
            const char *key, cJSON **out, StError *err);
int st_ws_ready(StClient *c, cJSON **out, StError *err);
int st_http_ticket(StClient *c,const char *method,const char *path,cJSON *body,const char *ticket,cJSON **out,StError *err);
int st_ws_accept(const char *key, char out[29]);
/* Decodes one server frame: 0 incomplete, -1 invalid, 1 complete. */
int st_ws_frame(const unsigned char *buf, size_t n, size_t *header,
                size_t *length, int *opcode, int *fin);
int st_ws_masked(int opcode, const void *data, size_t n,
                 unsigned char **out, size_t *size);

void st_client_init(StClient *c, const StConfig *config);
void st_client_clear(StClient *c);
int st_probe(StClient *c, StError *err);
int st_register(StClient *c,const char *email,const char *password,const char *invite,StError *err);
int st_onboarding(StClient *c,StError *err);
int st_onboard(StClient *c,const char *username,StError *err);
int st_status(StClient *c,const char *text,const char *presence,StError *err);
int st_login(StClient *c, const char *email, const char *password, StError *err);
int st_bootstrap(StClient *c, StError *err);
int st_load_ready(StClient *c, const cJSON *ready, StError *err);
int st_refresh_voice(StClient *c,StError *err);
int st_voice_count(const StClient *c,const char *channel);
int st_channel_can_call(const StChannel *channel);
const StVoiceMember *st_voice_member(const StClient *c,const char *channel,int row);
int st_fetch_profile(StClient *c, StError *err);
int st_fetch_user_profile(StClient *c,const char *id,StError *err);
int st_fetch_user_profile_context(StClient *c,const char *id,const char *server,StError *err);
int st_user_action(StClient *c,const char *id,const char *action,StError *err);
int st_friend_request(StClient *c,const char *username,StError *err);
int st_merge_user(StClient *c,const cJSON *user,StError *err);
int st_fetch_invite(StClient *c,const char *code,StError *err);
int st_join_invite(StClient *c,const char *code,StError *err);
int st_leave_server(StClient *c,const char *id,StError *err);
void st_remove_server(StClient *c,const char *id);
#define ST_INVITE_LINK_MAX 8
typedef struct {char code[64];} StInviteLink;
int st_invite_links(const StClient *c,const char *text,StInviteLink out[ST_INVITE_LINK_MAX]);
int st_open_dm(StClient *c,const char *id,StError *err);
int st_report_user(StClient *c,const char *id,const char *reason,const char *text,StError *err);
int st_fetch_sessions(StClient *c,StError *err);
int st_revoke_sessions(StClient *c,const char *id,int all,const char *password,StError *err);
const StReaction *st_message_reaction(const StMessage *m,int emoji);
const StReaction *st_message_reaction_key(const StMessage *m,const char *key);
int st_fetch_messages(StClient *c, const char *channel, const char *before, StError *err);
int st_fetch_messages_nearby(StClient *c,const char *channel,const char *nearby,StError *err);
void st_message_parse(const StClient *c,const cJSON *object,const char *channel,StMessage *message);
int st_fetch_participants(StClient *c, const char *channel, StError *err);
/* Returns 1/0 for ViewChannel, -1 for malformed permission metadata. */
int st_member_can_view(const cJSON *server, const cJSON *channel,
                      const cJSON *member, const cJSON *user, StError *err);
int st_send(StClient *c, const char *channel, const char *content, StError *err);
int st_send_reply(StClient *c,const char *channel,const char *content,const char *reply,StError *err);
int st_fetch_quote(StClient *c,const char *channel,const char *id,StQuote *quote,StError *err);
void st_quotes_resolve(StClient *c,StMessage *messages,size_t count,const char *channel);
void st_quotes_invalidate(StClient *c,const char *id,int deleted);
int st_delete_message(StClient *c,const char *channel,const char *id,StError *err);
void st_remove_message(StClient *c,const char *channel,const char *id);
int st_leave_group(StClient *c,const char *id,StError *err);
void st_remove_channel(StClient *c,const char *id);
int st_react(StClient *c, const char *channel, const char *message,
             const char *emoji, int remove, StError *err);
int st_logout(StClient *c, StError *err);
const char *st_user_name(const StClient *c, const char *id);
const char *st_channel_name(const StClient *c, const StChannel *channel);
/* Presentation metadata is separate from permission checks. Member cache is
 * scoped to one server; role IDs are kept in a compact shared pool. */
int st_roles_parse(const cJSON *servers,StRole **out,size_t *count,StError *err);
int st_server_merge(StClient *c,const cJSON *server,StError *err);
void st_server_file(const cJSON *file,const char *tag,StFile *out);
enum StMemberMerge { ST_MEMBERS_MERGE,ST_MEMBERS_REPLACE,ST_MEMBERS_PATCH };
int st_members_merge(StClient *c,const char *server,const cJSON *members,int mode,StError *err);
void st_member_remove(StClient *c,const char *server,const char *user);
void st_members_clear(StClient *c);
const StMember *st_member(const StClient *c,const char *server,const char *user);
const StRole *st_member_role(const StClient *c,const char *server,const char *user,int field);
int st_member_roles(const StClient *c,const char *server,const char *user,const StRole *out[256]);
const char *st_context_name(const StClient *c,const char *server,const char *user);
const char *st_context_pronouns(const StClient *c,const char *server,const char *user);
int st_role_colour(const char *text,unsigned *colour);
typedef struct {unsigned colours[8];float stops[8],dx,dy;int count;} StRoleGradient;
int st_role_gradient(const char *text,StRoleGradient *out);
void st_participants_sort(StClient *c);
int st_timestamp_label(int64_t milliseconds,int offset_minutes,char out[24]);
int st_local_offset_minutes(void);
#endif
