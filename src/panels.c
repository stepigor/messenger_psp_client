#include "panels.h"
#include "disk_cache.h"
#include "emoji.h"
#include "reactions.h"
#include "markdown.h"
#include "preferences.h"
#include "theme_data.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *const tabs[]={"Messages","Friends","My account"};
static int clamp(int value,int count){return count<=0?0:value<0?0:value>=count?count-1:value;}
static void profile_text(const StClient *c,char *out,size_t cap);
static int reaction_chips(const char *text,int x,int y,int width,int draw);
static int message_heading(const StClient *c,const StMessage *m,int x,int y,int width,int draw);
static int profile_lines(const StClient *c,int width);
static int detail_visible(const StClient *c,const StMessage *m,int width) {
    int n=(216-reaction_chips(m->reactions,0,0,width,0)-(m->attachments||m->embeds?24:0)-36-message_heading(c,m,0,0,width,0))/19;
    return n<1?1:n;
}
static int channel_count(const StPanels *p,const StClient *c) {
    int n=p->server[0]?0:1;
    for(size_t i=0;i<c->channel_count;i++) {
        const StChannel *ch=c->channels+i;
        if(p->server[0]?(!strcmp(ch->server,p->server) && (!strcmp(ch->type,"TextChannel")||!strcmp(ch->type,"VoiceChannel"))):
           (!strcmp(ch->type,"DirectMessage")||!strcmp(ch->type,"Group")))n++;
    }
    return n;
}
void st_panels_init(StPanels *p) {
    memset(p,0,sizeof(*p));p->focus=ST_CONTENT;p->page=ST_LOGIN;p->nav_tab=2;p->nav_cursor=2;
}
int st_panels_channel(const StPanels *p,const StClient *c,int row) {
    if(!p->server[0]){if(row==0){for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].type,"SavedMessages"))return (int)i;return -1;}row--;}
    for(size_t i=0;i<c->channel_count;i++) {
        const StChannel *ch=c->channels+i;
        int matches=p->server[0]?(!strcmp(ch->server,p->server) && (!strcmp(ch->type,"TextChannel")||!strcmp(ch->type,"VoiceChannel"))):
            (!strcmp(ch->type,"DirectMessage")||!strcmp(ch->type,"Group"));
        if(matches && row--==0)return (int)i;
    }
    return -1;
}
int st_panels_friend(const StClient *c,int row) {
    for(size_t i=0;i<c->user_count;i++)if(!strcmp(c->users[i].relationship,"Friend") && row--==0)return (int)i;
    return -1;
}
int st_panels_request(const StClient *c,int row) {
    for(size_t i=0;i<c->user_count;i++)if((!strcmp(c->users[i].relationship,"Incoming")||!strcmp(c->users[i].relationship,"Outgoing"))&&row--==0)return (int)i;
    return -1;
}
int st_panels_invites(const StPanels *p,const StClient *c,StInviteLink out[ST_INVITE_LINK_MAX]) {
    if(p->message<0||p->message>=(int)c->message_count)return 0;
    return st_invite_links(c,c->messages[c->message_count-1-(size_t)p->message].content,out);
}
const StChannel *st_panels_current(const StPanels *p,const StClient *c) {
    for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,p->channel))return c->channels+i;
    return NULL;
}
int st_panels_own_message(const StPanels *p,const StClient *c){
    if(strcmp(p->channel,c->messages_channel)||p->message<0||(size_t)p->message>=c->message_count||!c->self_id[0])return 0;
    const StMessage *m=c->messages+c->message_count-1-(size_t)p->message;
    return !m->system[0]&&!strcmp(m->author,c->self_id);
}
int st_panels_count(const StPanels *p,const StClient *c,enum StPane pane) {
    if(pane==ST_RAIL)return (int)c->server_count+1;
    if(pane==ST_MEMBERS)return !strcmp(p->channel,c->participants_channel)?(int)c->participant_count:0;
    if(pane==ST_NAV) {
        if(!p->server[0])return 3;
        return channel_count(p,c);
    }
    switch(p->page) {
        case ST_DMS:return channel_count(p,c);
        case ST_FRIENDS:{
            int n=2;for(size_t i=0;i<c->user_count;i++)if(!strcmp(c->users[i].relationship,"Friend"))n++;return n;
        }
        case ST_ADD_FRIEND:return 2;
        case ST_REQUESTS:{int n=0;while(st_panels_request(c,n)>=0)n++;return n;}
        case ST_REQUEST_ACTIONS:{for(size_t i=0;i<c->user_count;i++)if(!strcmp(c->users[i].id,p->person))return !strcmp(c->users[i].relationship,"Incoming")?2:!strcmp(c->users[i].relationship,"Outgoing")?1:0;return 0;}
        case ST_MESSAGE_ACTIONS:return 2+st_panels_own_message(p,c);
        case ST_INVITE_LINKS:{StInviteLink links[ST_INVITE_LINK_MAX];return st_panels_invites(p,c,links);}
        case ST_INVITE:return !strcmp(c->invitation_code,p->invite_code)&&c->invitation?1:0;
        case ST_SERVER_ACTIONS:return 3;
        case ST_NOTIFICATIONS:return 3;
        case ST_CHAT_ACTIONS:{const StChannel *ch=st_panels_current(p,c);return (st_channel_can_call(ch)?4:3)+(ch&&!strcmp(ch->type,"Group"));}
        case ST_PHOTOS:return p->photos?(int)p->photos->count+(p->photos->sources?0:1):0;
        case ST_SETTINGS:return 4;
        case ST_APPEARANCE:return ST_THEME_COLORS+1;
        case ST_IMAGE:case ST_ATTACHMENTS:if(c->message_count){const StMessage *m=c->messages+c->message_count-1-(size_t)clamp(p->message,(int)c->message_count);return (m->attachments>ST_ATTACHMENT_MAX?ST_ATTACHMENT_MAX:m->attachments)+m->preview_count;}return 0;
        case ST_ACCOUNT:return 4;
        case ST_CACHE:return 0;
        case ST_STATUS:return 7;
        case ST_LOGIN:return 5;
        case ST_REGISTER:return c->invite_only?5:4;
        case ST_ONBOARD:return 2;
        case ST_CHAT:return (int)c->message_count;
        case ST_VOICE:return st_voice_count(c,p->channel);
        case ST_REACTIONS:st_reactions_bind(c);return st_reactions_count();
        case ST_PROFILE_ACTIONS:return strcmp(p->person,c->self_id)?6:0;
        case ST_MUTUAL_SERVERS:case ST_MUTUAL_CHATS:return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(c->mutual,p->page==ST_MUTUAL_SERVERS?"servers":"channels"));
        case ST_REPORT:return 7;
        case ST_SESSIONS:return 2+(int)c->session_count;
        default:return 0;
    }
}
void st_panels_reconcile(StPanels *p,const StClient *c) {
    if(p->server[0]) {
        size_t i;for(i=0;i<c->server_count;i++)if(!strcmp(c->servers[i].id,p->server))break;
        if(i==c->server_count) {
            p->server[0]=p->channel[0]=p->before[0]=p->nearby[0]=0;p->page=ST_DMS;p->nav_tab=0;p->nav_cursor=0;p->focus=ST_NAV;
        }
    }
    if(p->channel[0] && !st_panels_current(p,c)) {
        p->channel[0]=p->before[0]=p->nearby[0]=0;p->page=p->server[0]?ST_EMPTY:ST_DMS;p->focus=ST_CONTENT;
    }
    p->rail_cursor=clamp(p->rail_cursor,st_panels_count(p,c,ST_RAIL));
    p->nav_cursor=clamp(p->nav_cursor,st_panels_count(p,c,ST_NAV));
    p->cursor=clamp(p->cursor,st_panels_count(p,c,ST_CONTENT));
    p->message=clamp(p->message,(int)c->message_count);
    p->member_cursor=clamp(p->member_cursor,st_panels_count(p,c,ST_MEMBERS));
    if(!p->channel[0] && p->focus==ST_MEMBERS)p->focus=ST_CONTENT;
}
static int conversation_page(const StPanels *p){
    return p->channel[0]&&(p->page==ST_CHAT||p->page==ST_DETAIL||p->page==ST_REACTIONS||p->page==ST_ATTACHMENTS||p->page==ST_IMAGE||p->page==ST_CHAT_ACTIONS||p->page==ST_MESSAGE_ACTIONS||p->page==ST_INVITE_LINKS||p->page==ST_INVITE);
}
void st_panels_animate(StPanels *p,const StClient *c,uint64_t now) {
    uint64_t delta=now>=p->animation_time?now-p->animation_time:0;p->animation_time=now;
    int target=p->focus==ST_MEMBERS&&p->channel[0]?120:0,nav_target=p->focus>=ST_CONTENT&&conversation_page(p)?144:0;
    int anchor=p->page==ST_CHAT&&p->chat_manual&&c&&delta&&(p->member_width!=target||p->nav_hidden!=nav_target);
    int old_position=anchor?st_panels_chat_position(p,c,p->cursor):0,old_width=st_panels_widths(p).content;
    if(delta>250)delta=250;
    int step=(int)(delta*120/200);if(delta&&!step)step=1;
    if(p->member_width<target){p->member_width+=step;if(p->member_width>target)p->member_width=target;}
    if(p->member_width>target){p->member_width-=step;if(p->member_width<target)p->member_width=target;}
    target=nav_target;step=(int)(delta*144/200);if(delta&&!step)step=1;
    if(p->nav_hidden<target){p->nav_hidden+=step;if(p->nav_hidden>target)p->nav_hidden=target;}
    if(p->nav_hidden>target){p->nav_hidden-=step;if(p->nav_hidden<target)p->nav_hidden=target;}
    if(anchor&&st_panels_widths(p).content!=old_width){p->chat_offset+=st_panels_chat_position(p,c,p->cursor)-old_position;if(p->chat_offset<0)p->chat_offset=0;}
}
void st_panels_focus(StPanels *p,int direction) {
    if(direction<0) {if(p->focus>ST_RAIL)p->focus--;return;}
    if(p->focus<ST_CONTENT)p->focus++;
    else if(p->focus==ST_CONTENT && p->channel[0] &&
            (p->page==ST_CHAT||p->page==ST_DETAIL||p->page==ST_REACTIONS||p->page==ST_ATTACHMENTS))p->focus=ST_MEMBERS;
}
void st_panels_back(StPanels *p) {
    if(p->page==ST_IMAGE){p->page=ST_ATTACHMENTS;p->focus=ST_CONTENT;return;}
    if(p->focus!=ST_CONTENT){st_panels_focus(p,-1);return;}
    if(p->page==ST_NOTIFICATIONS){p->page=p->notify_origin;p->focus=p->notify_focus;p->cursor=p->notify_cursor;return;}
    if(p->page==ST_CHAT_ACTIONS){p->page=ST_CHAT;p->cursor=p->message;return;}
    if(p->page==ST_PHOTOS){p->page=ST_CHAT;p->cursor=p->message;return;}
    if(p->page==ST_ADD_FRIEND||p->page==ST_REQUESTS){p->page=ST_FRIENDS;p->cursor=0;}
    else if(p->page==ST_REQUEST_ACTIONS){p->page=ST_REQUESTS;p->cursor=0;}
    else if(p->page==ST_INVITE){p->page=ST_INVITE_LINKS;p->cursor=0;}
    else if(p->page==ST_INVITE_LINKS){p->page=ST_MESSAGE_ACTIONS;p->cursor=1;}
    else if(p->page==ST_SERVER_ACTIONS){p->page=ST_EMPTY;p->focus=ST_NAV;p->cursor=0;}
    else if(p->page==ST_MESSAGE_ACTIONS){p->page=ST_DETAIL;p->scroll=0;}
    else if(p->page==ST_REACTIONS||p->page==ST_ATTACHMENTS){p->page=ST_DETAIL;p->scroll=0;p->media_requested[0]=p->media_error[0]=0;}
    else if(p->page==ST_DETAIL){p->page=ST_CHAT;p->cursor=p->message;p->scroll=0;}
    else if(p->page==ST_PROFILE){p->page=p->profile_origin;p->focus=p->profile_focus;p->cursor=p->profile_cursor;p->scroll=0;}
    else if(p->page==ST_PROFILE_ACTIONS){p->page=ST_PROFILE;p->scroll=p->cursor=0;}
    else if(p->page==ST_MUTUAL_SERVERS||p->page==ST_MUTUAL_CHATS||p->page==ST_REPORT){p->page=ST_PROFILE_ACTIONS;p->cursor=0;}
    else if(p->page==ST_STATUS||p->page==ST_SETTINGS){p->page=ST_ACCOUNT;p->cursor=0;p->scroll=0;}
    else if(p->page==ST_CACHE){p->page=ST_SETTINGS;p->cursor=0;}
    else if(p->page==ST_APPEARANCE){p->page=ST_SETTINGS;p->cursor=1;}
    else if(p->page==ST_SESSIONS){p->page=ST_SETTINGS;p->cursor=2;}
    else if(p->page==ST_REGISTER){p->page=ST_LOGIN;p->cursor=0;}
    else if(p->page==ST_CHAT && !p->server[0]){p->page=ST_DMS;p->channel[0]=p->before[0]=p->nearby[0]=0;p->cursor=0;}
    else p->focus=ST_NAV;
}
void st_panels_move(StPanels *p,const StClient *c,int direction) {
    const StChannel *context=st_panels_current(p,c);ui_mentions(c,context?context->server:"");
    if(p->focus==ST_CONTENT && (p->page==ST_DETAIL || p->page==ST_PROFILE)) {
        int width=st_panels_widths(p).content-16,lines=0,visible=p->loading?9:11;
        if(p->page==ST_DETAIL && c->message_count) {
            size_t index=c->message_count-1-(size_t)clamp(p->message,(int)c->message_count);
            visible=detail_visible(c,c->messages+index,width);
            lines=ui_markdown_lines(c->messages[index].content,width)+c->messages[index].reply_count*2;
        } else if(p->page==ST_PROFILE) {
            lines=profile_lines(c,width);visible=4;
        }
        int limit=lines>visible?lines-visible:0;
        p->scroll=clamp(p->scroll+direction,limit+1);
        return;
    }
    int n=st_panels_count(p,c,p->focus);
    int *cursor=p->focus==ST_RAIL?&p->rail_cursor:p->focus==ST_NAV?&p->nav_cursor:
                p->focus==ST_MEMBERS?&p->member_cursor:&p->cursor;
    int previous=*cursor;*cursor=clamp(*cursor+direction,n);
    if(p->page==ST_CHAT&&p->focus==ST_CONTENT)p->chat_manual=0;
    if(p->page==ST_ATTACHMENTS&&p->focus==ST_CONTENT&&*cursor!=previous)p->media_requested[0]=p->media_error[0]=0;
}
void st_panels_select_rail(StPanels *p,const StClient *c) {
    p->rail_cursor=clamp(p->rail_cursor,(int)c->server_count+1);
    p->server[0]=p->channel[0]=p->before[0]=p->nearby[0]=0;p->cursor=p->scroll=p->nav_cursor=0;p->focus=ST_NAV;
    if(p->rail_cursor){st_copy(p->server,sizeof(p->server),c->servers[p->rail_cursor-1].id);p->page=ST_EMPTY;}
    else {p->nav_tab=c->token[0]?0:2;p->nav_cursor=p->nav_tab;p->page=c->token[0]?ST_DMS:ST_LOGIN;}
}
void st_panels_select_tab(StPanels *p) {
    if(p->server[0])return;
    p->nav_tab=clamp(p->nav_cursor,3);p->page=p->nav_tab==0?ST_DMS:p->nav_tab==1?ST_FRIENDS:ST_ACCOUNT;
    p->channel[0]=p->before[0]=p->nearby[0]=0;p->cursor=p->scroll=0;p->focus=ST_CONTENT;
}
void st_panels_open_chat(StPanels *p,const StClient *c,const StChannel *channel) {
    st_copy(p->channel,sizeof(p->channel),channel->id);p->before[0]=p->nearby[0]=0;
    p->page=channel->voice?ST_VOICE:ST_CHAT;p->focus=ST_CONTENT;p->cursor=c->message_count?(int)c->message_count-1:0;
    p->message=p->cursor;p->scroll=p->member_cursor=0;p->chat_manual=p->chat_offset=0;p->chat_scroll_at=0;
}
void st_panels_open_profile(StPanels *p,const char *id) {
    p->profile_origin=p->page;p->profile_focus=p->focus;p->profile_cursor=p->cursor;
    st_copy(p->profile_server,64,p->server);
    st_copy(p->person,sizeof(p->person),id);p->page=ST_PROFILE;p->focus=ST_CONTENT;p->scroll=p->cursor=0;
}
int st_panels_emoji_columns(const StPanels *p){int columns=(st_panels_widths(p).content-16)/32;return columns<1?1:columns;}
void st_panels_photo_move(StPanels *p,int dx,int dy,int page){int count=p->photos?(int)p->photos->count+(p->photos->sources?0:1):0;p->cursor=clamp(p->cursor+dx+dy*ST_PHOTO_COLUMNS+page*ST_PHOTO_PAGE,count);}
void st_panels_emoji_move(StPanels *p,int dx,int dy,int page){int cols=st_panels_emoji_columns(p);p->cursor=st_reactions_move(p->cursor,cols,dx,dy,page);}
StPanelWidths st_panels_widths(const StPanels *p) {
    int nav=144-clamp(p->nav_hidden,145),members=clamp(p->member_width,121);
    return (StPanelWidths){44,nav,436-nav-members,members};
}
static void header(int x,int width,const char *label,int focused) {
    ui_text_clip(x+7,5,width-14,label,focused?UI_ACCENT:UI_WHITE);
    ui_rect(x,25,width,focused?2:1,focused?UI_ACCENT:UI_LINE);
}
static void row(int x,int y,int width,const char *label,int selected,int focused) {
    if(selected)ui_rect(x+3,y-2,width-6,22,UI_SELECTED);
    if(selected && focused)ui_rect(x+3,y-2,2,22,UI_ACCENT);
    ui_text_clip(x+8,y,width-16,label,selected&&focused?UI_ACCENT:UI_WHITE);
}
static void menu_row(int x,int y,int width,const char *label,enum UiIcon icon,int selected,int focused) {
    row(x,y,width,"",selected,focused);unsigned color=selected&&focused?UI_ACCENT:UI_DIM;
    ui_icon(icon,x+9,y+2,color);ui_text_clip(x+32,y,width-40,label,selected&&focused?UI_ACCENT:UI_WHITE);
}
static enum UiIcon menu_icon(enum StPage page,int i,const StClient *c) {
    static const enum UiIcon settings[]={UI_ICON_CACHE,UI_ICON_PALETTE,UI_ICON_DEVICES,UI_ICON_MIC},account[]={UI_ICON_USER,UI_ICON_ONLINE,UI_ICON_SETTINGS,UI_ICON_LOGOUT},status[]={UI_ICON_TEXT,UI_ICON_ONLINE,UI_ICON_MOON,UI_ICON_FOCUS,UI_ICON_BUSY,UI_ICON_INVISIBLE,UI_ICON_TRASH},actions[]={UI_ICON_MESSAGE,UI_ICON_ADD_USER,UI_ICON_BLOCK,UI_ICON_FLAG,UI_ICON_SERVERS,UI_ICON_MESSAGE};
    if(page==ST_ADD_FRIEND)return i?UI_ICON_ADD_USER:UI_ICON_USER;
    if(page==ST_REQUEST_ACTIONS)return i?UI_ICON_REMOVE_USER:UI_ICON_ADD_USER;
    if(page==ST_MESSAGE_ACTIONS)return i==2?UI_ICON_TRASH:i?UI_ICON_INVITE:UI_ICON_FILE;
    if(page==ST_INVITE_LINKS||page==ST_INVITE)return UI_ICON_INVITE;
    if(page==ST_CHAT_ACTIONS)return i==4?UI_ICON_LOGOUT:i==0?UI_ICON_IMAGE:i==1?UI_ICON_MESSAGE:i==2?UI_ICON_HASH:UI_ICON_VOICE;
    if(page==ST_NOTIFICATIONS)return i==2?UI_ICON_AUDIO_OFF:UI_ICON_MESSAGE;
    if(page==ST_SERVER_ACTIONS)return i==2?UI_ICON_LOGOUT:i==1?UI_ICON_MESSAGE:UI_ICON_SERVERS;
    if(page==ST_SETTINGS)return settings[i];
    if(page==ST_ACCOUNT)return account[i];
    if(page==ST_STATUS)return status[i];
    if(page==ST_PROFILE_ACTIONS){const char *r=st_str(c->profile_user,"relationship");if(i==1&&(!strcmp(r,"Friend")||!strcmp(r,"Outgoing")))return UI_ICON_REMOVE_USER;if(i==2&&!strcmp(r,"Blocked"))return UI_ICON_UNBLOCK;return actions[i];}
    if(page==ST_MUTUAL_SERVERS)return UI_ICON_SERVERS;
    if(page==ST_MUTUAL_CHATS)return UI_ICON_MESSAGE;
    if(page==ST_REPORT)return UI_ICON_FLAG;
    if(page==ST_SESSIONS)return i==1?UI_ICON_LOGOUT:i==0?UI_ICON_BLOCK:UI_ICON_DEVICES;
    return UI_ICON_FILE;
}
static void badge(const char *name,char out[9]) {
    const char *p=name;size_t n=0;
    for(int i=0;i<2 && *p;i++){const char *start=p;st_utf8_next(&p);size_t k=(size_t)(p-start);memcpy(out+n,start,k);n+=k;}
    out[n]=0;
}
static void profile_text(const StClient *c,char *out,size_t cap) {
    const cJSON *user=c->profile_user?c->profile_user:c->self,*status=cJSON_GetObjectItemCaseSensitive(user,"status");
    const char *pronouns=c->profile_server[0]?st_context_pronouns(c,c->profile_server,c->profile_id):st_str(user,"pronouns");
    snprintf(out,cap,"Status: %s\n%s%s\nAbout me:\n%s",st_str(status,"text"),*pronouns?"Pronouns: ":"",pronouns,st_str(c->profile,"content"));
}
static int profile_lines(const StClient *c,int width){char text[9000];profile_text(c,text,sizeof(text));const StRole *roles[256];int n=st_member_roles(c,c->profile_server,c->profile_id,roles);return ui_line_count(text,width)+(c->profile_server[0]?1+(n?n:1):0);}
static const StUser *user_by_id(const StClient *c,const char *id) {
    for(size_t i=0;i<c->user_count;i++)if(!strcmp(c->users[i].id,id))return c->users+i;
    return NULL;
}
static void default_avatar(StMediaRequest *key,const char *id){if(!key->id[0]&&id&&*id){st_copy(key->tag,sizeof(key->tag),"default");st_copy(key->id,sizeof(key->id),id);}}
static StMediaRequest user_avatar(const StUser *u){StMediaRequest key={.tag="avatars"};if(u){st_copy(key.id,sizeof(key.id),u->avatar);st_copy(key.filename,sizeof(key.filename),u->avatar_file);key.animated=u->avatar_animated;default_avatar(&key,u->id);}return key;}
static int draw_avatar(const StClient *c,const char *id,int x,int y,int size) {
    StMediaRequest key=user_avatar(user_by_id(c,id));
    if(!strcmp(id,c->self_id)&&cJSON_HasObjectItem(c->self,"avatar"))st_profile_image_ref(cJSON_GetObjectItemCaseSensitive(c->self,"avatar"),"avatars",&key);
    default_avatar(&key,id);return ui_image_ref(&key,x,y,size,size,1);
}
static void picture_row_tint(int x,int y,int width,const char *label,const char *tag,const char *id,const StUser *person,int selected,int focused,unsigned color,const char *style) {
    row(x,y,width,"",selected,focused);
    ui_circle(x+8,y-1,20,UI_SIDE);
    StMediaRequest key=user_avatar(person);
    int painted=person?ui_image_ref(&key,x+8,y-1,20,20,1):ui_image(tag,id,x+8,y-1,20,20,1);
    if(!painted){char initials[9];badge(label,initials);ui_text_clip(x+8,y,20,initials,UI_DIM);}
    ui_role_text_clip(x+34,y,width-42,label,style,color);
}
static void picture_row(int x,int y,int width,const char *label,const char *tag,const char *id,const StUser *person,int selected,int focused){picture_row_tint(x,y,width,label,tag,id,person,selected,focused,selected&&focused?UI_ACCENT:UI_WHITE,NULL);}
static enum StPresence self_presence(const StClient *c) {
    return st_presence_value(st_str(cJSON_GetObjectItemCaseSensitive(c->self,"status"),"presence"));
}
static void account_label(const StClient *c,char *label,size_t cap) {
    const char *name=st_str(c->self,"username"),*code=st_str(c->self,"discriminator");
    if(!*name)name=st_user_name(c,c->self_id);
    snprintf(label,cap,"%s%s%s",*name?name:"My account",*code?"#":"",code);
}
static void account_heading(const StClient *c,int x,int width,unsigned color) {
    const char *name=st_str(c->self,"username"),*code=st_str(c->self,"discriminator");
    if(!*name)name=st_user_name(c,c->self_id);
    char suffix[32];snprintf(suffix,sizeof(suffix),"%s%s",*code?"#":"",code);
    int room=width-14-ui_text_width(suffix);if(room<0)room=0;
    int used=ui_text_width(name);if(used>room)used=room;
    ui_text_clip(x+7,5,room,name,color);ui_text_clip(x+7+used,5,width-14-used,suffix,color);
}
static void presence_row(int x,int y,int width,enum StPresence value,unsigned background) {
    ui_presence(x,y+5,10,value,background);ui_text_clip(x+17,y,width-17,st_presence_label(value),UI_DIM);
}
static void person_row_tint(const StClient *c,const StUser *user,int x,int y,int width,int selected,int focused,unsigned color,const char *style) {
    picture_row_tint(x,y,width,user->name,"avatars",user->avatar,user,selected,focused,color,style);
    enum StPresence value=user->online?user->presence:ST_OFFLINE;
    if(!strcmp(user->id,c->self_id)&&c->self)value=self_presence(c);
    unsigned bg=selected?UI_SELECTED:UI_SIDE;
    ui_circle(x+19,y+8,11,bg);ui_presence(x+20,y+9,9,value,bg);
}
static void person_row(const StClient *c,const StUser *user,int x,int y,int width,int selected,int focused){person_row_tint(c,user,x,y,width,selected,focused,selected&&focused?UI_ACCENT:UI_WHITE,NULL);}
static StMediaRequest file_key(const StFile *file,const char *tag) {
    StMediaRequest key={0};st_copy(key.tag,sizeof(key.tag),tag);st_copy(key.id,sizeof(key.id),file->id);st_copy(key.filename,sizeof(key.filename),file->filename);key.animated=file->animated;return key;
}
static const char *channel_server(const StClient *c,const char *channel){for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,channel))return c->channels[i].server;return "";}
static unsigned author_colour(const StClient *c,const char *server,const char *id,unsigned fallback) {
    const StRole *role=st_member_role(c,server,id,0);unsigned color;
    return role&&st_role_colour(role->colour,&color)?color:fallback;
}
/* Same measured header in the timeline and full view, including narrow panes.
 * Date stays complete; pronouns move to their own line when necessary. */
static int message_heading(const StClient *c,const StMessage *m,int x,int y,int width,int draw) {
    const char *server=channel_server(c,c->messages_channel),*name=m->system[0]?(!strcmp(m->system,"call_started")?"Call":"System"):st_context_name(c,server,m->author);
    const char *pronouns=m->system[0]?"":st_context_pronouns(c,server,m->author);
    const StRole *icon=m->system[0]?NULL:st_member_role(c,server,m->author,1);
    char date[24]="";if(m->id[0])st_timestamp_label(m->sent_ms,st_local_offset_minutes(),date);
    int date_width=ui_text_width(date),pronouns_width=ui_text_width(pronouns),split=date[0]&&*pronouns&&date_width+pronouns_width+12>width;
    int height=20+((date[0]||*pronouns)?19:0)+(split?19:0);
    if(!draw)return height;
    int left=m->system[0]?0:22,room=width-left-(icon?20:0);
    if(!m->system[0]){ui_circle(x,y,16,UI_SIDE);draw_avatar(c,m->author,x,y,16);}
    const StRole *role=st_member_role(c,server,m->author,0);
    ui_role_text_clip(x+left,y,room,name,role?role->colour:NULL,UI_WHITE);
    if(icon){int used=ui_text_width(name);if(used>room)used=room;StMediaRequest key=file_key(&icon->icon,"icons");ui_image_ref(&key,x+left+used+3,y+1,16,16,0);}
    if(date[0])ui_text_clip(x,y+20,width,date,UI_DIM);
    if(*pronouns)ui_text_clip(x+(date[0]&&!split?date_width+12:0),y+20+(split?19:0),width-(date[0]&&!split?date_width+12:0),pronouns,UI_DIM);
    return height;
}
/* Count and paint with the same dimensions, including wrapped chips. */
static int reaction_rows(const char *text,int x,int y,int width,int draw,int rows) {
    int row=0,cx=0,any=0;
    while(*text) {
        const char *end=strstr(text,"  ");size_t n=end?(size_t)(end-text):strlen(text);
        char label[180];if(n>=sizeof(label))n=sizeof(label)-1;memcpy(label,text,n);label[n]=0;
        char *count=strrchr(label,':');if(count){*count++=0;char formatted[180];snprintf(formatted,sizeof(formatted),st_custom_emoji_id(label)?":%s: %s":"%s %s",label,count);st_copy(label,sizeof(label),formatted);}
        int w=ui_text_width(label)+14;if(w>width)w=width;
        if(cx&&cx+w>width){row++;cx=0;}if(row>=rows)break;
        if(draw){ui_round_rect(x+cx,y+row*24,w,22,6,UI_REACTION);ui_text_clip(x+cx+7,y+row*24+1,w-14,label,UI_WHITE);}
        cx+=w+5;any=1;if(!end)break;text=end+2;
    }
    return any?(row>=rows?rows:row+1)*24:0;
}
static int reaction_chips(const char *text,int x,int y,int width,int draw){return reaction_rows(text,x,y,width,draw,2);}
static const char *quote_text(const StQuote *q){return q->state==1?q->content:q->state==2?"Message unavailable":q->state==3?"Quote could not be loaded":"Loading quote...";}
static const char *quote_name(const StClient *c,const StQuote *q){return q->name[0]?q->name:q->author[0]?st_user_name(c,q->author):"Reply";}
static void quote_compact(const StClient *c,const StMessage *m,int x,int y,int width) {
    const StQuote *q=m->replies;ui_rect(x,y+1,2,18,UI_ACCENT);
    if(!q->state){ui_loader(x+8,y,width-8,"Loading quote...");return;}
    char label[180];if(m->reply_total>1)snprintf(label,sizeof(label),"%.129s (+%d):",quote_name(c,q),m->reply_total-1);
    else snprintf(label,sizeof(label),"%.129s:",quote_name(c,q));
    int name_width=ui_text_width(label),limit=(width-8)/3;if(name_width>limit)name_width=limit;
    ui_text_clip(x+8,y,name_width,label,UI_DIM);
    ui_markdown_inline(x+14+name_width,y,width-14-name_width,quote_text(q),UI_DIM);
}
static int detail_lines(const StMessage *m,int width){return m->reply_count*2+ui_markdown_lines(m->content,width);}
static void detail_body(const StClient *c,const StMessage *m,int x,int y,int width,int skip,int visible) {
    for(int row=0;row<visible;row++) {
        int line=skip+row;
        if(line<m->reply_count*2) {
            const StQuote *q=m->replies+line/2;ui_rect(x,y+row*19+1,2,17,UI_ACCENT);
            if(!(line%2)){char name[180];snprintf(name,sizeof(name),"Reply to %s",quote_name(c,q));ui_text_clip(x+8,y+row*19,width-8,name,UI_ACCENT);}
            else if(!q->state)ui_loader(x+8,y+row*19,width-8,"Loading quote...");
            else ui_markdown_area(x+8,y+row*19,width-8,quote_text(q),0,1,UI_DIM);
        } else {ui_markdown_area(x,y+row*19,width,m->content,line-m->reply_count*2,visible-row,m->system[0]?UI_DIM:UI_WHITE);break;}
    }
}
/* Previews use server-supplied metadata only. No page fetch, script, browser
 * navigation or automatic download of embedded remote media. */
static int embed_card(const StEmbed *e,int x,int y,int width,int draw,int full,int skip) {
    int title=ui_markdown_lines(e->title,width-18),description=e->description[0]?ui_markdown_lines(e->description,width-18):0;
    if(!full){if(title>2)title=2;if(description>2)description=2;}
    if(title<1)title=1;
    int lines=title+description+(e->site[0]?1:0)+(full&&e->url[0]?ui_line_count(e->url,width-20):0);
    int height=lines*19+12;
    if(draw){
        ui_round_rect(x,y,width,height,5,UI_SIDE);ui_rect(x,y+4,3,height-8,e->colour?e->colour:UI_ACCENT);
        int at=y+5;ui_markdown_area(x+9,at,width-18,e->title,skip,title,UI_ACCENT);at+=title*19;
        if(e->site[0]){ui_text_clip(x+9,at,width-18,e->site,UI_DIM);at+=19;}
        ui_markdown_area(x+9,at,width-18,e->description,0,description,UI_WHITE);at+=description*19;
        if(full&&e->url[0])ui_wrap_area(x+9,at,width-18,e->url,0,ui_line_count(e->url,width-20),UI_DIM);
    }
    return height;
}
static int embed_cards(const StMessage *m,int x,int y,int width,int draw) {
    int height=0;for(int i=0;i<m->preview_count;i++)height+=embed_card(m->previews+i,x,y+height,width,draw,0,0)+5;return height;
}
static int post_height(const StClient *c,const StMessage *m,int width) {
    return message_heading(c,m,0,0,width-20,0)+(m->reply_count?24:0)+ui_markdown_lines(m->content,width-20)*19+reaction_rows(m->reactions,0,0,width-20,0,1)+(m->attachments?24:0)+embed_cards(m,0,0,width-20,0)+13;
}
static int chat_anchor(const StPanels *p,const StClient *c,int width,int *total) {
    int selected=clamp(p->cursor,(int)c->message_count),at=0,end=0;
    for(size_t i=0;i<c->message_count;i++){at+=post_height(c,c->messages+c->message_count-1-i,width);if((int)i==selected)end=at;}
    *total=at;int height=p->typing[0]?190:210;
    int offset=p->chat_manual?p->chat_offset:end-height;
    if(offset<0)offset=0;
    if(offset>at-height)offset=at-height;
    if(offset<0)offset=0;
    return offset;
}
int st_panels_chat_position(const StPanels *p,const StClient *c,int message){int at=0,width=st_panels_widths(p).content;for(int i=0;i<message&&i<(int)c->message_count;i++)at+=post_height(c,c->messages+c->message_count-1-(size_t)i,width);return at;}
int st_panels_chat_bottom(const StPanels *p,const StClient *c){int total,offset=chat_anchor(p,c,st_panels_widths(p).content,&total);return offset+(p->typing[0]?190:210)>=total-1;}
void st_panels_scroll_chat(StPanels *p,const StClient *c,int analog,uint64_t now) {
    uint64_t dt=p->chat_scroll_at&&now>=p->chat_scroll_at?now-p->chat_scroll_at:16;p->chat_scroll_at=now;if(dt>50)dt=50;
    if(p->page!=ST_CHAT||p->focus!=ST_CONTENT||!c->message_count||abs(analog)<32)return;
    int width=st_panels_widths(p).content,total,offset=chat_anchor(p,c,width,&total),height=p->typing[0]?190:210;
    int delta=(int)((abs(analog)-31)*dt/150);if(!delta)delta=1;
    offset+=analog>0?delta:-delta;int max=total-height;if(max<0)max=0;if(offset<0)offset=0;if(offset>max)offset=max;
    p->chat_offset=offset;p->chat_manual=1;int middle=offset+height/2,at=0;
    for(size_t i=0;i<c->message_count;i++){at+=post_height(c,c->messages+c->message_count-1-i,width);if(at>middle){p->cursor=(int)i;break;}}
}
static void chat(const StPanels *p,const StClient *c,int x,int width) {
    if(!c->message_count){if(p->typing[0])ui_text_clip(x+8,226,width-16,p->typing,UI_ACCENT);if(p->loading)ui_loader(x+8,42,width-16,"Messages...");else if(!p->error[0])ui_wrap_area(x+8,40,width-16,"No messages yet.",0,8,UI_DIM);return;}
    int total,offset=chat_anchor(p,c,width,&total),y=34-offset,bottom=p->typing[0]?224:244;
    ui_clip(x+4,32,x+width-4,bottom);
    for(int i=0;i<(int)c->message_count&&y<bottom;i++) {
        const StMessage *m=c->messages+c->message_count-1-(size_t)i;int height=post_height(c,m,width);
        if(y+height<=32){y+=height;continue;}
        if(i==p->cursor)ui_rect(x+4,y-2,width-8,height-4,UI_SELECTED);
        int head=message_heading(c,m,x+10,y,width-20,1),quote_height=m->reply_count?24:0;
        if(quote_height)quote_compact(c,m,x+10,y+head,width-20);
        int lines=ui_markdown_lines(m->content,width-20);
        ui_markdown_area(x+10,y+head+quote_height,width-20,m->content,0,lines,m->system[0]?UI_DIM:UI_WHITE);
        int meta_y=y+head+quote_height+lines*19;meta_y+=reaction_rows(m->reactions,x+10,meta_y,width-20,1,1);
        if(m->attachments){char info[80];snprintf(info,sizeof(info),"%d attachments - open message",m->attachments);ui_round_rect(x+10,meta_y,width-20,22,6,UI_SIDE);ui_text_clip(x+16,meta_y+1,width-32,info,UI_DIM);meta_y+=24;}
        embed_cards(m,x+10,meta_y,width-20,1);
        y+=height;
    }
    ui_clip_reset();
    if(p->typing[0])ui_text_clip(x+8,226,width-16,p->typing,UI_ACCENT);
}
static void auth(const StPanels *p,const StClient *c,const char *confirm,const char *cancel) {
    int signup=p->page==ST_REGISTER,onboard=p->page==ST_ONBOARD;
    ui_rect(0,0,480,272,UI_BG);ui_rect(34,10,412,232,UI_SIDE);
    int heading_x=54+ui_text_width(ST_APP_NAME)+18;
    ui_text(54,17,ST_APP_NAME,UI_ACCENT);
    ui_text_clip(heading_x,17,426-heading_x,onboard?"Choose your username":signup?"Create account":"Welcome back!",UI_WHITE);
    int count=st_panels_count(p,c,ST_CONTENT);
    for(int i=0;i<count;i++) {
        char label[600];const char *text="";
        if(onboard){if(!i){snprintf(label,sizeof(label),"Name: %s",p->username[0]?p->username:"choose");text=label;}else text="Continue";}
        else if(i==0){snprintf(label,sizeof(label),"Email: %s",p->email[0]?p->email:"enter your email");text=label;}
        else if(i==1){text=p->password[0]?"Password: ********":"Password: enter your password";}
        else if(signup&&c->invite_only&&i==2){snprintf(label,sizeof(label),"Invite: %s",p->invite[0]?p->invite:"enter code");text=label;}
        else {int action=i-(signup&&c->invite_only?1:0);
            if(action==2)text=onboard?"Continue":signup?"Create account":"Log in";
            if(action==3)text=signup?"Have an account? Log in":"Need an account? Sign up";
            if(action==4)text="Close client";
        }
        int action=i-(signup&&c->invite_only?1:0);enum UiIcon icon=onboard?(!i?UI_ICON_USER:UI_ICON_LOGIN):i==0?UI_ICON_EMAIL:i==1?UI_ICON_KEY:signup&&c->invite_only&&i==2?UI_ICON_INVITE:action==4?UI_ICON_LOGOUT:action==3?UI_ICON_ADD_USER:UI_ICON_LOGIN;
        menu_row(48,53+i*26,384,text,icon,i==p->cursor,1);
    }
    if(p->loading)ui_loader(54,211,370,"Connecting...");
    else if(p->error[0])ui_wrap_area(54,191,370,p->error,0,2,UI_DIM);
    else if(signup&&c->captcha)ui_wrap_area(54,190,370,"This server requires hCaptcha. Register in the web client.",0,2,UI_DIM);
    else if(!c->discovered)ui_text_clip(54,211,370,"Connect to your server to continue.",UI_DIM);
    else ui_text_clip(54,211,370,c->config.api_url,UI_DIM);
    char hint[120];snprintf(hint,sizeof(hint),"%s select  %s back  " UI_KEY_SQUARE " retry",confirm,cancel);ui_footer(p->network_notice[0]?p->network_notice:hint);
}
static const char *mutual_name(const StClient *c,const char *id,int server) {
    if(server){for(size_t i=0;i<c->server_count;i++)if(!strcmp(c->servers[i].id,id))return c->servers[i].name;}
    else {for(size_t i=0;i<c->channel_count;i++)if(!strcmp(c->channels[i].id,id))return st_channel_name(c,c->channels+i);}
    return id;
}
static void profile_card(const StPanels *p,const StClient *c,int x,int width,const char *confirm) {
    if(strcmp(p->person,c->profile_id)||strcmp(p->profile_server,c->profile_server)){if(p->loading)ui_loader(x+8,42,width-16,"Profile...");return;}
    const cJSON *user=c->profile_user,*status=cJSON_GetObjectItemCaseSensitive(user,"status");
    StMediaRequest background,face;st_profile_image_ref(cJSON_GetObjectItemCaseSensitive(c->profile,"background"),"backgrounds",&background);st_profile_image_ref(cJSON_GetObjectItemCaseSensitive(user,"avatar"),"avatars",&face);
    default_avatar(&face,p->person);
    ui_round_rect(x+8,32,width-16,64,6,UI_SIDE);ui_image_ref(&background,x+8,32,width-16,64,2);
    ui_circle(x+12,72,40,UI_BG);if(!ui_image_ref(&face,x+16,76,32,32,1)){char initials[9];badge(st_str(user,"username"),initials);ui_text_clip(x+20,82,26,initials,UI_DIM);}
    const char *name=st_str(user,"display_name");if(!*name)name=st_str(user,"username");
    const StMember *member=st_member(c,c->profile_server,p->person);if(member&&member->nickname[0])name=member->nickname;
    const StRole *name_role=st_member_role(c,c->profile_server,p->person,0);
    ui_role_text_clip(x+58,81,width-66,name,name_role?name_role->colour:NULL,UI_WHITE);
    char handle[300];snprintf(handle,sizeof(handle),"%s#%s",st_str(user,"username"),st_str(user,"discriminator"));ui_text_clip(x+8,110,width-16,handle,UI_DIM);
    enum StPresence presence=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(user,"online"))||!strcmp(p->person,c->self_id)?st_presence_value(st_str(status,"presence")):ST_OFFLINE;
    presence_row(x+8,130,width-16,presence,UI_BG);
    char text[9000];profile_text(c,text,sizeof(text));int text_lines=ui_line_count(text,width-16);
    const StRole *roles[256];int role_count=st_member_roles(c,c->profile_server,p->person,roles);
    for(int row=0;row<4;row++) {
        int line=p->scroll+row,y=151+row*19;
        if(line<text_lines)ui_wrap_area(x+8,y,width-16,text,line,1,UI_WHITE);
        else if(c->profile_server[0]) {
            int r=line-text_lines-1;
            if(r==-1){char title[40];snprintf(title,sizeof(title),"Roles (%d)",role_count);ui_text_clip(x+8,y,width-16,title,UI_DIM);}
            else if(r<role_count) {
                const StRole *role=roles[r];unsigned color=UI_DIM;st_role_colour(role->colour,&color);
                ui_round_rect(x+8,y,width-16,18,5,UI_SIDE);ui_circle(x+13,y+5,8,color);int left=26;
                if(role->icon.id[0]){StMediaRequest key=file_key(&role->icon,"icons");ui_image_ref(&key,x+26,y+1,16,16,0);left+=19;}
                ui_text_clip(x+left,y,width-left-12,role->name,UI_WHITE);
            }else if(!role_count&&r==0)ui_text_clip(x+8,y,width-16,member?"No roles":"Roles unavailable",UI_DIM);
        }
    }
    (void)confirm;
}
static int picker_layout(int position,int columns,int *column,int *header) {
    int y=0,in_group=0,last=-1;
    for(int i=0;i<=position;i++) {
        int group=st_reactions_group_id(i);*header=group!=last;
        if(*header){if(in_group)y+=32;y+=23;in_group=0;last=group;}
        *column=in_group%columns;
        if(i==position)return y;
        in_group++;if(in_group%columns==0){y+=32;in_group=0;}
    }
    return y;
}
static void member_list(const StPanels *p,const StClient *c,int x,int n) {
    if(!n)return;
    const char *server=channel_server(c,p->channel);int grouped=*server!=0;
    int selected=clamp(p->member_cursor,n),start=selected,used=23+(grouped?19:0);
    while(start>0) {
        int add=23+(grouped&&c->participants[start-1].member_group!=c->participants[start].member_group?19:0);
        if(used+add>184)break;
        used+=add;start--;
    }
    int y=34;
    for(int i=start;i<n;i++) {
        const StUser *u=c->participants+i;
        if(grouped&&(i==start||u->member_group!=c->participants[i-1].member_group)) {
            if(y+42>218)break;
            const StRole *role=u->member_group>0&&(size_t)u->member_group<=c->role_count?c->roles+u->member_group-1:NULL;
            const char *label=role?role->name:u->member_group==-1?"Online":"Offline";
            int total=0;for(int k=0;k<n;k++)if(c->participants[k].member_group==u->member_group)total++;
            char count[16];snprintf(count,sizeof(count),"%d",total);int tw=ui_text_width(count),left=8;
            unsigned color=UI_DIM;if(role)st_role_colour(role->colour,&color);
            if(role&&role->icon.id[0]){StMediaRequest key=file_key(&role->icon,"icons");ui_image_ref(&key,x+6,y+1,14,14,0);left=24;}
            ui_role_text_clip(x+left,y,120-left-tw-12,label,role?role->colour:NULL,color);ui_text_clip(x+112-tw,y,tw,count,UI_DIM);y+=19;
        }
        if(y+23>218)break;
        unsigned fallback=i==selected&&p->focus==ST_MEMBERS?UI_ACCENT:UI_WHITE;
        const StRole *role=st_member_role(c,server,u->id,0);
        person_row_tint(c,u,x,y,120,i==selected,p->focus==ST_MEMBERS,author_colour(c,server,u->id,fallback),role?role->colour:NULL);
        y+=23;
    }
}
static void emoji_picker(const StPanels *p,const StClient *c,int x,int width) {
    st_reactions_bind(c);int cols=st_panels_emoji_columns(p),column,head;
    int selected_y=picker_layout(p->cursor,cols,&column,&head),offset=selected_y>140?selected_y-100:0;
    const StMessage *m=c->message_count?c->messages+c->message_count-1-(size_t)clamp(p->message,(int)c->message_count):NULL;
    /* Each server is a separate section, including when several fit on screen. */
    int y=0,in_group=0,last=-1,group_top=0,first_visible=1;
    for(int i=0;i<st_reactions_count();i++) {
        int group=st_reactions_group_id(i);
        if(group!=last){if(in_group)y+=32;group_top=32+y-offset;int hy=group_top;if(hy>=32&&hy<191)ui_text_clip(x+8,hy,width-16,st_reactions_group(i),UI_DIM);y+=23;in_group=0;last=group;}
        int ey=32+y-offset,ex=x+8+(in_group%cols)*32;
        if(ey>=52&&ey+28<=207) {
            if(first_visible&&group_top<32)ui_text_clip(x+8,32,width-16,st_reactions_group(i),UI_DIM);
            first_visible=0;
            const char *key=st_reactions_key(st_reactions_at(i));const StReaction *r=m?st_message_reaction_key(m,key):NULL;
            if(i==p->cursor)ui_round_rect(ex-2,ey-2,30,30,5,UI_ACCENT);
            ui_round_rect(ex,ey,26,26,4,r&&r->mine?UI_REACTION:i==p->cursor?UI_SELECTED:UI_BG);
            char text[140];snprintf(text,sizeof(text),st_custom_emoji_id(key)?":%s:":"%s",key);ui_text_clip(ex+4,ey+3,22,text,UI_WHITE);
            if(r&&r->mine)ui_circle(ex+20,ey+20,4,UI_ACCENT);
        }
        in_group++;if(in_group%cols==0){y+=32;in_group=0;}
        if(ey>207)break;
    }
    const char *key=st_reactions_key(st_reactions_at(p->cursor));const StReaction *r=m?st_message_reaction_key(m,key):NULL;
    char info[120];snprintf(info,sizeof(info),"%d / %d   %s",p->cursor+1,st_reactions_count(),r&&r->mine?"Remove my reaction":"Add reaction");ui_text_clip(x+8,212,width-16,info,UI_DIM);

}
static void unread_badge(const StNotices *n,const StClient *c,const char *id,int aggregate,int right,int y,unsigned background) {
    int unknown=0;unsigned count=aggregate?st_notices_total(n,c,id,&unknown):st_notices_channel(n,id,&unknown);
    if(!count&&!unknown)return;
    if(!count){ui_circle(right-9,y+4,8,background);ui_circle(right-8,y+5,6,UI_ACCENT);return;}
    char text[16];if(count>99)st_copy(text,sizeof(text),"99+");else snprintf(text,sizeof(text),"%u",count);
    int width=ui_text_width(text)+8;if(width<18)width=18;
    ui_round_rect(right-width-1,y-1,width+2,20,9,background);ui_round_rect(right-width,y,width,18,8,UI_ACCENT);
    ui_text_clip(right-width+(width-ui_text_width(text))/2,y-1,width-4,text,UI_BG);
}
static void unread_avatar_badge(const StNotices *n,const char *id,int right,int y,unsigned background) {
    int dot=0;unsigned count=st_notices_channel(n,id,&dot);if(!count&&!dot)return;
    if(!count){ui_circle(right-7,y,7,background);ui_circle(right-6,y+1,5,UI_ACCENT);return;}
    static const char *digits[]={"111101101101111","010110010010111","111001111100111","111001111001111","101101111001001","111100111001111","111100111101111","111001001001001","111101111101111","111101111001111","000010111010000"};
    char text[8];if(count>99)st_copy(text,sizeof(text),"99+");else snprintf(text,sizeof(text),"%u",count);
    int length=(int)strlen(text),width=length*4+5;ui_round_rect(right-width-1,y-1,width+2,11,5,background);ui_round_rect(right-width,y,width,9,4,UI_ACCENT);
    for(int i=0;i<length;i++)for(int row=0;row<5;row++)for(int col=0;col<3;col++)if(digits[text[i]=='+'?10:text[i]-'0'][row*3+col]=='1')ui_rect(right-width+3+i*4+col,y+2+row,1,1,UI_BG);
}
static void photo_grid(const StPanels *p,const char *confirm,const char *cancel){
    const StPhotos *photos=p->photos;ui_rect(0,0,480,272,UI_BG);ui_title("Choose a photo");
    ui_text_clip(10,23,460,photos?(photos->sources?"Photo folders":photos->path):"Photo folders",UI_DIM);
    if(!photos)return;
    int count=(int)photos->count+(photos->sources?0:1),selected=clamp(p->cursor,count),first=selected/ST_PHOTO_PAGE*ST_PHOTO_PAGE;
    for(int i=first;i<count&&i<first+ST_PHOTO_PAGE;i++){
        int x=10+(i-first)%ST_PHOTO_COLUMNS*156,y=46+(i-first)/ST_PHOTO_COLUMNS*94,back=!photos->sources&&i==0;
        const StPhotoEntry *entry=back?NULL:photos->items+i-(photos->sources?0:1);int folder=back||entry->directory;
        const char *label=back?(!strcmp(photos->root,photos->path)?"Photo folders":".. (parent)"):entry->name;
        if(i==selected)ui_round_rect(x-2,y-2,152,94,6,UI_ACCENT);
        ui_round_rect(x,y,148,90,5,i==selected?UI_SELECTED:UI_SIDE);
        if(folder)ui_icon_size(UI_ICON_FOLDER,x+50,y+10,48,i==selected?UI_ACCENT:UI_DIM);
        else if(!entry->key[0]||!ui_image("photos",entry->key,x+4,y+3,ST_PHOTO_WIDTH,ST_PHOTO_HEIGHT,0)){
            ui_icon_size(UI_ICON_IMAGE,x+58,y+13,32,UI_DIM);
            if(entry->key[0]&&ui_image_state("photos",entry->key)!=4)ui_loader(x+61,y+46,32,"");
            else ui_text_clip(x+7,y+47,134,"No preview",UI_DIM);
        }
        ui_text_clip(x+5,y+71,138,label,i==selected?UI_ACCENT:UI_WHITE);
    }
    char info[400];const StPhotoEntry *entry=selected>=(photos->sources?0:1)&&count?photos->items+selected-(photos->sources?0:1):NULL;
    if(entry&&!entry->directory)snprintf(info,sizeof(info),"%s  |  %.2f MB",entry->name,(double)entry->bytes/1048576.);
    else snprintf(info,sizeof(info),"%d / %d%s",selected+1,count,photos->truncated?"  (first 256 entries)":"");
    if(p->error[0])st_copy(info,sizeof(info),p->error);else if(p->loading)st_copy(info,sizeof(info),"Sending photo...");
    ui_text_clip(10,232,460,info,p->error[0]?UI_BUSY:UI_DIM);ui_rect(0,250,480,1,UI_LINE);
    char footer[160];snprintf(footer,sizeof(footer),"%s choose  %s back  " UI_KEY_DPAD " move  " UI_KEY_START "/" UI_KEY_SELECT " pages",confirm,cancel);ui_footer(footer);
}
static void image_fullscreen(const StPanels *p,const StClient *c,const char *cancel){
    ui_rect(0,0,480,272,UI_BG);
    const StMessage *m=p->message>=0&&(size_t)p->message<c->message_count&&!strcmp(p->channel,c->messages_channel)?c->messages+c->message_count-1-(size_t)p->message:NULL;
    const StAttachment *a=m&&p->cursor>=0&&p->cursor<m->attachments&&p->cursor<ST_ATTACHMENT_MAX?m->files+p->cursor:NULL;
    if(a&&a->image&&!strcmp(a->id,p->media_requested)){
        StMediaRequest key={.tag="attachments",.animated=a->animated};st_copy(key.id,sizeof(key.id),a->id);if(a->animated)st_copy(key.filename,sizeof(key.filename),a->name);
        if(p->media_error[0])ui_wrap_area(24,106,432,p->media_error,0,5,UI_DIM);
        else if(!ui_image_ref(&key,0,0,480,250,0))ui_loader(24,116,432,"Loading image...");
    }else ui_text(24,116,"Image is no longer available.",UI_DIM);
    ui_rect(0,250,480,1,UI_LINE);char footer[200];snprintf(footer,sizeof(footer),"%s back   %s",cancel,a?a->name:"");ui_footer(footer);
}
static void navigation(const StPanels *p,const StClient *c,int width,const char *side){
    if(width<=0)return;
    StPanelWidths w={.rail=44+width-144,.nav=144};
    ui_clip(44,0,44+width,250);
    int nav_y=34;
    if(!p->server[0]&&c->token[0]) {
        account_heading(c,w.rail,w.nav,p->focus==ST_NAV?UI_ACCENT:UI_WHITE);
        presence_row(w.rail+8,25,w.nav-16,self_presence(c),UI_SIDE);
        ui_text_clip(w.rail+8,44,w.nav-16,st_str(cJSON_GetObjectItemCaseSensitive(c->self,"status"),"text"),UI_DIM);
        ui_rect(w.rail,65,w.nav,p->focus==ST_NAV?2:1,p->focus==ST_NAV?UI_ACCENT:UI_LINE);nav_y=75;
    } else {
        const StServer *server=NULL;for(size_t i=0;i<c->server_count;i++)if(!strcmp(c->servers[i].id,p->server))server=c->servers+i;
        if(server&&server->banner.id[0]) {
            StMediaRequest key=file_key(&server->banner,"banners");ui_rect(w.rail,0,w.nav,64,UI_RAIL);
            if(!ui_image_ref(&key,w.rail,0,w.nav,64,2))ui_icon(UI_ICON_IMAGE,w.rail+w.nav/2-8,24,UI_DIM);
            ui_text_clip(w.rail+7,67,w.nav-14,side,p->focus==ST_NAV?UI_ACCENT:UI_WHITE);
            ui_rect(w.rail,89,w.nav,p->focus==ST_NAV?2:1,p->focus==ST_NAV?UI_ACCENT:UI_LINE);nav_y=98;
        }else header(w.rail,w.nav,side,p->focus==ST_NAV);
    }
    int count=st_panels_count(p,c,ST_NAV),nav_rows=(222-nav_y)/23,nav_first=p->nav_cursor>=nav_rows?p->nav_cursor-nav_rows+1:0;
    for(int i=nav_first;i<count&&i<nav_first+nav_rows;i++) {
        char name[320];const char *label;
        if(!p->server[0])label=tabs[i];
        else {
            int idx=st_panels_channel(p,c,i);const StChannel *ch=c->channels+idx;
            snprintf(name,sizeof(name),"%s",st_channel_name(c,ch));label=name;
        }
        int selected=p->focus==ST_NAV?i==p->nav_cursor:(!p->server[0]?i==p->nav_tab:
                     !strcmp(p->channel,c->channels[st_panels_channel(p,c,i)].id));
        static const enum UiIcon tab_icons[]={UI_ICON_MESSAGE,UI_ICON_PEOPLE,UI_ICON_USER};
        const StChannel *nav_channel=p->server[0]?c->channels+st_panels_channel(p,c,i):NULL;
        int voiced=nav_channel&&nav_channel->voice,y=nav_y+(i-nav_first)*23;
        int unread_dot=0;unsigned unread=nav_channel?st_notices_channel(p->notices,nav_channel->id,&unread_dot):0;
        menu_row(w.rail,y,w.nav,voiced||unread||unread_dot?"":label,p->server[0]?(voiced?UI_ICON_VOICE:UI_ICON_HASH):tab_icons[i],selected,p->focus==ST_NAV);
        if(voiced) {
            char total[16];snprintf(total,sizeof(total),"%d",st_voice_count(c,nav_channel->id));int tw=ui_text_width(total);
            unsigned color=selected&&p->focus==ST_NAV?UI_ACCENT:UI_WHITE;
            ui_text_clip(w.rail+32,y,w.nav-48-tw,label,color);ui_text_clip(w.rail+w.nav-8-tw,y,tw,total,UI_DIM);if(unread||unread_dot)ui_circle(w.rail+25,y+12,5,UI_ACCENT);
        }
        if(nav_channel&&!voiced&&(unread||unread_dot)) {
            ui_text_clip(w.rail+32,y,w.nav-75,label,selected&&p->focus==ST_NAV?UI_ACCENT:UI_WHITE);
            unread_badge(p->notices,c,nav_channel->id,0,w.rail+w.nav-5,y+1,selected?UI_SELECTED:UI_SIDE);
        }else if(!p->server[0]&&i==0)unread_badge(p->notices,c,"",1,w.rail+w.nav-5,y+1,selected?UI_SELECTED:UI_SIDE);
    }
    ui_clip_reset();
}
void st_panels_draw(const StPanels *p,const StClient *c,const char *confirm,const char *cancel) {
    const StChannel *context=st_panels_current(p,c);ui_mentions(c,context?context->server:"");
    st_reactions_bind(c);
    if(p->page==ST_IMAGE){image_fullscreen(p,c,cancel);return;}
    if(p->page==ST_PHOTOS){photo_grid(p,confirm,cancel);return;}
    if(p->page==ST_LOGIN||p->page==ST_REGISTER||p->page==ST_ONBOARD){StThemeColors old=st_theme;st_theme=(StThemeColors)ST_THEME_DEFAULT;auth(p,c,confirm,cancel);st_theme=old;return;}
    static const char *settings[]={"Static cache","Appearance","Sessions","Microphone loopback"};
    static const char *account[]={"My profile","My status","Settings","Log out"};
    static const char *presence[]={"Status text","Online","Idle","Focus","Do not disturb","Invisible","Clear status"};
    StPanelWidths w=st_panels_widths(p);int x=w.rail+w.nav;
    ui_rect(0,0,480,272,UI_BG);ui_rect(0,0,w.rail,250,UI_RAIL);ui_rect(w.rail,0,w.nav,250,UI_SIDE);
    ui_rect(w.rail-1,0,1,250,UI_LINE);ui_rect(x-1,0,1,250,UI_LINE);
    ui_circle(6,4,32,!p->server[0]?UI_SELECTED:UI_SIDE);
    if(!draw_avatar(c,c->self_id,6,4,32))ui_text_clip(13,10,22,"Me",p->rail_cursor==0&&p->focus==ST_RAIL?UI_ACCENT:UI_WHITE);
    unread_badge(p->notices,c,"",1,42,1,UI_RAIL);
    if(p->focus==ST_RAIL && p->rail_cursor==0)ui_rect(1,5,3,30,UI_ACCENT);
    int first=p->rail_cursor>6?p->rail_cursor-6:0;
    for(int i=first;i<(int)c->server_count && i<first+6;i++) {
        int y=44+(i-first)*34;char name[9];badge(c->servers[i].name,name);
        int active=!strcmp(p->server,c->servers[i].id),selected=p->rail_cursor==i+1;
        ui_circle(6,y,32,active||selected?UI_SELECTED:UI_SIDE);
        if(!ui_image("icons",c->servers[i].icon,6,y,32,32,1))ui_text_clip(10,y+4,26,name,selected&&p->focus==ST_RAIL?UI_ACCENT:UI_WHITE);
        unread_badge(p->notices,c,c->servers[i].id,1,42,y+14,UI_RAIL);
        if(selected&&p->focus==ST_RAIL)ui_rect(1,y,3,28,UI_ACCENT);
    }
    char identity[300];account_label(c,identity,sizeof(identity));
    const char *side=c->token[0]?identity:"My account";
    for(size_t i=0;i<c->server_count;i++)if(!strcmp(p->server,c->servers[i].id))side=c->servers[i].name;
    navigation(p,c,w.nav,side);ui_rect(x-1,0,1,250,UI_LINE);
    if(c->token[0]){ui_circle(26,24,13,UI_RAIL);ui_presence(28,26,9,self_presence(c),UI_RAIL);}
    const StChannel *channel=st_panels_current(p,c);
    const char *title=p->page==ST_CHAT_ACTIONS?"Chat actions":p->page==ST_NOTIFICATIONS?"Notifications":p->page==ST_PHOTOS?"Send a photo":p->page==ST_DMS?tabs[0]:p->page==ST_FRIENDS?tabs[1]:p->page==ST_SETTINGS?"Settings":p->page==ST_APPEARANCE?"Appearance":p->page==ST_ATTACHMENTS?"Media and previews":
                      p->page==ST_ADD_FRIEND?"Add friend":p->page==ST_REQUESTS?"Friend requests":p->page==ST_REQUEST_ACTIONS?st_user_name(c,p->person):p->page==ST_MESSAGE_ACTIONS?"Message actions":p->page==ST_INVITE_LINKS?"Invitations":p->page==ST_INVITE?"Server invitation":p->page==ST_SERVER_ACTIONS?side:p->page==ST_ACCOUNT?identity:p->page==ST_CACHE?"Static cache":p->page==ST_STATUS?"My status":p->page==ST_PROFILE?"Profile":p->page==ST_REACTIONS?"Reactions":p->page==ST_PROFILE_ACTIONS?"Profile actions":p->page==ST_MUTUAL_SERVERS?"Mutual servers":p->page==ST_MUTUAL_CHATS?"Mutual conversations":p->page==ST_REPORT?"Report user":p->page==ST_SESSIONS?"Sessions":channel?st_channel_name(c,channel):"Select a channel";
    header(x,w.content,title,p->focus==ST_CONTENT);
    if(p->page==ST_ACCOUNT){ui_rect(x,0,w.content,25,UI_BG);account_heading(c,x,w.content,p->focus==ST_CONTENT?UI_ACCENT:UI_WHITE);}
    int count=st_panels_count(p,c,ST_CONTENT);
    int first_row=p->cursor>=8?p->cursor-7:0;
    if(p->page==ST_INVITE) {
        if(c->invitation&&!strcmp(c->invitation_code,p->invite_code)) {
            char icon[64],info[100];st_image_ref(cJSON_GetObjectItemCaseSensitive(c->invitation,"server_icon"),"icons",icon);
            ui_circle(x+8,34,40,UI_SIDE);ui_image("icons",icon,x+8,34,40,40,1);ui_text_clip(x+58,35,w.content-66,st_str(c->invitation,"server_name"),UI_WHITE);
            const cJSON *members=cJSON_GetObjectItemCaseSensitive(c->invitation,"member_count");snprintf(info,sizeof(info),"%.0f members",cJSON_IsNumber(members)?members->valuedouble:0.0);ui_text_clip(x+58,56,w.content-66,info,UI_DIM);
            ui_text_clip(x+8,90,w.content-16,st_str(c->invitation,"channel_name"),UI_DIM);
            ui_wrap_area(x+8,115,w.content-16,st_str(c->invitation,"channel_description"),0,3,UI_DIM);
            int joined=0;for(size_t i=0;i<c->server_count;i++)if(!strcmp(c->servers[i].id,st_str(c->invitation,"server_id")))joined=1;
            menu_row(x,188,w.content,joined?"Open server":"Join server",UI_ICON_INVITE,1,p->focus==ST_CONTENT);
        }
    } else if(p->page==ST_CACHE) {
        char info[240];snprintf(info,sizeof(info),"On card: %.2f MB  |  %zu files",(double)p->cache_bytes/1048576.,p->cache_files);
        ui_wrap_area(x+8,34,w.content-16,info,0,2,UI_WHITE);
        ui_icon(UI_ICON_DEVICES,x+8,88,UI_DIM);
        ui_wrap_area(x+8,114,w.content-16,"To reset, close the app and delete its Cache and session entry in XMB > Saved Data Utility. This also removes settings and the saved login.",0,6,UI_DIM);
    } else if(p->page==ST_APPEARANCE) {
        char mode[64];snprintf(mode,sizeof(mode),"Theme: %s",st_theme_dark?"Dark":"Light");menu_row(x,34,w.content,mode,st_theme_dark?UI_ICON_MOON:UI_ICON_SUN,p->cursor==0,p->focus==ST_CONTENT);
        ui_text_clip(x+8,59,w.content-16,"Colours",UI_DIM);
        for(int i=0;i<ST_THEME_COLORS;i++) {
            char label[64];snprintf(label,sizeof(label),"%s%s",st_theme_accent==i?"[x] ":"[ ] ",st_theme_names[i]);
            menu_row(x,80+i*22,w.content,label,UI_ICON_COLOUR,p->cursor==i+1,p->focus==ST_CONTENT);ui_icon(UI_ICON_COLOUR,x+9,82+i*22,st_theme_seeds[i]);
        }
    } else if(p->page==ST_ATTACHMENTS&&c->message_count) {
        const StMessage *m=c->messages+c->message_count-1-(size_t)clamp(p->message,(int)c->message_count);
        int selected=clamp(p->cursor,count),files=m->attachments>ST_ATTACHMENT_MAX?ST_ATTACHMENT_MAX:m->attachments;
        if(selected>=files&&selected-files<m->preview_count){
            const StEmbed *e=m->previews+selected-files;
            ui_clip(x+4,32,x+w.content-4,218);embed_card(e,x+8,34-p->scroll*19,w.content-16,1,1,0);ui_clip_reset();
            char info[64];snprintf(info,sizeof(info),"Preview %d / %d",selected-files+1,m->preview_count);ui_text_clip(x+8,228,w.content-16,info,UI_DIM);
        } else {
            int start=selected>=4?selected-3:0;
            for(int i=start;i<files&&i<start+4;i++){char label[180];const StAttachment *a=m->files+i;snprintf(label,sizeof(label),"%s%s: %s",a->animated?"Animated ":"",a->type[0]?a->type:"File",a->name[0]?a->name:"Unnamed file");menu_row(x,34+(i-start)*23,w.content,label,a->image?UI_ICON_IMAGE:UI_ICON_FILE,i==selected,p->focus==ST_CONTENT);}
            if(files){
                const StAttachment *a=m->files+selected;
                if(a->image&&!strcmp(a->id,p->media_requested)&&!p->media_error[0]){
                    ui_round_rect(x+8,132,w.content-16,88,6,UI_SIDE);
                    StMediaRequest key={.tag="attachments",.animated=a->animated};st_copy(key.id,sizeof(key.id),a->id);if(a->animated)st_copy(key.filename,sizeof(key.filename),a->name);
                    if(!ui_image_ref(&key,x+8,132,w.content-16,88,0))ui_loader(x+16,164,w.content-32,"Loading image...");
                }else ui_wrap_area(x+8,143,w.content-16,p->media_error[0]?p->media_error:a->image?"Image is hidden. Select to load.":"Video and audio playback unavailable.",0,4,UI_DIM);
            }
        }
    } else if(p->page==ST_VOICE) {
        int n=st_voice_count(c,p->channel),start=p->cursor>=5?p->cursor-4:0;char label[160];
        snprintf(label,sizeof(label),"Voice: %d connected",n);ui_icon(UI_ICON_VOICE,x+8,33,UI_ACCENT);ui_text_clip(x+31,32,w.content-39,label,UI_DIM);
        for(int i=start;i<n&&i<start+5;i++) {
            const StVoiceMember *m=st_voice_member(c,p->channel,i);const StUser *u=user_by_id(c,m->user);
            StUser fallback={0};if(!u){st_copy(fallback.id,sizeof(fallback.id),m->user);st_copy(fallback.name,sizeof(fallback.name),m->user);fallback.online=1;u=&fallback;}
            int y=61+(i-start)*25;person_row(c,u,x,y,w.content,i==p->cursor,p->focus==ST_CONTENT);
            ui_rect(x+w.content-42,y,36,19,i==p->cursor?UI_SELECTED:UI_BG);
            ui_icon(m->publishing?UI_ICON_MIC:UI_ICON_MIC_OFF,x+w.content-40,y+1,m->publishing?UI_ACCENT:UI_DIM);
            ui_icon(m->receiving?UI_ICON_VOICE:UI_ICON_AUDIO_OFF,x+w.content-22,y+1,m->receiving?UI_DIM:UI_BUSY);
        }
        if(!n&&!p->loading)ui_wrap_area(x+8,65,w.content-16,"No one is connected.",0,3,UI_DIM);
        st_copy(label,sizeof(label),n?"Join the conversation.":"Start a voice call.");
        ui_wrap_area(x+8,190,w.content-16,label,0,2,UI_DIM);

    } else if(p->page==ST_CHAT)chat(p,c,x,w.content);
    else if(p->page==ST_DETAIL && c->message_count) {
        const StMessage *m=c->messages+c->message_count-1-(size_t)clamp(p->message,(int)c->message_count);
        int head=message_heading(c,m,x+8,32,w.content-16,1);
        int reactions_height=reaction_chips(m->reactions,0,0,w.content-16,0),media_height=m->attachments||m->embeds?24:0,visible=detail_visible(c,m,w.content-16);
        int lines=detail_lines(m,w.content-16),skip=clamp(p->scroll,lines>visible?lines-visible+1:1);
        detail_body(c,m,x+8,36+head,w.content-16,skip,visible);
        reaction_chips(m->reactions,x+8,216-reactions_height-media_height,w.content-16,1);
        if(media_height){char media[64];snprintf(media,sizeof(media),"Attachments: %d%s",m->attachments,m->embeds?" + embeds":"");ui_text_clip(x+8,192,w.content-16,media,UI_DIM);}
        StInviteLink links[ST_INVITE_LINK_MAX];int invitations=st_panels_invites(p,c,links);
        (void)invitations;
    } else if(p->page==ST_PROFILE)profile_card(p,c,x,w.content,confirm);
    else if(p->page==ST_REACTIONS)emoji_picker(p,c,x,w.content);
    else {
        int content_y=34;
        if(p->page==ST_ACCOUNT){presence_row(x+8,34,w.content-16,self_presence(c),UI_BG);ui_text_clip(x+8,55,w.content-16,st_str(cJSON_GetObjectItemCaseSensitive(c->self,"status"),"text"),UI_DIM);content_y=88;}
        for(int i=first_row;i<count&&i<first_row+8;i++) {
            const char *label="";
            if(p->page==ST_DMS){int index=st_panels_channel(p,c,i);label=index<0?"Saved messages":st_channel_name(c,c->channels+index);}
            if(p->page==ST_FRIENDS&&i>=2)label=c->users[st_panels_friend(c,i-2)].name;
            char social[300];
            if(p->page==ST_ADD_FRIEND){snprintf(social,sizeof(social),"Username: %s",p->friend_tag[0]?p->friend_tag:"username#0000");label=i?"Send friend request":social;}
            if(p->page==ST_REQUEST_ACTIONS){int incoming=0;for(size_t k=0;k<c->user_count;k++)if(!strcmp(c->users[k].id,p->person))incoming=!strcmp(c->users[k].relationship,"Incoming");label=incoming?(i?"Decline request":"Accept request"):"Cancel request";}
            if(p->page==ST_MESSAGE_ACTIONS)label=i==2?"Delete message":i?"Invitations":"Attachments";
            if(p->page==ST_INVITE_LINKS){StInviteLink links[ST_INVITE_LINK_MAX];st_panels_invites(p,c,links);snprintf(social,sizeof(social),"Invitation %s",links[i].code);label=social;}
            if(p->page==ST_CHAT_ACTIONS)label=i==4?"Leave group":i==0?"Send a photo":i==1?"Notifications":i==2?"Refresh messages":"Voice call";
            if(p->page==ST_SERVER_ACTIONS)label=i==2?"Leave server":i==1?"Notifications":"Refresh server";
            if(p->page==ST_NOTIFICATIONS){static const char *modes[]={"All messages","Mentions only","Nothing"};snprintf(social,sizeof(social),"%s %s",st_preferences_mode(c,p->notify_id,p->notify_server)==(enum StNotify)i?"[x]":"[ ]",modes[i]);label=social;}
            if(p->page==ST_SETTINGS)label=settings[i];
            if(p->page==ST_PROFILE_ACTIONS) {
                const char *relation=st_str(c->profile_user,"relationship");
                static const char *actions[]={"Write a message","Add friend","Block user","Report user","Mutual servers","Mutual conversations"};label=actions[i];
                if(i==1)label=!strcmp(relation,"Friend")?"Remove friend":!strcmp(relation,"Outgoing")?"Cancel friend request":!strcmp(relation,"Incoming")?"Accept friend request":"Add friend";
                if(i==2&&!strcmp(relation,"Blocked"))label="Unblock user";
            }
            if(p->page==ST_MUTUAL_SERVERS||p->page==ST_MUTUAL_CHATS){const cJSON *list=cJSON_GetObjectItemCaseSensitive(c->mutual,p->page==ST_MUTUAL_SERVERS?"servers":"channels"),*entry=cJSON_GetArrayItem(list,i);label=mutual_name(c,cJSON_IsString(entry)?entry->valuestring:"",p->page==ST_MUTUAL_SERVERS);}
            if(p->page==ST_REPORT){static const char *reasons[]={"Unsolicited spam","Spam / abuse","Inappropriate profile","Impersonation","Ban evasion","Underage user","Other (describe)"};label=reasons[i];}
            char session_label[320];
            if(p->page==ST_SESSIONS){if(i<2)label=i?"Revoke ALL sessions (log out)":"Revoke other sessions";else{const StSession *s=c->sessions+i-2;snprintf(session_label,sizeof(session_label),"%s%s",!strcmp(s->id,c->session_id)?"[This PSP] ":"",s->name[0]?s->name:"Unnamed session");label=session_label;}}
            if(p->page==ST_ACCOUNT)label=account[i];
            if(p->page==ST_STATUS)label=presence[i];
            int y=content_y+(i-first_row)*23;
            if(p->page==ST_DMS) {
                int index=st_panels_channel(p,c,i);if(index<0){menu_row(x,y,w.content,"Saved messages",UI_ICON_MESSAGE,i==p->cursor,p->focus==ST_CONTENT);continue;}
                const StChannel *dm=c->channels+index;if(!strcmp(dm->type,"SavedMessages")){menu_row(x,y,w.content,"Saved messages",UI_ICON_MESSAGE,i==p->cursor,p->focus==ST_CONTENT);continue;}
                const StUser *person=!strcmp(dm->type,"DirectMessage")?user_by_id(c,dm->peer):NULL;
                picture_row(x,y,w.content,label,person?"avatars":"icons",person?person->avatar:dm->icon,person,i==p->cursor,p->focus==ST_CONTENT);
                unread_avatar_badge(p->notices,dm->id,x+31,y+10,i==p->cursor?UI_SELECTED:UI_BG);
            } else if(p->page==ST_FRIENDS) {
                if(i<2)menu_row(x,y,w.content,i?"Friend requests":"Add friend",i?UI_ICON_PEOPLE:UI_ICON_ADD_USER,i==p->cursor,p->focus==ST_CONTENT);
                else person_row(c,c->users+st_panels_friend(c,i-2),x,y,w.content,i==p->cursor,p->focus==ST_CONTENT);
            } else if(p->page==ST_REQUESTS)person_row(c,c->users+st_panels_request(c,i),x,y,w.content,i==p->cursor,p->focus==ST_CONTENT);
            else menu_row(x,y,w.content,label,p->page==ST_REQUEST_ACTIONS&&!strcmp(label,"Cancel request")?UI_ICON_REMOVE_USER:menu_icon(p->page,i,c),i==p->cursor,p->focus==ST_CONTENT);
        }
        if(p->page==ST_STATUS) {
            const cJSON *status=cJSON_GetObjectItemCaseSensitive(c->self,"status");
            presence_row(x+8,201,w.content-16,self_presence(c),UI_BG);ui_text_clip(x+8,223,w.content-16,st_str(status,"text"),UI_DIM);
        }
        if(p->page==ST_REQUESTS&&count){int idx=st_panels_request(c,p->cursor);ui_text_clip(x+8,224,w.content-16,!strcmp(c->users[idx].relationship,"Incoming")?"Incoming request":"Outgoing request",UI_DIM);}
        if(p->page==ST_SETTINGS)ui_text_clip(x+8,221,w.content-16,"Client settings",UI_DIM);
        else if(p->page==ST_SESSIONS)ui_text_clip(x+8,224,w.content-16,c->session_id[0]?"Select a session to revoke it.":"Older save: current ID unavailable.",UI_DIM);
        else if(p->page==ST_REPORT)ui_text_clip(x+8,224,w.content-16,"Next: description, then send.",UI_DIM);
        else if(!count&&!p->loading&&!p->lists_loading&&!p->error[0])ui_wrap_area(x+8,40,w.content-16,"Nothing here yet",0,8,UI_DIM);
    }
    if(w.members) {
        int mx=x+w.content;ui_rect(mx,0,w.members,250,UI_SIDE);header(mx,120,"Members",p->focus==ST_MEMBERS);
        int n=st_panels_count(p,c,ST_MEMBERS);member_list(p,c,mx,n);
        if(p->members_loading){ui_rect(mx+4,n?222:34,112,n?26:24,UI_SIDE);ui_loader(mx+8,n?224:42,104,"Loading...");}
        else if(p->members_error[0]){ui_rect(mx+4,202,112,46,UI_SIDE);ui_wrap_area(mx+8,204,104,p->members_error,0,2,UI_DIM);}
        else if(!n)ui_wrap_area(mx+8,40,104,!strcmp(c->participants_channel,p->channel)?"Nothing here yet":"Loading members",0,8,UI_DIM);
        char total[64];snprintf(total,sizeof(total),"%d members",n);if(!p->members_loading&&!p->members_error[0])ui_text_clip(mx+8,224,104,total,UI_DIM);
    }
    if(p->lists_loading){
        if(w.nav>0){ui_clip(w.rail,0,w.rail+w.nav,250);ui_rect(w.rail+4,222,w.nav-8,26,UI_SIDE);ui_loader(w.rail+8,224,w.nav-16,"Account...");ui_clip_reset();}
        if(p->page==ST_DMS||p->page==ST_FRIENDS) {
            int ly=count?224:42;ui_rect(x+4,count?222:34,w.content-8,26,UI_BG);
            ui_loader(x+8,ly,w.content-16,"Loading list...");
        }
    }
    if(p->loading&&!(p->page==ST_CHAT&&!c->message_count)){
        ui_rect(x+4,222,w.content-8,26,UI_BG);ui_loader(x+8,224,w.content-16,"Loading...");
    }
    if(p->error[0]){ui_rect(x+4,202,w.content-8,46,UI_BG);ui_wrap_area(x+8,204,w.content-16,p->error,0,2,UI_DIM);}
    ui_rect(0,250,480,1,UI_LINE);
    char footer[160];snprintf(footer,sizeof(footer),"" UI_KEY_L "/" UI_KEY_R " panes  %s select  %s back%s",confirm,cancel,p->focus<=ST_NAV&&p->server[0]?"  " UI_KEY_TRIANGLE " server":p->page==ST_DETAIL||p->page==ST_APPEARANCE?"":p->page==ST_ATTACHMENTS?"  " UI_KEY_SQUARE " hide":"  " UI_KEY_SQUARE " refresh");
    if(p->focus==ST_CONTENT) {
        if(p->page==ST_DMS)snprintf(footer,sizeof(footer),"%s open  " UI_KEY_TRIANGLE " notifications  %s back",confirm,cancel);
        if(p->page==ST_PHOTOS)snprintf(footer,sizeof(footer),"%s choose  %s back",confirm,cancel);
        if(p->page==ST_CHAT)snprintf(footer,sizeof(footer),UI_KEY_L "/" UI_KEY_R " panes  %s open  " UI_KEY_TRIANGLE " write  " UI_KEY_RIGHT " reply  " UI_KEY_SQUARE " actions  " UI_KEY_ANALOG " scroll",confirm);
        else if(p->page==ST_VOICE)snprintf(footer,sizeof(footer),UI_KEY_SQUARE " call  %s profile  " UI_KEY_TRIANGLE " chat  %s back",confirm,cancel);
        else if(p->page==ST_DETAIL)snprintf(footer,sizeof(footer),"%s reactions  " UI_KEY_SQUARE " actions  " UI_KEY_DPAD " scroll  %s back",confirm,cancel);
        else if(p->page==ST_REACTIONS)snprintf(footer,sizeof(footer),"%s react  " UI_KEY_DPAD " choose  " UI_KEY_START "/" UI_KEY_SELECT " pages  %s back",confirm,cancel);
        else if(p->page==ST_PROFILE)snprintf(footer,sizeof(footer),"%s actions  " UI_KEY_DPAD " scroll  %s back",confirm,cancel);
        else if(p->page==ST_ATTACHMENTS){const StMessage *m=c->message_count?c->messages+c->message_count-1-(size_t)clamp(p->message,(int)c->message_count):NULL;int files=m?(m->attachments>ST_ATTACHMENT_MAX?ST_ATTACHMENT_MAX:m->attachments):0;if(p->cursor>=files)snprintf(footer,sizeof(footer),UI_KEY_DPAD " choose/scroll  %s back",cancel);else {const StAttachment *a=m->files+p->cursor;int ready=a->image&&!strcmp(a->id,p->media_requested)&&!p->media_error[0]&&ui_image_state("attachments",a->id)==3;snprintf(footer,sizeof(footer),"%s %s  " UI_KEY_DPAD " choose  " UI_KEY_SQUARE " hide  %s back",confirm,ready?"fullscreen":"load",cancel);}}
    }
    if(p->call_active)ui_icon(UI_ICON_VOICE,460,8,UI_ACCENT);
    ui_footer(p->network_notice[0]?p->network_notice:footer);
}
