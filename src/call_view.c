#include "call_view.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static const char *display_name(const StCallView *v,const StRtcMember *m){if(v->client&&(!*m->name||!strcmp(m->name,m->user)))for(size_t i=0;i<v->client->user_count;i++)if(!strcmp(v->client->users[i].id,m->user))return st_user_name(v->client,m->user);return m->name;}
void ui_startup(const StImage *icon,unsigned progress,const char *status) {
    unsigned background=icon&&icon->pixels?icon->pixels[0]|0xff000000:0xff19120d;
    ui_rect(0,0,480,272,background);ui_bitmap(icon,168,86,144,80,progress);
    if(status)ui_text_clip((480-ui_text_width(status))/2,230,460,status,0xffe8e8e8);
}
static void avatar(const StCallView *v,const StRtcMember *m,int x,int y,int size) {
    StMediaRequest key={.tag="avatars"};st_copy(key.id,sizeof(key.id),m->avatar);
    for(size_t i=0;v->client&&i<v->client->user_count;i++)if(!strcmp(v->client->users[i].id,m->user)) {
        const StUser *u=v->client->users+i;st_copy(key.id,sizeof(key.id),u->avatar);st_copy(key.filename,sizeof(key.filename),u->avatar_file);key.animated=u->avatar_animated;break;
    }
    if(!*key.id){st_copy(key.tag,sizeof(key.tag),"default");st_copy(key.id,sizeof(key.id),m->user);}
    ui_circle(x,y,size,UI_SIDE);
    if(!ui_image_ref(&key,x,y,size,size,1)){char name[9]={0};const char *original=display_name(v,m),*p=original;uint32_t cp=st_utf8_next(&p);size_t n=(size_t)(p-original);if(cp&&n<sizeof(name))memcpy(name,original,n);ui_text_clip(x+size/2-6,y+size/2-9,size/2,name,UI_WHITE);}
}
void st_call_view_draw(const StCallView *v,const char *confirm,const char *cancel) {
    ui_rect(0,0,480,272,UI_BG);ui_title(v->loopback?"Microphone loopback":v->channel&&*v->channel?v->channel:"Voice call");
    const StRtcStats *s=v->stats;
    if(v->loopback){ui_circle(207,62,66,UI_SIDE);ui_icon(UI_ICON_MIC,232,87,UI_ACCENT);ui_text(160,151,"Live microphone loopback",UI_WHITE);ui_text(178,177,"Use headphones.",UI_DIM);ui_rect(80,209,320,5,UI_LINE);ui_rect(80,209,(int)(v->mic_peak*320/32768),5,UI_ACCENT);char footer[80];snprintf(footer,sizeof(footer),"%s stop / back",cancel);ui_footer(footer);return;}
    if(v->reconnect){
        ui_icon(UI_ICON_VOICE,230,47,UI_ACCENT);
        ui_wrap_area(42,82,396,"Your account already has a voice connection. Reconnect here to replace it, including a call on another device.",0,6,UI_WHITE);
        ui_round_rect(40,231,242,34,7,UI_SELECTED);ui_text(50,238,confirm,UI_WHITE);ui_text(77,237,"Reconnect here",UI_WHITE);
        ui_round_rect(294,231,146,34,7,UI_SIDE);ui_text(305,238,cancel,UI_WHITE);ui_text(332,237,"Back",UI_DIM);return;
    }
    unsigned members[ST_RTC_MEMBERS],count=0;for(unsigned i=0;i<s->member_count;i++)if(s->members[i].connected)members[count++]=i;
    unsigned page=v->cursor>=0?(unsigned)v->cursor/6:0,first=page*6;
    if(!count){ui_loader(130,112,240,v->status?v->status:"Connecting...");}
    for(unsigned j=first;j<count&&j<first+6;j++) {
        const StRtcMember *m=s->members+members[j];int cell=(int)(j-first),x=8+(cell%3)*156,y=34+(cell/3)*90,selected=!v->controls&&(int)j==v->cursor;
        ui_round_rect(x,y,150,84,8,m->speaking?UI_ACCENT:selected?UI_SELECTED:UI_SIDE);
        ui_round_rect(x+2,y+2,146,80,7,UI_BG);avatar(v,m,x+8,y+7,34);
        int own=!strcmp(m->user,s->self),muted=own?(m->muted||s->mic_muted||s->deafen):m->muted;
        ui_icon(!own&&!m->volume?UI_ICON_AUDIO_OFF:muted?UI_ICON_MIC_OFF:UI_ICON_MIC,x+124,y+10,muted||(!own&&!m->volume)?UI_BUSY:UI_DIM);
        const char *name=own?"You":display_name(v,m);ui_text_clip(x+8,y+53,112,name,selected?UI_ACCENT:UI_WHITE);
        if(selected&&!own)ui_text(x+128,y+57,confirm,UI_ACCENT);
        if(!own){char volume[24];if(!m->track[0])st_copy(volume,sizeof(volume),"Mic off");else if(m->muted)st_copy(volume,sizeof(volume),"Muted");else if(m->subscribed)snprintf(volume,sizeof(volume),"%d%%",m->volume);else st_copy(volume,sizeof(volume),"Listen limit");ui_text_clip(x+49,y+15,72,volume,m->volume?UI_DIM:UI_BUSY);}
    }
    if(s->state!=ST_RTC_LISTEN||!s->publish_ready)ui_text_clip(10,211,460,v->status?v->status:"Connecting...",UI_DIM);
    for(int i=0;i<3;i++) {
        int x=8+i*156,muted=s->mic_muted||s->deafen;
        const char *key=i==0?UI_KEY_TRIANGLE:i==1?UI_KEY_SQUARE:cancel;
        const char *label=i==0?(muted?"Unmute":"Mute"):i==1?(s->deafen?"Undeafen":"Deafen"):"Disconnect";
        ui_round_rect(x,231,150,34,7,v->controls&&v->button==i?UI_SELECTED:UI_SIDE);
        ui_text(x+9,238,key,UI_WHITE);ui_text_clip(x+34,237,110,label,i==2?UI_BUSY:UI_WHITE);
    }
    if(v->member_settings&&v->cursor>=0&&(unsigned)v->cursor<count) {
        const StRtcMember *m=s->members+members[v->cursor];ui_round_rect(85,59,310,154,9,UI_SELECTED);ui_round_rect(87,61,306,150,8,UI_BG);
        ui_text_clip(103,85,274,display_name(v,m),UI_WHITE);char volume[80];snprintf(volume,sizeof(volume),"Volume: %d%%%s",m->volume,m->volume?"":" (muted)");ui_text(103,119,volume,UI_ACCENT);
        ui_rect(103,157,270,5,UI_LINE);ui_rect(103,157,m->volume*270/200,5,UI_ACCENT);
        char hint[100];snprintf(hint,sizeof(hint),UI_KEY_SQUARE " mute / restore  %s close",cancel);ui_text_clip(103,182,274,hint,UI_DIM);
    }
}
