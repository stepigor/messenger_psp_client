#ifndef ST_PANELS_H
#define ST_PANELS_H
#include "stoat.h"
#include "notices.h"
#include "photos.h"
#include "ui_draw.h"
enum StPane { ST_RAIL, ST_NAV, ST_CONTENT, ST_MEMBERS };
enum StPage { ST_DMS, ST_FRIENDS, ST_SETTINGS, ST_CHAT, ST_DETAIL, ST_REACTIONS, ST_PROFILE, ST_EMPTY, ST_ACCOUNT, ST_CACHE, ST_STATUS, ST_LOGIN, ST_REGISTER, ST_ONBOARD, ST_APPEARANCE, ST_ATTACHMENTS,ST_PROFILE_ACTIONS,ST_MUTUAL_SERVERS,ST_MUTUAL_CHATS,ST_REPORT,ST_SESSIONS,ST_VOICE,ST_ADD_FRIEND,ST_REQUESTS,ST_REQUEST_ACTIONS,ST_MESSAGE_ACTIONS,ST_INVITE_LINKS,ST_INVITE,ST_SERVER_ACTIONS,ST_NOTIFICATIONS,ST_PHOTOS,ST_CHAT_ACTIONS,ST_IMAGE };
typedef struct {
    const StNotices *notices;
    const StPhotos *photos;
    char notify_id[64];int notify_server;enum StPage notify_origin;enum StPane notify_focus;int notify_cursor;
    enum StPane focus;
    enum StPage page;
    int chat_offset,chat_scroll_valid,chat_manual;uint64_t chat_scroll_at;
    int rail_cursor, nav_cursor, nav_tab, cursor, message, scroll, member_cursor;
    char server[64], channel[64], before[64],nearby[64];
    int member_width,nav_hidden;uint64_t animation_time;
    int call_active,call_failed;char call_channel[64],call_user[64],call_status[128];
    int loading, members_loading, lists_loading;
    char error[256], members_error[256],network_notice[128];
    char media_requested[64],media_error[128];
    char typing[600],friend_tag[257],invite_code[64];
    char person[64],profile_server[64];enum StPage profile_origin;enum StPane profile_focus;int profile_cursor;
    uint64_t cache_bytes;size_t cache_files;
    char email[256], password[512], invite[128], username[129];
} StPanels;
typedef struct { int rail,nav,content,members; } StPanelWidths;
int st_panels_chat_bottom(const StPanels *panels,const StClient *client);
int st_panels_chat_position(const StPanels *panels,const StClient *client,int message);
void st_panels_scroll_chat(StPanels *p,const StClient *c,int analog,uint64_t now);
void st_panels_init(StPanels *p);
void st_panels_reconcile(StPanels *p,const StClient *c);
void st_panels_animate(StPanels *p,const StClient *c,uint64_t now);
void st_panels_focus(StPanels *p,int direction);
void st_panels_back(StPanels *p);
void st_panels_move(StPanels *p,const StClient *c,int direction);
void st_panels_select_rail(StPanels *p,const StClient *c);
void st_panels_select_tab(StPanels *p);
void st_panels_open_chat(StPanels *p,const StClient *c,const StChannel *channel);
void st_panels_open_profile(StPanels *p,const char *id);
int st_panels_own_message(const StPanels *p,const StClient *c);
int st_panels_emoji_columns(const StPanels *p);
void st_panels_photo_move(StPanels *p,int dx,int dy,int page);
void st_panels_emoji_move(StPanels *p,int dx,int dy,int page);
int st_panels_count(const StPanels *p,const StClient *c,enum StPane pane);
int st_panels_channel(const StPanels *p,const StClient *c,int row);
int st_panels_friend(const StClient *c,int row);
int st_panels_request(const StClient *c,int row);
int st_panels_invites(const StPanels *p,const StClient *c,StInviteLink out[ST_INVITE_LINK_MAX]);
const StChannel *st_panels_current(const StPanels *p,const StClient *c);
StPanelWidths st_panels_widths(const StPanels *p);
void st_panels_draw(const StPanels *p,const StClient *c,const char *confirm,const char *cancel);
#endif
