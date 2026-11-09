#include "transport.h"
#include "ui.h"
#include "network.h"
#include "wifi.h"
#include "panels.h"
#include "media.h"
#include "jobs.h"
#include "emoji.h"
#include "reactions.h"
#include "storage.h"
#include "psp_tls.h"
#include "psp_dns.h"
#include "audio.h"
#include "voice_test.h"
#include "call.h"
#include "call_view.h"
#include "preferences.h"
#include "photos.h"
#include <stdlib.h>
#include <limits.h>
#include <kubridge.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <psppower.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef ST_MAIN_TEST
PSP_MODULE_INFO(ST_APP_NAME,0,1,0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(128);
/* The installed libcglue treats any negative heap size as automatic sizing;
 * its separate threshold defaults to only 512 KiB. Reserve room for PRX
 * modules, utility dialogs and PSP threads, which allocate outside malloc. */
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(8192);
#endif

volatile int app_running=1;
static volatile int cancelled,work_done;
static const char *volatile work_stage="Idle";
void st_stage(const char *stage){work_stage=stage;__sync_synchronize();}
static StClient client;
static StError error;
static int entropy_ok,network_ok,ready;
static StPanels panels;
static StPhotos photos;
static char photo_device[16]="ms0:";
static char draft[ST_TEXT_MAX],save_warning[256];
int entropy_init_savedata(const unsigned char nonce[16],StError *err);
void entropy_term(void);
int st_cancelled(void){return cancelled || !app_running;}
uint64_t st_millis(void){return sceKernelGetSystemTimeWide()/1000;}
void st_pause(void){sceKernelDelayThread(10000);}
static int exit_callback(int a,int b,void *p){(void)a;(void)b;(void)p;app_running=0;cancelled=1;sceKernelExitGame();return 0;}
static int callbacks(SceSize n,void *p){(void)n;(void)p;int cb=sceKernelCreateCallback("Exit",exit_callback,NULL);sceKernelRegisterExitCallback(cb);sceKernelSleepThreadCB();return 0;}

static StMediaCache media_cache;
static StJob active;
static int thread=-1,worker_thread=-1,ready_session,expired,worker_stalled;
/* UI priority 32 is above the worker's 36. Vblank alone is not a CPU budget:
 * a populated software-rendered chat may leave the worker only a tiny gap. */
void ui_background_yield(unsigned render_us){if((thread>=0&&!work_done)||st_call_busy())sceKernelDelayThread(render_us<10000?10000:render_us>50000?50000:render_us);}
static StWifi wifi;
static int choose_network,resume_network,dns_recoveries,dns_failed;
static volatile int transport_dirty;
static unsigned generation=1,call_generation=1;
static int call_force,call_conflict;
static int call_pending,call_auth_sent,call_screen,call_resync,voice_paused,voice_closed,call_mic_muted,call_deafened;
static int call_cursor,call_controls,call_button,call_member_settings,call_joined;
static unsigned call_members;static char call_settings_user[64];
static char call_channel[64],call_user[64];
static void leave_call(void){int had_call=call_screen||call_pending||st_call_busy();call_generation++;call_pending=call_auth_sent=call_force=call_conflict=0;call_screen=0;call_resync|=had_call;st_call_stop();if(had_call)st_audio_effect(ST_FX_LEAVE);if(thread>=0&&(active.type==ST_JOB_CALL_PREPARE||active.type==ST_JOB_CALL_AUTH))cancelled=1;}
static uint64_t started,stopped,voice_refresh_at,live_at,live_refresh_at;
static int preferences_loaded;static uint64_t preferences_at;
static int sound_loaded;static uint64_t sound_at,notice_at,ack_at,last_sound;
static StLive live;static unsigned live_failures,join_generation;
static char live_writing[64];static int background_dialog,live_read_failed;
typedef struct { enum StJobType type;unsigned generation;char a[ST_TEXT_MAX],b[ST_TEXT_MAX],d[256];int remove;StMediaRequest media; } Request;
static Request queue[2];static int queued;
static int quiet_job(enum StJobType t){return t==ST_JOB_PREFS_FETCH||t==ST_JOB_CALL_CATCHUP||t==ST_JOB_CALL_PREPARE||t==ST_JOB_CALL_AUTH||t==ST_JOB_MENTION||t==ST_JOB_SOUND||t==ST_JOB_NOTICES||t==ST_JOB_NOTICES_SAVE||t==ST_JOB_ACK;}
static int mutation(enum StJobType t){return t==ST_JOB_LEAVE_GROUP||t==ST_JOB_DELETE_MESSAGE||t==ST_JOB_PREFS_SET||t==ST_JOB_PHOTO||t==ST_JOB_CALL_AUTH||t==ST_JOB_FRIEND||t==ST_JOB_JOIN||t==ST_JOB_LEAVE||t==ST_JOB_LOGIN||t==ST_JOB_REGISTER||t==ST_JOB_ONBOARD||t==ST_JOB_SEND||t==ST_JOB_REACT||t==ST_JOB_LOGOUT||t==ST_JOB_STATUS||t==ST_JOB_CACHE_CLEAR||t==ST_JOB_USER_ACTION||t==ST_JOB_DM||t==ST_JOB_REPORT||t==ST_JOB_REVOKE;}
static void inline_error(const char *text){live_read_failed=0;st_copy(panels.error,sizeof(panels.error),text);}
static void cancel_active(void){if(thread>=0&&!work_done){if(!cancelled){cancelled=1;if(active.type!=ST_JOB_THUMBNAIL)st_network_abort();}if(!stopped)stopped=st_millis();}}
static void changed_view(void) {
    generation++;panels.loading=panels.members_loading=0;panels.error[0]=panels.members_error[0]=0;
    panels.media_requested[0]=panels.media_error[0]=0;
    if(thread>=0&&(st_job_read(active.type)||(active.type==ST_JOB_LIVE&&!live.connected)))cancel_active();
    for(int i=0;i<queued;) {
        if(st_job_read(queue[i].type)){st_wipe(queue+i,sizeof(queue[i]));for(int k=i+1;k<queued;k++)queue[k-1]=queue[k];queued--;st_wipe(queue+queued,sizeof(queue[0]));}
        else i++;
    }
}
static void loading(enum StJobType t,int yes) {
    if(quiet_job(t))return;
    if(t==ST_JOB_MEMBERS){panels.members_loading=yes;if(yes)panels.members_error[0]=0;}
    else if(t==ST_JOB_BOOTSTRAP||t==ST_JOB_HELLO||(t==ST_JOB_PROBE&&client.token[0]))panels.lists_loading=yes;
    else if(t!=ST_JOB_QUOTE&&t!=ST_JOB_THUMBNAIL&&t!=ST_JOB_MEDIA&&t!=ST_JOB_LIVE&&t!=ST_JOB_LIVE_CLOSE&&t!=ST_JOB_LIVE_HISTORY){panels.loading=yes;if(yes)panels.error[0]=0;}
}
static int online_required(enum StJobType t){return t!=ST_JOB_THUMBNAIL&&t!=ST_JOB_SOUND&&t!=ST_JOB_NOTICES_SAVE&&t!=ST_JOB_LIVE_CLOSE&&t!=ST_JOB_MEDIA&&t!=ST_JOB_CACHE_STATS&&t!=ST_JOB_CACHE_CLEAR;}
static int active_uses_network(void){return online_required(active.type)||(active.type==ST_JOB_MEDIA&&!active.media.offline&&strcmp(active.media.tag,"emoji"));}
static void mark_loader(enum StJobType t) {
    if(quiet_job(t))return;
    if(online_required(t)&&(wifi.state!=ST_WIFI_READY||dns_failed))return;
    if(t==ST_JOB_MEMBERS)panels.members_loading=1;
    else if(t==ST_JOB_BOOTSTRAP||t==ST_JOB_HELLO||(t==ST_JOB_PROBE&&client.token[0]))panels.lists_loading=1;
    else if(t!=ST_JOB_QUOTE&&t!=ST_JOB_THUMBNAIL&&t!=ST_JOB_MEDIA&&t!=ST_JOB_LIVE&&t!=ST_JOB_LIVE_CLOSE&&t!=ST_JOB_LIVE_HISTORY)panels.loading=1;
}
static void update_loaders(void) {
    panels.loading=panels.members_loading=panels.lists_loading=0;
    if(thread>=0&&!cancelled&&(active.generation==generation||active.type==ST_JOB_BOOTSTRAP||active.type==ST_JOB_HELLO||active.type==ST_JOB_PROBE))mark_loader(active.type);
    for(int i=0;i<queued;i++)if(queue[i].generation==generation||queue[i].type==ST_JOB_BOOTSTRAP||queue[i].type==ST_JOB_HELLO||queue[i].type==ST_JOB_PROBE)mark_loader(queue[i].type);
}
static int enqueue(enum StJobType type,const char *a,const char *b,const char *d,int remove) {
    if(mutation(type)) {
        if(type!=ST_JOB_LOGIN&&type!=ST_JOB_REGISTER&&type!=ST_JOB_CACHE_CLEAR&&!client.token[0]){inline_error("Log in to continue.");return 0;}
        if(online_required(type)&&(wifi.state!=ST_WIFI_READY||dns_failed)){inline_error("Connection unavailable. Choose a connection from the footer.");return 0;}
        if(thread>=0&&mutation(active.type)){inline_error("The previous operation is still running.");return 0;}
        for(int i=0;i<queued;i++)if(mutation(queue[i].type)){inline_error("The previous operation is still running.");return 0;}
    }
    /* Keep a submitted mutation; replace an obsolete queued read by the newest
     * destination. Only the worker owns sockets, TLS and its model snapshot. */
    for(int i=0;i<queued;i++)if(!mutation(queue[i].type)) {
        st_wipe(queue+i,sizeof(queue[i]));for(int k=i+1;k<queued;k++)queue[k-1]=queue[k];queued--;st_wipe(queue+queued,sizeof(queue[0]));break;
    }
    if(queued>=2){inline_error("A request is already queued.");return 0;}
    Request *r=queue+queued++;memset(r,0,sizeof(*r));r->type=type;r->generation=generation;r->remove=remove;
    st_copy(r->a,sizeof(r->a),a?a:"");st_copy(r->b,sizeof(r->b),b?b:"");st_copy(r->d,sizeof(r->d),d?d:"");loading(type,1);update_loaders();
    if(thread>=0&&(st_job_read(active.type)||(active.type==ST_JOB_LIVE&&!live.connected)))cancel_active();
    return 1;
}
static void close_members(void) {
    panels.members_loading=0;
    if(thread>=0&&active.type==ST_JOB_MEMBERS)cancel_active();
    for(int i=0;i<queued;) {
        if(queue[i].type==ST_JOB_MEMBERS){for(int k=i+1;k<queued;k++)queue[k-1]=queue[k];queued--;st_wipe(queue+queued,sizeof(queue[0]));}else i++;
    }
    if(strcmp(client.messages_channel,panels.channel))enqueue(ST_JOB_HISTORY,panels.channel,panels.before,NULL,0);
}
static int worker(SceSize n,void *p){
    (void)n;(void)p;
    for(;;) {
        __sync_synchronize();if(transport_dirty){transport_dirty=0;st_ws_live_close();st_transport_reset();}st_job_execute(&active);
        if(!active.result.token[0]||(active.ok&&(active.type==ST_JOB_JOIN||active.type==ST_JOB_LEAVE)))st_ws_live_close();
        if(active.type==ST_JOB_LOGIN&&active.ok){StError saved={0};if(!st_session_save(&active.result,"session.json",&saved))st_copy(active.error.text,sizeof(active.error.text),saved.text);}
        /* After publishing done, the worker never touches this job again until
         * the next wake. PSP remembers an early wake in its wakeup counter. */
        __sync_synchronize();work_done=1;
        if(sceKernelSleepThread()<0){sceKernelExitThread(0);return 0;}
    }
}
static void start_request(const Request *r) {
    if(transport_dirty)live.connected=0;
    if(r->generation==generation||r->type==ST_JOB_BOOTSTRAP||r->type==ST_JOB_HELLO||r->type==ST_JOB_PROBE)loading(r->type,1);
    if(!st_job_prepare(&active,&client,r->type,r->generation,r->a,r->b,r->d,r->remove,&error)){
        if(r->type==ST_JOB_MEDIA||r->type==ST_JOB_THUMBNAIL){StImage empty={0};st_media_commit(&media_cache,&r->media,&empty,0);}else{loading(r->type,0);inline_error(error.text);}return;
    }
    if(r->type==ST_JOB_VOICE)voice_refresh_at=st_millis()+30000;
    if(r->type==ST_JOB_NOTICES_SAVE) {
        active.events=st_notices_export(&live.notices);
        if(!active.events){st_job_clear(&active);notice_at=st_millis()+30000;return;}
    }
    if(r->type==ST_JOB_CALL_CATCHUP)active.events=st_notices_export(&live.notices);
    active.revision=r->type==ST_JOB_NOTICES_SAVE?live.notices.revision:live.revision;active.media=r->media;expired=0;cancelled=0;work_done=0;stopped=0;started=st_millis();work_stage="Starting";
    __sync_synchronize();
    int result;
    if(worker_thread<0) {
        worker_thread=sceKernelCreateThread(ST_APP_NAME " background",worker,0x24,256*1024,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);
        result=worker_thread<0?worker_thread:sceKernelStartThread(worker_thread,0,NULL);
        if(result<0&&worker_thread>=0){sceKernelDeleteThread(worker_thread);worker_thread=-1;}
    } else result=sceKernelWakeupThread(worker_thread);
    if(result>=0)thread=worker_thread;
    else {
        thread=-1;
        if(r->type==ST_JOB_MEDIA||r->type==ST_JOB_THUMBNAIL)st_media_commit(&media_cache,&active.media,&active.image,0);
        else inline_error("Cannot start background loading. Try again.");
        st_job_clear(&active);update_loaders();
    }
}
static void forget_account(void) {
    leave_call();
    StError saved={0};int forgotten=st_session_forget("session.json",&saved);
    st_photos_clear(&photos);preferences_loaded=0;preferences_at=0;ui_mentions_reset();st_audio_term();sound_loaded=0;sound_at=notice_at=ack_at=last_sound=0;st_notices_clear(&live.notices);memset(&live,0,sizeof(live));live_writing[0]=0;live_at=live_failures=join_generation=0;transport_dirty=1;
    st_media_clear(&media_cache);st_client_clear(&client);ready=ready_session=0;st_reactions_bind(&client);st_wipe(draft,sizeof(draft));
    st_wipe(&panels,sizeof(panels));st_panels_init(&panels);changed_view();
    for(int i=0;i<queued;i++)st_wipe(queue+i,sizeof(queue[i]));
    queued=0;call_resync=0;enqueue(ST_JOB_LIVE_CLOSE,NULL,NULL,NULL,0);if(!forgotten)inline_error(saved.text);
}
static void write_diagnostic(int stalled) {
    FILE *diag=fopen("netdiag.txt","wb");if(!diag)return;
    /* An unfinished worker still owns its HTTP result. Do not read that model
     * or its error/heap buffers until its completion publication. */
    fprintf(diag,ST_APP_NAME " %s\nJob %d\nStage %s\nElapsed %u ms\nHTTP %ld\nCancelled %d\nWorker stalled %d\n",ST_APP_VERSION,active.type,work_stage,(unsigned)(st_millis()-started),stalled?0:active.error.status,cancelled,stalled);
    if(stalled)fprintf(diag,"HTTP unavailable while worker is running; no automatic exit\n");
    else {
        StSocketStats sockets=st_psp_socket_stats();SceNetMallocStat memory;
        fprintf(diag,"Sockets opened %u closed %u live %u open-errors %u close-errors %u\n",sockets.opened,sockets.closed,sockets.live,sockets.open_errors,sockets.close_errors);
        StPollStats poll=st_psp_poll_stats();
        fprintf(diag,"TCP poll calls %u errors %u errno %u native-last %u native-max %u\n",poll.calls,poll.errors,poll.last_errno,poll.last_native,poll.maximum_native);
        StDnsStats dns=st_psp_dns_stats();
        fprintf(diag,"DNS result %u queries %u replies %u TCP %u elapsed %u ms sockets %u/%u\n",(unsigned)dns.result,dns.queries,dns.replies,dns.tcp,dns.elapsed_ms,dns.opened,dns.closed);
        StTransferStats transfer=st_transfer_stats();
        fprintf(diag,"Last transfer curl %d TCP %u ms TLS %u ms ready %u ms first-byte %u ms total %u ms\nRequest %u bytes upload %u bytes response-headers %u bytes new-connections %u\n",transfer.code,transfer.connect_ms,transfer.tls_ms,transfer.ready_ms,transfer.first_byte_ms,transfer.total_ms,transfer.request_bytes,transfer.upload_bytes,transfer.header_bytes,transfer.new_connections);
        StPspTlsStats tls=st_psp_tls_stats();
        fprintf(diag,"TLS work phase %u steps %u max-step %u ms CPU %u ms CPU-valid %u result %d deadline %u\nTLS IO reads %u writes %u received %u sent %u max-call %u ms\n",tls.phase,tls.steps,tls.max_step_ms,tls.cpu_ms,tls.cpu_valid,tls.result,tls.expired,tls.reads,tls.writes,tls.received,tls.sent,tls.max_io_ms);
        if(sceNetGetMallocStat(&memory)>=0)fprintf(diag,"Net pool %d free %d maximum %d\n",memory.pool,memory.free,memory.maximum);
        fprintf(diag,"Heap free %u largest %u\n",(unsigned)sceKernelTotalFreeMemSize(),(unsigned)sceKernelMaxFreeMemSize());
    }
    fclose(diag);
}
static void stall_worker(void) {
    worker_stalled=1;transport_dirty=1;write_diagnostic(1);
    st_copy(panels.network_notice,sizeof(panels.network_notice),"Waiting for connection...");
    panels.loading=panels.members_loading=panels.lists_loading=0;

}
static void restore_member_cursor(const char *id){if(*id)for(size_t i=0;i<client.participant_count;i++)if(!strcmp(client.participants[i].id,id)){panels.member_cursor=(int)i;break;}}
static void completed(void) {
    enum StJobType type=active.type;int was_cancelled=cancelled;
    char member_id[64]="";if(!strcmp(client.participants_channel,panels.channel)&&panels.member_cursor>=0&&(size_t)panels.member_cursor<client.participant_count)st_copy(member_id,64,client.participants[panels.member_cursor].id);
    __sync_synchronize();
    /* The outer watchdog used to cancel a blocked resolver after 60 s. That
     * is a timeout, not a user cancellation, and must permit WLAN recovery.
     * Apply this to event-channel establishment too, before its early return. */
    StDnsStats dns=st_psp_dns_stats();
    if(!active.ok&&(!was_cancelled||expired)&&!active.error.status&&!strcmp(work_stage,"DNS")&&
       wifi.state==ST_WIFI_READY&&(expired||dns.result==ST_DNS_TIMEOUT||dns.result==ST_DNS_NETWORK||dns.result==ST_DNS_NO_SERVER)) {
        if(!dns_recoveries){dns_recoveries=1;resume_network=transport_dirty=1;wifi_retry(&wifi);}
        else dns_failed=1;
    }
    if(type==ST_JOB_LIVE) {
        if(!active.ok&&(!was_cancelled||expired)&&!strcmp(work_stage,"DNS"))write_diagnostic(0);
        if(!strcmp(active.session,client.token)) {
            st_live_channel(&live,panels.channel);unsigned revision=live.revision,sounds=live.notices.sounds;
            st_live_events(&live,&client,active.events,st_millis());restore_member_cursor(member_id);live.connected=active.ok&&active.live_connected;
            const char *presence=st_str(cJSON_GetObjectItemCaseSensitive(client.self,"status"),"presence");
            if(live.notices.sounds!=sounds&&st_millis()-last_sound>=700&&strcmp(presence,"Busy")){st_audio_play();last_sound=st_millis();}
            if(revision==live.applied&&live.revision!=live.applied)live_refresh_at=st_millis()+120;
            if(active.ok){live_failures=0;live_at=st_millis()+100;}else{if(live_failures<4)live_failures++;live_at=st_millis()+(1000u<<live_failures);}
            if(live.logged_out||active.error.status==401){forget_account();inline_error("Session expired. Please log in again.");}
        }
        st_job_clear(&active);cancelled=0;stopped=0;update_loaders();return;
    }
    if(type==ST_JOB_CALL_PREPARE||type==ST_JOB_CALL_AUTH) {
        if(active.generation==call_generation&&!strcmp(active.session,client.token)) {
            call_pending=0;
            if(active.error.status==401){leave_call();forget_account();inline_error("Session expired. Please log in again.");}
            else if(!active.ok||was_cancelled){call_conflict=type==ST_JOB_CALL_AUTH&&!was_cancelled&&active.error.status==400&&strstr(active.error.text,"AlreadyConnected");st_call_error(active.error.text[0]?active.error.text:"Voice connection cancelled.");inline_error(call_conflict?"Your account already has a voice connection.":active.error.text);}
            else if(type==ST_JOB_CALL_PREPARE){st_audio_install_effects(active.effects);if(!st_call_start(&client.config,&active.call,call_channel,call_user))inline_error("Cannot start voice connection.");else {st_call_microphone(call_mic_muted);st_call_deafen(call_deafened);}}
            else if(!st_call_token(active.call.token))st_call_error("Voice ticket arrived after connection closed.");
        }
        st_job_clear(&active);cancelled=0;stopped=0;update_loaders();return;
    }
    if(quiet_job(type)&&type!=ST_JOB_CALL_CATCHUP) {
        if(!strcmp(active.session,client.token)) {
            if(active.error.status==401){forget_account();inline_error("Session expired. Please log in again.");}
            else if(type==ST_JOB_PREFS_FETCH){st_job_apply(&active,&client,active.generation,panels.channel);preferences_loaded=1;preferences_at=st_millis()+(active.ok?300000:30000);}
            else if(type==ST_JOB_MENTION){if(active.ok&&!strcmp(st_str(active.events,"_id"),active.a)){StError ignored={0};st_merge_user(&client,active.events,&ignored);}}
            else if(type==ST_JOB_SOUND){if(active.ok){st_audio_install(&active.sound);sound_loaded=1;}sound_at=st_millis()+(was_cancelled&&!expired?1000:60000);}
            else if(type==ST_JOB_NOTICES&&active.ok){st_notices_import(&live.notices,&client,active.events);notice_at=st_millis()+3000;}
            else if(type==ST_JOB_NOTICES_SAVE){if(active.ok)live.notices.saved=active.revision;notice_at=st_millis()+(active.ok?3000:30000);}
            else if(type==ST_JOB_ACK) {
                if(active.ok)for(size_t i=0;i<live.notices.count;i++)if(!strcmp(live.notices.items[i].channel,active.a)&&!strcmp(live.notices.items[i].read,active.b))live.notices.items[i].ack=0;
                ack_at=st_millis()+(active.ok||(was_cancelled&&!expired)?1500:30000);
            }
        }
        st_job_clear(&active);cancelled=0;stopped=0;update_loaders();return;
    }
    if(active.ok&&online_required(type)){dns_recoveries=dns_failed=0;}
    if(type==ST_JOB_MEDIA||type==ST_JOB_THUMBNAIL) {
        int invalid=(type==ST_JOB_THUMBNAIL&&!active.ok&&!was_cancelled)||active.error.status==401||active.error.status==403||active.error.status==404||active.error.status==410;
        st_media_commit(&media_cache,&active.media,&active.image,invalid?-1:active.ok);
        if(type==ST_JOB_THUMBNAIL)for(int i=0;i<ST_MEDIA_SLOTS;i++)if(!strcmp(media_cache.slots[i].key.tag,"photos")&&!strcmp(media_cache.slots[i].key.id,active.media.id))media_cache.slots[i].retry=was_cancelled?0:active.ok?0:UINT64_MAX;
        if(active.generation==generation&&(panels.page==ST_ATTACHMENTS||panels.page==ST_IMAGE)&&!strcmp(active.media.tag,"attachments")&&
           !strcmp(panels.media_requested,active.media.id)&&!active.ok)
            st_copy(panels.media_error,sizeof(panels.media_error),expired?"Image request timed out.":was_cancelled?"Image request cancelled.":active.error.text[0]?active.error.text:"Image could not be loaded");
    } else {
        if(expired&&!active.ok)st_copy(active.error.text,sizeof(active.error.text),"Request timed out. Try again.");
        int old_chat_position=panels.chat_manual?st_panels_chat_position(&panels,&client,panels.cursor):0;
        char selected_message[64]="";int at_end=type==ST_JOB_LIVE_HISTORY&&panels.page==ST_CHAT&&!client.message_count&&!panels.before[0]&&!active.d[0];enum StPage previous_page=panels.page;
        if(type==ST_JOB_LIVE_HISTORY&&client.message_count) {
            int selected=panels.page==ST_CHAT?panels.cursor:panels.message;
            at_end=panels.page==ST_CHAT&&selected==(int)client.message_count-1&&st_panels_chat_bottom(&panels,&client)&&!panels.before[0]&&!active.d[0];
            if(selected>=0&&selected<(int)client.message_count)st_copy(selected_message,64,client.messages[client.message_count-1-(size_t)selected].id);
        }else if(type==ST_JOB_HISTORY&&panels.page==ST_REACTIONS&&client.message_count&&panels.message<(int)client.message_count)st_copy(selected_message,sizeof(selected_message),client.messages[client.message_count-1-(size_t)panels.message].id);
        int accepted=st_job_apply(&active,&client,generation,panels.channel);
        restore_member_cursor(member_id);
        if(accepted&&type==ST_JOB_CACHE_CLEAR){st_media_clear(&media_cache);panels.cache_bytes=active.stats.bytes;panels.cache_files=active.stats.files;}
        if(active.generation==generation)loading(type,0);
        else if(type==ST_JOB_BOOTSTRAP||type==ST_JOB_HELLO||type==ST_JOB_PROBE)panels.lists_loading=0;
        if((active.error.status==401&&type!=ST_JOB_LOGIN&&type!=ST_JOB_REGISTER&&(type!=ST_JOB_REVOKE||!active.result.token[0]))||
           (type==ST_JOB_BOOTSTRAP&&!active.result.token[0]&&!active.ok)) {
            forget_account();if(!panels.error[0])inline_error("Session expired. Please log in again.");
        } else if(accepted && !active.ok) {
            if(!(was_cancelled&&st_job_read(type)&&!expired)) {
                if(type==ST_JOB_QUOTE){for(size_t i=0;i<client.message_count;i++)for(int k=0;k<client.messages[i].reply_count;k++)if(!strcmp(client.messages[i].replies[k].id,active.b)){client.messages[i].replies[k].retry=st_millis()+15000;client.messages[i].replies[k].state=3;}}
                else if(type==ST_JOB_MEMBERS)st_copy(panels.members_error,sizeof(panels.members_error),active.error.text);
                else {inline_error(active.error.text);if(type==ST_JOB_LIVE_HISTORY){live_read_failed=1;live_refresh_at=st_millis()+5000;}}
            }
        } else if(accepted && active.ok) {
            switch(type) {
                case ST_JOB_PROBE:
                    if(client.token[0])enqueue(ready_session?ST_JOB_BOOTSTRAP:ST_JOB_HELLO,NULL,NULL,NULL,0);
                    break;
                case ST_JOB_LOGIN: {
                    if(active.error.text[0])st_copy(save_warning,sizeof(save_warning),active.error.text);
                    st_wipe(panels.password,sizeof(panels.password));ready_session=0;enqueue(ST_JOB_HELLO,NULL,NULL,NULL,0);break;
                }
                case ST_JOB_REGISTER:
                    st_wipe(panels.password,sizeof(panels.password));st_wipe(panels.invite,sizeof(panels.invite));
                    panels.page=ST_LOGIN;panels.cursor=1;changed_view();
                    inline_error(client.email_verification?"Request accepted. Verify your email, then log in.":"Registration request accepted. Please log in.");break;
                case ST_JOB_HELLO:
                    if(client.onboarding){panels.page=ST_ONBOARD;panels.cursor=0;changed_view();}
                    else enqueue(ST_JOB_BOOTSTRAP,NULL,NULL,NULL,0);
                    break;
                case ST_JOB_ONBOARD:enqueue(ST_JOB_BOOTSTRAP,NULL,NULL,NULL,0);break;
                case ST_JOB_CALL_CATCHUP:{
                    cJSON *root=cJSON_CreateObject();cJSON_AddItemToObject(root,"local",cJSON_Duplicate(active.events,1));st_notices_import(&live.notices,&client,root);cJSON_Delete(root);
                    if(active.remove&&strcmp(st_str(cJSON_GetObjectItemCaseSensitive(client.self,"status"),"presence"),"Busy"))st_audio_play();
                    live.connected=0;live_at=0;live.lists_dirty=0;voice_refresh_at=st_millis()+30000;ready=ready_session=1;
                    /* Event reconnect cannot replay edits/deletions/reactions
                     * missed during voice. Refresh even an unchanged last ID. */
                    if(panels.page==ST_CHAT||panels.page==ST_DETAIL||panels.page==ST_REACTIONS||panels.page==ST_ATTACHMENTS||panels.page==ST_IMAGE)for(size_t k=0;k<client.channel_count;k++)if(!strcmp(client.channels[k].id,panels.channel)){
                        char pivot[64]="";int selected=panels.page==ST_CHAT?panels.cursor:panels.message;
                        if(client.message_count&&(panels.page!=ST_CHAT||panels.before[0]||!st_panels_chat_bottom(&panels,&client))&&selected>=0&&selected<(int)client.message_count)st_copy(pivot,64,client.messages[client.message_count-1-(size_t)selected].id);
                        enqueue(ST_JOB_LIVE_HISTORY,panels.channel,panels.before,pivot,0);break;
                    }
                    break;
                }
                case ST_JOB_BOOTSTRAP:
                    preferences_loaded=0;
                    ready=ready_session=1;st_notices_sync(&live.notices,&client);voice_refresh_at=st_millis()+30000;
                    if(panels.page==ST_LOGIN||panels.page==ST_REGISTER||panels.page==ST_ONBOARD){panels.nav_tab=panels.nav_cursor=0;st_panels_select_tab(&panels);changed_view();}
                    if(join_generation&&join_generation==generation&&client.joined_server[0]) {
                        for(size_t i=0;i<client.server_count;i++)if(!strcmp(client.servers[i].id,client.joined_server)){panels.rail_cursor=(int)i+1;st_panels_select_rail(&panels,&client);break;}
                        join_generation=0;
                    }
                    if(save_warning[0]){inline_error(save_warning);save_warning[0]=0;}
                    break;
                case ST_JOB_LIVE_HISTORY: {
                    if(live_read_failed){panels.error[0]=0;live_read_failed=0;}
                    st_copy(panels.nearby,64,active.d);
                    live.applied=active.revision;size_t i;
                    for(i=0;i<client.message_count;i++)if(!strcmp(client.messages[i].id,selected_message))break;
                    if(at_end){panels.cursor=client.message_count?(int)client.message_count-1:0;panels.chat_manual=0;}
                    else if(i<client.message_count){int selected=(int)(client.message_count-1-i);if(previous_page==ST_CHAT){panels.cursor=selected;if(panels.chat_manual)panels.chat_offset+=st_panels_chat_position(&panels,&client,selected)-old_chat_position;}else panels.message=selected;}
                    else if(previous_page!=ST_CHAT){panels.page=ST_CHAT;panels.scroll=0;panels.media_requested[0]=0;}
                    break;
                }
                case ST_JOB_FRIEND:if(active.generation==generation){panels.page=ST_REQUESTS;panels.cursor=0;changed_view();}break;
                case ST_JOB_USER_ACTION:if(active.generation==generation&&panels.page==ST_REQUEST_ACTIONS&&!strcmp(panels.person,active.a)){panels.page=ST_REQUESTS;panels.cursor=0;changed_view();}break;
                case ST_JOB_JOIN:
                    live.connected=0;
                    if(active.generation==generation){join_generation=generation;panels.page=ST_EMPTY;}
                    enqueue(ST_JOB_BOOTSTRAP,NULL,NULL,NULL,0);live_at=0;break;
                case ST_JOB_LEAVE_GROUP:
                case ST_JOB_LEAVE:live.connected=0;st_notices_sync(&live.notices,&client);st_panels_reconcile(&panels,&client);changed_view();live_at=0;break;
                case ST_JOB_VOICE:st_notices_sync(&live.notices,&client);live.lists_dirty=0;break;
                case ST_JOB_HISTORY:
                    panels.nearby[0]=0;live.applied=active.revision;
                    st_copy(panels.before,sizeof(panels.before),active.b);
                    if(selected_message[0]){size_t i;for(i=0;i<client.message_count;i++)if(!strcmp(client.messages[i].id,selected_message))break;if(i<client.message_count){panels.message=(int)(client.message_count-1-i);break;}}
                    panels.page=ST_CHAT;panels.cursor=client.message_count?(int)client.message_count-1:0;break;
                case ST_JOB_PHOTO:
                    if(active.generation==generation&&!strcmp(panels.channel,active.a)){panels.page=ST_CHAT;panels.chat_manual=0;enqueue(ST_JOB_HISTORY,panels.channel,NULL,NULL,0);}else inline_error("Photo sent.");
                    break;
                case ST_JOB_SEND:
                    if(!strcmp(panels.channel,active.a)&&!strcmp(draft,active.b)){st_wipe(draft,sizeof(draft));enqueue(ST_JOB_HISTORY,panels.channel,NULL,NULL,0);}
                    else inline_error("Message sent.");
                    break;
                case ST_JOB_DELETE_MESSAGE:
                    if(active.generation==generation&&!strcmp(panels.channel,active.a)){panels.page=ST_CHAT;panels.cursor=panels.message;st_panels_reconcile(&panels,&client);changed_view();}
                    break;
                case ST_JOB_REACT:
                    if(!active.remove){
                        int selected=st_reactions_at(panels.cursor);st_reactions_used_key(active.d);
                        if(panels.page==ST_REACTIONS)panels.cursor=st_reactions_position(selected);
                        StError saved={0};if(!st_reactions_save(ST_REACTION_SETTINGS,&saved))inline_error(saved.text);
                    }
                    if(!strcmp(panels.channel,active.a))enqueue(panels.nearby[0]?ST_JOB_LIVE_HISTORY:ST_JOB_HISTORY,panels.channel,panels.before,panels.nearby,0);
                    break;
                case ST_JOB_LOGOUT:forget_account();break;
                case ST_JOB_REVOKE:
                    if(!client.token[0])forget_account();else if(panels.page==ST_SESSIONS)enqueue(ST_JOB_SESSIONS,NULL,NULL,NULL,0);
                    break;
                case ST_JOB_DM:
                    if(active.generation==generation){panels.server[0]=0;panels.rail_cursor=0;panels.nav_tab=panels.nav_cursor=0;for(size_t i=0;i<client.channel_count;i++)if(!strcmp(client.channels[i].id,client.opened_dm)){StChannel ch=client.channels[i];changed_view();free(client.messages);client.messages=NULL;client.message_count=0;client.messages_channel[0]=0;st_panels_open_chat(&panels,&client,&ch);enqueue(ST_JOB_HISTORY,ch.id,NULL,NULL,0);break;}}
                    break;
                case ST_JOB_REPORT:inline_error("Report sent.");break;
                case ST_JOB_MEMBERS:
                    live.members_dirty=0;
                    if(strcmp(client.messages_channel,panels.channel))enqueue(ST_JOB_HISTORY,panels.channel,panels.before,NULL,0);
                    break;
                case ST_JOB_CACHE_CLEAR:
                    st_media_clear(&media_cache);panels.cache_bytes=active.stats.bytes;panels.cache_files=active.stats.files;break;
                case ST_JOB_CACHE_STATS:
                    panels.cache_bytes=active.stats.bytes;panels.cache_files=active.stats.files;break;
                default:break;
            }
        }
    }
    if(type!=ST_JOB_THUMBNAIL&&!active.ok&&(!was_cancelled||expired))write_diagnostic(0);
    st_job_clear(&active);st_reactions_bind(&client);cancelled=0;stopped=0;update_loaders();
}
static void refresh(void);
static void connection_tick(void) {
    if(!network_ok)return;
    int was_ready=wifi.state==ST_WIFI_READY;
    int connected=wifi_poll(&wifi,st_millis(),thread<0);
    st_copy(panels.network_notice,sizeof(panels.network_notice),dns_failed?"DNS failed. " UI_KEY_SQUARE " choose connection.":wifi.notice);
    if(wifi.state==ST_WIFI_OFF)dns_recoveries=dns_failed=0;
    if(was_ready&&!connected){leave_call();st_network_abort_all();live.connected=0;live_at=0;resume_network=transport_dirty=1;if(thread>=0&&active_uses_network())cancel_active();}
    if(choose_network&&!background_dialog&&thread<0&&wifi.state!=ST_WIFI_OFF) {
        leave_call();st_network_abort_all();live.connected=0;live_at=0;transport_dirty=1;sceNetApctlDisconnect();int selected=ui_network();wifi_dialog_result(&wifi,selected);choose_network=0;
        connected=selected;if(selected)dns_recoveries=dns_failed=0;st_copy(panels.network_notice,sizeof(panels.network_notice),wifi.notice);
    }
    if(connected&&resume_network){
        live_at=0;resume_network=0;
        for(int i=0;i<ST_MEDIA_SLOTS;i++)if(media_cache.slots[i].state==4)media_cache.slots[i].retry=0;
        /* Only reads are resumed. Submitted POSTs retain their original result
         * and nonce; a failed mutation is never automatically sent again. */
        if(!queued)refresh();
    }
    if(worker_stalled)st_copy(panels.network_notice,sizeof(panels.network_notice),"Waiting for connection...");
    update_loaders();
}
static StRtcStats call_snapshot;
static void tick(void) {
    st_voice_test_poll();st_call_poll();
    StRtcStats call_stats=st_call_snapshot();call_snapshot=call_stats;if(call_screen){call_snapshot.mic_muted=call_mic_muted;call_snapshot.deafen=call_deafened;}
    if(call_screen&&call_stats.state==ST_RTC_LISTEN&&!call_joined){st_audio_effect(ST_FX_JOIN);call_joined=1;}
    unsigned connected=0;for(unsigned i=0;i<call_stats.member_count;i++)connected+=call_stats.members[i].connected!=0;
    if(call_screen&&call_joined&&call_members&&connected!=call_members)st_audio_effect(connected>call_members?ST_FX_JOIN:ST_FX_LEAVE);
    call_members=connected;
    panels.call_active=st_call_busy()||call_pending;panels.call_failed=call_stats.state==ST_RTC_FAILED;st_copy(panels.call_channel,64,call_pending?call_channel:st_call_channel());st_copy(panels.call_user,64,call_pending?call_user:st_call_user());
    static const char *states[]={"Preparing call...","Joining call...","Connecting...","Connecting...","Connecting...","Connected","Unable to connect","Disconnected"};
    st_copy(panels.call_status,sizeof(panels.call_status),call_stats.state==ST_RTC_FAILED?call_stats.error:states[call_stats.state]);
    if(call_stats.state==ST_RTC_LISTEN){
        if(call_stats.publish_ready)st_copy(panels.call_status,sizeof(panels.call_status),"Connected");else st_copy(panels.call_status,sizeof(panels.call_status),"Connecting microphone...");}
    panels.notices=&live.notices;live.notices.visible[0]=0;
    if(!background_dialog&&panels.page==ST_CHAT&&!panels.loading&&!panels.before[0]&&!panels.nearby[0]&&
       !strcmp(client.messages_channel,panels.channel)&&panels.cursor==(int)client.message_count-1&&st_panels_chat_bottom(&panels,&client)) {
        st_copy(live.notices.visible,64,panels.channel);
        if(client.message_count)st_notices_read(&live.notices,panels.channel,client.messages[0].id);
    }
    st_live_channel(&live,panels.channel);st_live_typing_text(&live,&client,st_millis(),panels.typing,sizeof(panels.typing));
    connection_tick();
    if(thread>=0) {
        uint64_t now=st_millis();unsigned limit=active.type==ST_JOB_PHOTO?180000:60000; /* HTTP has its own 5s image deadline; allow disk/decode time. */
        if(!work_done&&(active_uses_network()||active.type==ST_JOB_THUMBNAIL||active.type==ST_JOB_MEDIA)&&now-started>=limit){expired=1;cancel_active();}
        if(stopped&&!work_done&&now-stopped>=8000&&!worker_stalled)stall_worker();
        if(!work_done)return;
        __sync_synchronize();int stalled=worker_stalled;worker_stalled=0;thread=-1;completed();
        if(stalled){panels.network_notice[0]=0;if(!queued)refresh();}
    }
    if(call_pending&&queued&&mutation(queue[0].type)&&wifi.state==ST_WIFI_READY){Request r=queue[0];for(int i=1;i<queued;i++)queue[i-1]=queue[i];queued--;start_request(&r);return;}
    if(wifi.state==ST_WIFI_READY&&client.token[0]&&(call_pending||(!call_auth_sent&&st_call_needs_token()))) {
        Request request={.type=call_pending?ST_JOB_CALL_PREPARE:ST_JOB_CALL_AUTH,.generation=call_generation};
        st_copy(request.a,sizeof(request.a),call_channel);
        if(!call_pending){call_auth_sent=1;request.remove=call_force;}
        start_request(&request);return;
    }
    if(call_screen||voice_paused||st_call_busy())return;
    if(call_resync&&wifi.state==ST_WIFI_READY&&client.token[0]){call_resync=0;Request r={.type=ST_JOB_CALL_CATCHUP,.generation=generation};start_request(&r);return;}
    if(queued && (!online_required(queue[0].type)||(wifi.state==ST_WIFI_READY&&!dns_failed))) {
        Request request=queue[0];for(int i=1;i<queued;i++)queue[i-1]=queue[i];st_wipe(queue+--queued,sizeof(queue[0]));
        start_request(&request);st_wipe(&request,sizeof(request));update_loaders();return;
    }
    st_live_channel(&live,panels.channel);st_live_typing_text(&live,&client,st_millis(),panels.typing,sizeof(panels.typing));
    if(client.token[0]&&ready&&wifi.state==ST_WIFI_READY&&!dns_failed) {
        if(!live.notices.loaded){Request request={.type=ST_JOB_NOTICES,.generation=generation};start_request(&request);return;}
        if(!preferences_loaded||st_millis()>=preferences_at){Request request={.type=ST_JOB_PREFS_FETCH,.generation=generation};start_request(&request);return;}
        if(!sound_loaded&&st_millis()>=sound_at){sound_at=st_millis()+60000;Request request={.type=ST_JOB_SOUND,.generation=generation};start_request(&request);return;}
    }
    if(client.token[0]&&ready&&wifi.state==ST_WIFI_READY&&!dns_failed&&client.ws_url[0]) {
        if(st_millis()>=live_at){Request request={.type=ST_JOB_LIVE,.generation=generation};st_copy(request.a,sizeof(request.a),live_writing);start_request(&request);return;}
        if(live.revision!=live.applied&&st_millis()>=live_refresh_at&&
           (panels.page==ST_CHAT||panels.page==ST_DETAIL||panels.page==ST_REACTIONS||panels.page==ST_ATTACHMENTS||panels.page==ST_IMAGE)) {
            char pivot[64]="";int selected=panels.page==ST_CHAT?panels.cursor:panels.message;
            if(client.message_count&&(panels.page!=ST_CHAT||panels.before[0]||panels.nearby[0]||selected!=(int)client.message_count-1)&&selected>=0&&selected<(int)client.message_count)st_copy(pivot,64,client.messages[client.message_count-1-(size_t)selected].id);
            live_refresh_at=st_millis()+1000;enqueue(ST_JOB_LIVE_HISTORY,panels.channel,panels.before,pivot,0);return;
        }
        if(live.lists_dirty){enqueue(ST_JOB_VOICE,panels.channel,NULL,NULL,0);return;}
        if(live.members_dirty&&(panels.member_width||panels.focus==ST_MEMBERS)&&panels.channel[0]){live.members_dirty=0;enqueue(ST_JOB_MEMBERS,panels.channel,NULL,NULL,0);return;}
    }
    if(client.token[0]&&ready&&wifi.state==ST_WIFI_READY&&!dns_failed&&(panels.server[0]||panels.page==ST_VOICE)&&
       (panels.page==ST_VOICE||panels.page==ST_EMPTY||panels.page==ST_CHAT)&&st_millis()>=voice_refresh_at) {
        voice_refresh_at=st_millis()+30000;enqueue(ST_JOB_VOICE,panels.channel,NULL,NULL,0);return;
    }
    if(client.token[0]&&ready&&live.notices.loaded) {
        if(live.notices.revision!=live.notices.saved&&st_millis()>=notice_at){Request request={.type=ST_JOB_NOTICES_SAVE,.generation=generation};start_request(&request);return;}
        if(wifi.state==ST_WIFI_READY&&!dns_failed&&st_millis()>=ack_at)for(size_t i=0;i<live.notices.count;i++)if(live.notices.items[i].ack){Request request={.type=ST_JOB_ACK,.generation=generation};st_copy(request.a,sizeof(request.a),live.notices.items[i].channel);st_copy(request.b,sizeof(request.b),live.notices.items[i].read);start_request(&request);return;}
    }
    if(client.token[0]&&ready&&panels.page!=ST_CACHE) {
        if(wifi.state==ST_WIFI_READY&&!dns_failed&&!strcmp(client.messages_channel,panels.channel)&&client.message_count&&(panels.page==ST_CHAT||panels.page==ST_DETAIL)) {
            int selected=panels.page==ST_DETAIL?panels.message:panels.cursor;
            for(int offset=-1;offset<=1;offset++) {
                int row=selected+offset;if(row<0||row>=(int)client.message_count)continue;
                StMessage *m=client.messages+client.message_count-1-(size_t)row;
                for(int j=0;j<m->reply_count;j++)if((m->replies[j].state==0||m->replies[j].state==3)&&st_millis()>=m->replies[j].retry) {
                    m->replies[j].state=0;m->replies[j].retry=st_millis()+15000;
                    Request request={.type=ST_JOB_QUOTE,.generation=generation};st_copy(request.a,sizeof(request.a),panels.channel);st_copy(request.b,sizeof(request.b),m->replies[j].id);start_request(&request);return;
                }
            }
        }
        if(wifi.state==ST_WIFI_READY&&!dns_failed&&(panels.page==ST_CHAT||panels.page==ST_DETAIL||panels.page==ST_ATTACHMENTS)&&!strcmp(client.messages_channel,panels.channel)) {
            Request request={.type=ST_JOB_MENTION,.generation=generation};
            if(ui_mention_next(request.a)){start_request(&request);return;}
        }
        Request request={.type=ST_JOB_MEDIA,.generation=generation};
        if(st_media_next(&media_cache,&request.media)){
            if(!strcmp(request.media.tag,"photos")){
                request.type=ST_JOB_THUMBNAIL;int found=0;
                if(panels.page==ST_PHOTOS&&!photos.sources)for(size_t i=0;i<photos.count;i++)if(!photos.items[i].directory&&!strcmp(photos.items[i].key,request.media.id)){found=st_photos_file(&photos,(int)i,request.a,sizeof(request.a),&error);break;}
                if(!found){StImage empty={0};st_media_commit(&media_cache,&request.media,&empty,-1);return;}
            }else request.media.offline=wifi.state!=ST_WIFI_READY||dns_failed;
            start_request(&request);
        }
    }
}
void ui_voice_pause(int paused){voice_paused=paused;if(paused){voice_closed=0;if(thread>=0&&!mutation(active.type))cancel_active();for(int i=0;i<queued;)if(!mutation(queue[i].type)){for(int k=i+1;k<queued;k++)queue[k-1]=queue[k];queued--;}else i++;}else {call_resync=1;live_at=0;}}
int ui_voice_idle(void){if(thread>=0)return 0;if(queued&&mutation(queue[0].type)){Request r=queue[0];for(int i=1;i<queued;i++)queue[i-1]=queue[i];queued--;start_request(&r);return 0;}if(!voice_closed){Request r={.type=ST_JOB_LIVE_CLOSE,.generation=generation};voice_closed=1;start_request(&r);return 0;}return 1;}
void ui_background(void){background_dialog++;tick();background_dialog--;}
static int confirm_text(const char *title,const char *text) {
    int scroll=0;
    while(app_running) {
        ui_background();ui_begin();ui_title(title);int lines=ui_wrap(12,34,text,scroll,11,UI_WHITE);char hint[120];snprintf(hint,sizeof(hint),"%s confirm  %s cancel  " UI_KEY_DPAD " scroll",ui_confirm_label(),ui_cancel_label());ui_footer(hint);ui_end();unsigned b=ui_buttons();
        if(b&ui_confirm_button())return 1;
        if(b&ui_cancel_button())return 0;
        if((b&PSP_CTRL_DOWN)&&scroll+11<lines)scroll++;
        if((b&PSP_CTRL_UP)&&scroll)scroll--;
    }
    return 0;
}
static int connect_api(void) {
    if(!entropy_ok||!network_ok){if(!panels.error[0])inline_error("Network unavailable. Fix initialization and restart.");return 0;}
    if(wifi.state!=ST_WIFI_READY||dns_failed){choose_network=resume_network=1;cancel_active();update_loaders();return 0;}
    return enqueue(ST_JOB_PROBE,NULL,NULL,NULL,0);
}
static void load_lists(void){if(!client.discovered)connect_api();else if(client.token[0])enqueue(ST_JOB_BOOTSTRAP,NULL,NULL,NULL,0);}
static void open_chat(int index) {
    if(index<0||index>=(int)client.channel_count||!ready)return;
    StChannel channel=client.channels[index];
    if(strcmp(panels.channel,channel.id)){st_wipe(draft,sizeof(draft));if(!strcmp(client.pending_channel,channel.id))st_copy(draft,sizeof(draft),client.pending_text);}
    changed_view();free(client.messages);client.messages=NULL;client.message_count=0;client.messages_channel[0]=0;
    st_panels_open_chat(&panels,&client,&channel);enqueue(channel.voice?ST_JOB_VOICE:ST_JOB_HISTORY,channel.id,NULL,NULL,0);
}
static void refresh(void) {
    if(panels.page==ST_APPEARANCE)return;
    if(panels.page==ST_PHOTOS){if(!photos.sources&&!st_photos_scan(&photos,&error))inline_error(error.text);return;}
    if(panels.page!=ST_CACHE&&(wifi.state!=ST_WIFI_READY||dns_failed)){choose_network=resume_network=1;cancel_active();update_loaders();return;}
    if(panels.page==ST_LOGIN||panels.page==ST_REGISTER){connect_api();return;}
    if(panels.page==ST_ONBOARD){enqueue(ST_JOB_HELLO,NULL,NULL,NULL,0);return;}
    if(panels.page==ST_NOTIFICATIONS)enqueue(ST_JOB_PREFS_FETCH,NULL,NULL,NULL,0);
    else if(panels.focus==ST_MEMBERS)enqueue(ST_JOB_MEMBERS,panels.channel,NULL,NULL,0);
    else if(panels.page==ST_VOICE)enqueue(ST_JOB_VOICE,panels.channel,NULL,NULL,0);
    else if(panels.page==ST_CHAT||panels.page==ST_DETAIL||panels.page==ST_REACTIONS||panels.page==ST_CHAT_ACTIONS)enqueue(panels.nearby[0]?ST_JOB_LIVE_HISTORY:ST_JOB_HISTORY,panels.channel,panels.before,panels.nearby,0);
    else if(panels.page==ST_PROFILE||panels.page==ST_PROFILE_ACTIONS||panels.page==ST_MUTUAL_SERVERS||panels.page==ST_MUTUAL_CHATS)enqueue(ST_JOB_USER,panels.person,panels.profile_server,NULL,0);
    else if(panels.page==ST_SESSIONS)enqueue(ST_JOB_SESSIONS,NULL,NULL,NULL,0);
    else if(panels.page==ST_CACHE)enqueue(ST_JOB_CACHE_STATS,NULL,NULL,NULL,0);
    else load_lists();
}
static void compose_to(const char *reply) {
    if(!ready)return;
    char channel[64];st_copy(channel,sizeof(channel),panels.channel);st_copy(live_writing,sizeof(live_writing),channel);live_at=0;
    char target[64];st_copy(target,sizeof(target),reply?reply:"");
    int accepted=ui_osk(target[0]?"Reply to message":"New message",draft,sizeof(draft),2000);live_writing[0]=0;live_at=0;
    if(accepted&&*draft&&ready&&!strcmp(channel,panels.channel))enqueue(ST_JOB_SEND,channel,draft,target,0);
}
static void compose(void){compose_to(NULL);}
static void reply_selected(void) {
    if(strcmp(client.messages_channel,panels.channel)){inline_error("Wait for this chat to load before replying.");return;}
    int row=panels.page==ST_DETAIL?panels.message:panels.cursor;
    if(row<0||row>=(int)client.message_count)return;
    const StMessage *m=client.messages+client.message_count-1-(size_t)row;
    char id[64];st_copy(id,sizeof(id),m->id);compose_to(id);
}
static void voice_listen(void) {
    if(call_pending||st_call_busy()){leave_call();return;}
    if(wifi.state!=ST_WIFI_READY){inline_error("Connect to Wi-Fi first.");return;}
    st_copy(call_channel,64,panels.channel);call_user[0]=0;call_generation++;call_force=call_conflict=call_auth_sent=0;call_pending=call_screen=1;call_mic_muted=call_deafened=0;call_cursor=call_controls=call_button=call_member_settings=call_joined=0;call_members=0;
    if(thread>=0&&!mutation(active.type))cancel_active();
    for(int i=0;i<queued;)if(!mutation(queue[i].type)){for(int k=i+1;k<queued;k++)queue[k-1]=queue[k];queued--;}else i++;
}
static unsigned call_visible(unsigned rows[ST_RTC_MEMBERS]){unsigned n=0;for(unsigned i=0;i<call_snapshot.member_count;i++)if(call_snapshot.members[i].connected)rows[n++]=i;return n;}
static void call_toggle_mic(void){if(call_deafened){call_deafened=call_mic_muted=0;st_call_deafen(0);st_call_microphone(0);st_audio_effect(ST_FX_UNDEAFEN);return;}call_mic_muted=!call_mic_muted;st_call_microphone(call_mic_muted);st_audio_effect(call_mic_muted?ST_FX_MUTE:ST_FX_UNMUTE);}
static void call_toggle_deafen(void){call_deafened=!call_deafened;st_call_deafen(call_deafened);st_audio_effect(call_deafened?ST_FX_DEAFEN:ST_FX_UNDEAFEN);}
static void call_input(unsigned b) {
    if(call_conflict&&!st_call_busy()&&!call_pending){if(b&ui_cancel_button())leave_call();else if(b&ui_confirm_button()){voice_listen();call_force=1;}return;}
    unsigned rows[ST_RTC_MEMBERS],n=call_visible(rows);if(call_cursor>=(int)n)call_cursor=n?(int)n-1:0;
    if(call_member_settings&&n) {
        StRtcMember *m=call_snapshot.members+rows[call_cursor];int volume=m->volume;
        if(b&PSP_CTRL_LEFT)volume-=10;
        if(b&PSP_CTRL_RIGHT)volume+=10;
        if(b&PSP_CTRL_SQUARE){volume=volume?0:st_call_restore_volume(m->user);st_audio_effect(volume?ST_FX_UNMUTE:ST_FX_MUTE);}
        if(volume!=m->volume)st_call_volume(m->user,volume);
        if(b&(ui_cancel_button()|ui_confirm_button()))call_member_settings=0;
        return;
    }
    if(b&ui_cancel_button()){leave_call();return;}
    if(b&PSP_CTRL_TRIANGLE)call_toggle_mic();
    if(b&PSP_CTRL_SQUARE)call_toggle_deafen();
    if(call_controls){if(b&PSP_CTRL_LEFT)call_button=(call_button+2)%3;if(b&PSP_CTRL_RIGHT)call_button=(call_button+1)%3;if(b&PSP_CTRL_UP)call_controls=0;}
    else {
        if(b&PSP_CTRL_LEFT&&call_cursor>0)call_cursor--;
        if(b&PSP_CTRL_RIGHT&&call_cursor+1<(int)n)call_cursor++;
        if(b&PSP_CTRL_UP&&call_cursor>=3)call_cursor-=3;
        if(b&PSP_CTRL_DOWN){if(call_cursor+3<(int)n)call_cursor+=3;else call_controls=1;}
        if(b&PSP_CTRL_LTRIGGER)call_cursor=call_cursor>=6?call_cursor-6:0;
        if(b&PSP_CTRL_RTRIGGER&&call_cursor+6<(int)n)call_cursor+=6;
    }
    if(b&ui_confirm_button()) {
        if(call_controls){if(call_button==0)call_toggle_mic();else if(call_button==1)call_toggle_deafen();else leave_call();}
        else if(n){if(!strcmp(call_snapshot.members[rows[call_cursor]].user,call_snapshot.self))call_toggle_mic();else {st_copy(call_settings_user,sizeof(call_settings_user),call_snapshot.members[rows[call_cursor]].user);call_member_settings=1;}}
    }
}
static void call_draw(void) {
    if(call_member_settings){unsigned rows[ST_RTC_MEMBERS],n=call_visible(rows);int found=0;for(unsigned i=0;i<n;i++)if(!strcmp(call_snapshot.members[rows[i]].user,call_settings_user)){call_cursor=(int)i;found=1;break;}if(!found)call_member_settings=0;}
    const StChannel *channel=NULL;for(size_t i=0;i<client.channel_count;i++)if(!strcmp(client.channels[i].id,call_channel))channel=client.channels+i;
    StCallView view={.stats=&call_snapshot,.client=&client,.channel=channel?st_channel_name(&client,channel):"Voice call",.status=call_pending?"Preparing voice...":panels.error[0]&&!st_call_busy()?panels.error:panels.call_status,.cursor=call_cursor,.controls=call_controls,.button=call_button,.member_settings=call_member_settings,.reconnect=call_conflict&&!st_call_busy()&&!call_pending};
    st_call_view_draw(&view,ui_confirm_label(),ui_cancel_label());
}
static void voice_text_chat(void) {
    const StChannel *ch=st_panels_current(&panels,&client);if(!ch||(!strcmp(ch->type,"VoiceChannel")))return;
    changed_view();st_panels_open_chat(&panels,&client,ch);panels.page=ST_CHAT;enqueue(ST_JOB_HISTORY,ch->id,NULL,NULL,0);
}
static void history_button(unsigned buttons) {
    char cursor[64]="";
    if((buttons&PSP_CTRL_SELECT)&&client.message_count)st_copy(cursor,sizeof(cursor),client.messages[client.message_count-1].id);
    enqueue(ST_JOB_HISTORY,panels.channel,cursor,NULL,0);
}
static void auth_action(void) {
    if(panels.page==ST_ONBOARD) {
        if(!panels.cursor)ui_osk("Username",panels.username,sizeof(panels.username),32);
        else enqueue(ST_JOB_ONBOARD,panels.username,NULL,NULL,0);
        return;
    }
    int signup=panels.page==ST_REGISTER,i=panels.cursor;
    if(i==0){ui_osk("Account email",panels.email,sizeof(panels.email),200);return;}
    if(i==1){ui_osk("Password",panels.password,sizeof(panels.password),200);return;}
    if(signup&&client.invite_only&&i==2){ui_osk("Invite code",panels.invite,sizeof(panels.invite),100);return;}
    i-=signup&&client.invite_only?1:0;
    if(i==3){panels.page=signup?ST_LOGIN:ST_REGISTER;panels.cursor=0;st_wipe(panels.password,sizeof(panels.password));changed_view();return;}
    if(i==4){app_running=0;return;}
    if(i!=2)return;
    if(client.token[0]){enqueue(ST_JOB_HELLO,NULL,NULL,NULL,0);return;}
    if(!client.discovered){connect_api();inline_error("Wait for server settings to load, then log in.");return;}
    if(!panels.email[0]||!panels.password[0]){inline_error("Enter your email and password.");return;}
    if(enqueue(signup?ST_JOB_REGISTER:ST_JOB_LOGIN,panels.email,panels.password,panels.invite,0))st_wipe(panels.password,sizeof(panels.password));
}
static void account_action(void) {
    switch(panels.cursor) {
        case 0:st_panels_open_profile(&panels,client.self_id);changed_view();enqueue(ST_JOB_USER,panels.person,panels.profile_server,NULL,0);break;
        case 1:panels.page=ST_STATUS;panels.cursor=0;changed_view();break;
        case 2:panels.page=ST_SETTINGS;panels.cursor=0;changed_view();break;
        case 3:enqueue(ST_JOB_LOGOUT,NULL,NULL,NULL,0);break;
    }
}
static void settings_action(void) {
    if(panels.cursor==0){panels.page=ST_CACHE;panels.cursor=0;changed_view();enqueue(ST_JOB_CACHE_STATS,NULL,NULL,NULL,0);}
    else if(panels.cursor==1){panels.page=ST_APPEARANCE;panels.cursor=0;changed_view();}
    else if(panels.cursor==2){panels.page=ST_SESSIONS;panels.cursor=0;changed_view();enqueue(ST_JOB_SESSIONS,NULL,NULL,NULL,0);}
    else if(call_pending||st_call_busy())inline_error("Leave the voice channel before running a local audio test.");else st_voice_test_open();
}
static void open_person(const char *id){st_panels_open_profile(&panels,id);changed_view();enqueue(ST_JOB_USER,panels.person,panels.profile_server,NULL,0);}
static void profile_action(void) {
    const char *relation=st_str(client.profile_user,"relationship");
    switch(panels.cursor) {
        case 0:enqueue(ST_JOB_DM,panels.person,NULL,NULL,0);break;
        case 1:if(confirm_text("Friendship",!strcmp(relation,"Friend")?"Remove this friend?":!strcmp(relation,"Outgoing")?"Cancel this request?":!strcmp(relation,"Incoming")?"Accept this friend request?":"Send a friend request?"))enqueue(ST_JOB_USER_ACTION,panels.person,!strcmp(relation,"Friend")||!strcmp(relation,"Outgoing")?"unfriend":"friend",NULL,0);break;
        case 2:if(confirm_text("Block user",!strcmp(relation,"Blocked")?"Unblock this user?":"Block this user?"))enqueue(ST_JOB_USER_ACTION,panels.person,!strcmp(relation,"Blocked")?"unblock":"block",NULL,0);break;
        case 3:panels.page=ST_REPORT;panels.cursor=0;changed_view();break;
        case 4:panels.page=ST_MUTUAL_SERVERS;panels.cursor=0;changed_view();break;
        case 5:panels.page=ST_MUTUAL_CHATS;panels.cursor=0;changed_view();break;
    }
}
static void report_action(void) {
    static const char *reasons[]={"UnsolicitedSpam","SpamAbuse","InappropriateProfile","Impersonation","BanEvasion","Underage","NoneSpecified"};char text[ST_TEXT_MAX]="";
    if(ui_osk("Report description",text,sizeof(text),2000)&&confirm_text("Send report?",text[0]?text:"Send this report to server moderators?"))enqueue(ST_JOB_REPORT,panels.person,text,reasons[panels.cursor],0);
    st_wipe(text,sizeof(text));
}
static void session_action(void) {
    int all=panels.cursor<2?panels.cursor+1:0;const char *id=all?"":client.sessions[panels.cursor-2].id;
    if(!confirm_text("Revoke sessions?",all==2?"Revoke every session, including this PSP?":all==1?"Revoke every other session? This PSP stays logged in.":"Revoke the selected session?"))return;
    char password[512]="";if(ui_osk("Confirm account password",password,sizeof(password),200)&&password[0])enqueue(ST_JOB_REVOKE,id,password,NULL,all);st_wipe(password,sizeof(password));
}
static void mutual_action(void) {
    int servers=panels.page==ST_MUTUAL_SERVERS;const cJSON *entry=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(client.mutual,servers?"servers":"channels"),panels.cursor);if(!cJSON_IsString(entry))return;
    if(servers){for(size_t i=0;i<client.server_count;i++)if(!strcmp(client.servers[i].id,entry->valuestring)){panels.rail_cursor=(int)i+1;st_panels_select_rail(&panels,&client);changed_view();return;}}
    else {for(size_t i=0;i<client.channel_count;i++)if(!strcmp(client.channels[i].id,entry->valuestring)){st_copy(panels.server,sizeof(panels.server),client.channels[i].server);panels.rail_cursor=0;for(size_t s=0;s<client.server_count;s++)if(!strcmp(client.servers[s].id,panels.server))panels.rail_cursor=(int)s+1;panels.nav_tab=0;open_chat((int)i);return;}}
    inline_error("This item is not in the loaded channel list.");
}
static void appearance_action(void) {
    st_theme_apply(panels.cursor?st_theme_dark:!st_theme_dark,panels.cursor?panels.cursor-1:st_theme_accent);
    panels.error[0]=0;if(!st_theme_save(ST_THEME_SETTINGS,&error))inline_error(error.text);
}
static void image_back(void){char id[64];st_copy(id,sizeof(id),panels.media_requested);st_panels_back(&panels);changed_view();st_copy(panels.media_requested,sizeof(panels.media_requested),id);}
static void request_attachment(void) {
    if(worker_stalled){st_copy(panels.media_error,sizeof(panels.media_error),"Image is still loading. Try again shortly.");return;}
    if(strcmp(panels.channel,client.messages_channel)||panels.message<0||(size_t)panels.message>=client.message_count)return;
    const StMessage *m=client.messages+client.message_count-1-(size_t)panels.message;
    if(panels.cursor<0||panels.cursor>=m->attachments||panels.cursor>=ST_ATTACHMENT_MAX)return;
    const StAttachment *a=m->files+panels.cursor;if(!a->image)return;
    if(!panels.media_error[0]&&!strcmp(panels.media_requested,a->id)&&ui_image_state("attachments",a->id)==3){char id[64];st_copy(id,sizeof(id),a->id);changed_view();st_copy(panels.media_requested,sizeof(panels.media_requested),id);panels.page=ST_IMAGE;return;}
    panels.media_error[0]=0;st_copy(panels.media_requested,sizeof(panels.media_requested),a->id);
    /* A failed preview is retried only after this explicit action. */
    for(int i=0;i<ST_MEDIA_SLOTS;i++){StMediaSlot *s=media_cache.slots+i;if(s->state==4&&!strcmp(s->key.tag,"attachments")&&!strcmp(s->key.id,a->id))s->retry=0;}
}
static void media_button(void) {
    if(panels.page==ST_IMAGE)return;
    if(panels.focus!=ST_CONTENT){refresh();return;}
    if(panels.page==ST_DETAIL&&client.message_count) {
        StInviteLink links[ST_INVITE_LINK_MAX];if(st_panels_own_message(&panels,&client)||st_panels_invites(&panels,&client,links)){const StMessage *m=client.messages+client.message_count-1-(size_t)panels.message;panels.page=ST_MESSAGE_ACTIONS;panels.cursor=m->attachments||m->embeds?0:st_panels_own_message(&panels,&client)?2:1;changed_view();return;}
        const StMessage *m=client.messages+client.message_count-1-(size_t)panels.message;
        if(m->attachments||m->embeds){panels.page=ST_ATTACHMENTS;panels.cursor=0;changed_view();}return;
    }
    if(panels.page==ST_CHAT){panels.message=panels.cursor;changed_view();panels.page=ST_CHAT_ACTIONS;panels.cursor=0;return;}
    if(panels.page==ST_ATTACHMENTS){changed_view();return;}
    refresh();
}
static void status_action(void) {
    const cJSON *status=cJSON_GetObjectItemCaseSensitive(client.self,"status");char text[513];st_copy(text,sizeof(text),st_str(status,"text"));
    static const char *values[]={"Online","Idle","Focus","Busy","Invisible"};const char *presence=st_str(status,"presence");if(!*presence)presence="Online";
    if(!panels.cursor){if(ui_osk("Status text",text,sizeof(text),128))enqueue(ST_JOB_STATUS,text,presence,NULL,0);}
    else if(panels.cursor==6)enqueue(ST_JOB_STATUS,"",presence,NULL,0);
    else enqueue(ST_JOB_STATUS,text,values[panels.cursor-1],NULL,0);
}
static void react_selected(void) {
    if(!client.message_count||panels.message<0||panels.message>=(int)client.message_count)return;
    const StMessage *m=client.messages+client.message_count-1-(size_t)panels.message;int emoji=st_reactions_at(panels.cursor);const StReaction *r=st_message_reaction_key(m,st_reactions_key(emoji));
    enqueue(ST_JOB_REACT,panels.channel,m->id,r?r->key:st_reactions_key(emoji),r&&r->mine);
}
static void social_action(void) {
    if(panels.page==ST_ADD_FRIEND) {
        if(!panels.cursor)ui_osk("Friend username#0000",panels.friend_tag,sizeof(panels.friend_tag),70);
        else enqueue(ST_JOB_FRIEND,panels.friend_tag,NULL,NULL,0);
        return;
    }
    if(panels.page==ST_REQUESTS) {
        int i=st_panels_request(&client,panels.cursor);if(i<0)return;st_copy(panels.person,64,client.users[i].id);panels.page=ST_REQUEST_ACTIONS;panels.cursor=0;changed_view();return;
    }
    if(panels.page==ST_REQUEST_ACTIONS) {
        for(size_t i=0;i<client.user_count;i++)if(!strcmp(client.users[i].id,panels.person)) {
            const char *r=client.users[i].relationship;if(!strcmp(r,"Incoming"))enqueue(ST_JOB_USER_ACTION,panels.person,panels.cursor?"unfriend":"friend",NULL,0);
            else if(!strcmp(r,"Outgoing"))enqueue(ST_JOB_USER_ACTION,panels.person,"unfriend",NULL,0);
            break;
        }
    }
}
static void invitation_action(void) {
    if(panels.page==ST_MESSAGE_ACTIONS&&panels.cursor==2){
        if(!st_panels_own_message(&panels,&client)){inline_error("You can only delete your own messages.");return;}
        char id[64];st_copy(id,sizeof(id),client.messages[client.message_count-1-(size_t)panels.message].id);
        if(confirm_text("Delete message","Delete this message for everyone? This cannot be undone."))enqueue(ST_JOB_DELETE_MESSAGE,panels.channel,id,NULL,0);
        return;
    }
    if(panels.page==ST_MESSAGE_ACTIONS) {panels.page=panels.cursor?ST_INVITE_LINKS:ST_ATTACHMENTS;panels.cursor=0;changed_view();return;}
    if(panels.page==ST_INVITE_LINKS) {
        StInviteLink links[ST_INVITE_LINK_MAX];int count=st_panels_invites(&panels,&client,links);if(panels.cursor<0||panels.cursor>=count)return;
        st_copy(panels.invite_code,64,links[panels.cursor].code);panels.page=ST_INVITE;panels.cursor=0;changed_view();enqueue(ST_JOB_INVITE,panels.invite_code,NULL,NULL,0);return;
    }
    if(panels.page==ST_INVITE&&client.invitation&&!strcmp(client.invitation_code,panels.invite_code)) {
        for(size_t i=0;i<client.server_count;i++)if(!strcmp(client.servers[i].id,st_str(client.invitation,"server_id"))){panels.rail_cursor=(int)i+1;st_panels_select_rail(&panels,&client);changed_view();return;}
        enqueue(ST_JOB_JOIN,panels.invite_code,NULL,NULL,0);
    }
}
static void notifications_open(const char *id,int server) {
    if(!id||!*id)return;
    panels.notify_origin=panels.page;panels.notify_focus=panels.focus;panels.notify_cursor=panels.cursor;
    st_copy(panels.notify_id,sizeof(panels.notify_id),id);panels.notify_server=server;
    changed_view();panels.page=ST_NOTIFICATIONS;panels.focus=ST_CONTENT;panels.cursor=(int)st_preferences_mode(&client,id,server);
    enqueue(ST_JOB_PREFS_FETCH,NULL,NULL,NULL,0);
}
static void photo_open(void) {
    if(!st_photos_sources(&photos,photo_device,&error)){inline_error(error.text);return;}
    changed_view();st_media_frame(&media_cache);panels.photos=&photos;panels.page=ST_PHOTOS;panels.focus=ST_CONTENT;panels.cursor=0;
}
static void photo_parent(void) {
    if(photos.sources){st_panels_back(&panels);changed_view();return;}
    int source=strstr(photos.root,"/PSP/SCREENSHOT")?2:strstr(photos.root,"/PSP/PHOTO")?1:0;
    int ok=!strcmp(photos.root,photos.path)?st_photos_sources(&photos,photo_device,&error):st_photos_parent(&photos,&error);
    if(ok){changed_view();st_media_frame(&media_cache);panels.cursor=photos.sources?source:0;}else inline_error(error.text);
}
static void photo_action(void) {
    if(photos.sources){if(st_photos_source(&photos,panels.cursor,&error)){changed_view();st_media_frame(&media_cache);panels.cursor=0;}else inline_error(error.text);return;}
    if(panels.cursor==0){photo_parent();return;}
    int row=panels.cursor-1;if(row<0||(size_t)row>=photos.count)return;
    if(photos.items[row].directory){if(st_photos_enter(&photos,row,&error)){changed_view();st_media_frame(&media_cache);panels.cursor=0;}else inline_error(error.text);return;}
    char path[800];if(!st_photos_file(&photos,row,path,sizeof(path),&error)){inline_error(error.text);return;}
    uint64_t bytes;const char *mime;if(!st_photo_info(path,&bytes,&mime,&error)){inline_error(error.text);return;}
    char text[500];snprintf(text,sizeof(text),"Send %s (%.2f MB) to this chat?",photos.items[row].name,(double)bytes/1048576.);
    if(confirm_text("Send photo",text))enqueue(ST_JOB_PHOTO,panels.channel,path,NULL,0);
}
static void chat_action(void) {
    if(panels.cursor==4){const StChannel *ch=st_panels_current(&panels,&client);if(ch&&!strcmp(ch->type,"Group")){char id[64];st_copy(id,sizeof(id),ch->id);if(confirm_text("Leave group","Leave this conversation? You will need to be added again to return."))enqueue(ST_JOB_LEAVE_GROUP,id,NULL,NULL,0);}return;}
    if(panels.cursor==0){photo_open();return;}
    if(panels.cursor==1){notifications_open(panels.channel,0);return;}
    if(panels.cursor==2){panels.page=ST_CHAT;panels.cursor=panels.message;changed_view();enqueue(ST_JOB_HISTORY,panels.channel,NULL,NULL,0);return;}
    changed_view();panels.page=ST_VOICE;panels.cursor=0;enqueue(ST_JOB_VOICE,panels.channel,NULL,NULL,0);
}
static void server_action(void) {
    if(!panels.cursor){enqueue(ST_JOB_VOICE,panels.channel,NULL,NULL,0);return;}
    if(panels.cursor==1){notifications_open(panels.server,1);return;}
    char id[64];st_copy(id,sizeof(id),panels.server);if(!id[0])return;
    for(size_t i=0;i<client.server_count;i++)if(!strcmp(client.servers[i].id,id)&&!strcmp(client.servers[i].owner,client.self_id)){inline_error("Transfer ownership before leaving your server.");return;}
    char text[400];const char *name=id;for(size_t i=0;i<client.server_count;i++)if(!strcmp(client.servers[i].id,id))name=client.servers[i].name;
    snprintf(text,sizeof(text),"Leave %s?\nYou will need an invitation to return.",name);
    if(confirm_text("Leave server",text))enqueue(ST_JOB_LEAVE,id,NULL,NULL,0);
}
static void activate(void) {
    if(panels.page==ST_LOGIN||panels.page==ST_REGISTER||panels.page==ST_ONBOARD){auth_action();return;}
    if(panels.focus==ST_RAIL){st_panels_select_rail(&panels,&client);changed_view();return;}
    if(panels.focus==ST_NAV){if(panels.server[0])open_chat(st_panels_channel(&panels,&client,panels.nav_cursor));else{st_panels_select_tab(&panels);changed_view();}return;}
    if(panels.focus==ST_MEMBERS) {
        if(st_panels_count(&panels,&client,ST_MEMBERS)){char id[64];st_copy(id,sizeof(id),client.participants[panels.member_cursor].id);open_person(id);}return;
    }
    if(panels.page==ST_ADD_FRIEND||panels.page==ST_REQUESTS||panels.page==ST_REQUEST_ACTIONS){social_action();return;}
    if(panels.page==ST_MESSAGE_ACTIONS||panels.page==ST_INVITE_LINKS||panels.page==ST_INVITE){invitation_action();return;}
    if(panels.page==ST_CHAT_ACTIONS){chat_action();return;}
    if(panels.page==ST_PHOTOS){photo_action();return;}
    if(panels.page==ST_NOTIFICATIONS){char mode[4];snprintf(mode,sizeof(mode),"%d",panels.cursor);enqueue(ST_JOB_PREFS_SET,panels.notify_id,mode,NULL,panels.notify_server);return;}
    if(panels.page==ST_SERVER_ACTIONS){server_action();return;}
    if(panels.page==ST_ACCOUNT){account_action();return;}
    if(panels.page==ST_SETTINGS){settings_action();return;}
    if(panels.page==ST_APPEARANCE){appearance_action();return;}
    if(panels.page==ST_IMAGE)return;
    if(panels.page==ST_ATTACHMENTS){request_attachment();return;}
    if(panels.page==ST_CACHE)return;
    if(panels.page==ST_STATUS){status_action();return;}
    if(panels.page==ST_SESSIONS){session_action();return;}
    if(panels.page==ST_PROFILE){if(!panels.loading&&!strcmp(client.profile_id,panels.person)&&strcmp(panels.person,client.self_id)){panels.page=ST_PROFILE_ACTIONS;panels.cursor=0;changed_view();}return;}
    if(panels.page==ST_PROFILE_ACTIONS){profile_action();return;}
    if(panels.page==ST_REPORT){report_action();return;}
    if(panels.page==ST_MUTUAL_SERVERS||panels.page==ST_MUTUAL_CHATS){mutual_action();return;}
    if(!ready)return;
    if(panels.page==ST_DMS){int i=st_panels_channel(&panels,&client,panels.cursor);if(i<0&&panels.cursor==0)enqueue(ST_JOB_DM,client.self_id,NULL,NULL,0);else open_chat(i);}
    else if(panels.page==ST_FRIENDS){if(panels.cursor<2){panels.page=panels.cursor?ST_REQUESTS:ST_ADD_FRIEND;panels.cursor=0;changed_view();return;}int i=st_panels_friend(&client,panels.cursor-2);if(i>=0){char id[64];st_copy(id,sizeof(id),client.users[i].id);open_person(id);}}
    else if(panels.page==ST_CHAT&&client.message_count){panels.message=panels.cursor;panels.page=ST_DETAIL;panels.scroll=0;changed_view();}
    else if(panels.page==ST_DETAIL&&client.message_count){const StMessage *m=client.messages+client.message_count-1-(size_t)panels.message;if(!m->system[0]){panels.page=ST_REACTIONS;panels.cursor=0;changed_view();}}
    else if(panels.page==ST_VOICE){const StVoiceMember *m=st_voice_member(&client,panels.channel,panels.cursor);if(m){char id[64];st_copy(id,sizeof(id),m->user);open_person(id);}}
    else if(panels.page==ST_REACTIONS)react_selected();
}
#ifndef ST_MAIN_TEST
static const char *boot_eboot_path(char *file) {
    /* Loadcore leaves an unresolved weak import as SYSCALL_NOT_LINKED or a
     * return-only stub. In that case use the normal launcher arguments. */
    const volatile uint32_t *stub=(const volatile uint32_t *)(uintptr_t)kuKernelInitFileName;
    if(stub[0]==0x0000054c||(stub[0]==0x03e00008&&stub[1]==0))return NULL;
    return kuKernelInitFileName(file)==0?file:NULL;
}

int main(int argc,char **argv) {
    int callback=sceKernelCreateThread("Callbacks",callbacks,0x11,4096,PSP_THREAD_ATTR_USER,NULL);if(callback>=0)sceKernelStartThread(callback,0,NULL);
    StConfig config;
    char boot_file[PATH_MAX]={0},cwd[512];StStorage storage;unsigned char nonce[16];
    /* The SDK's weak KUBridge import exposes the boot path on CFW even when
     * a launcher supplies no usable argv or working directory. */
    const char *boot=boot_eboot_path(boot_file);
    if(!st_storage_locate(&storage,boot,argc>0?argv[0]:NULL,getcwd(cwd,sizeof(cwd)),&error)||
       !st_config_load_assets(storage.config,storage.game,&config,&error)||!ui_init(config.font_file)) {
        pspDebugScreenInit();pspDebugScreenPrintf(ST_APP_NAME ": %s\nCheck install and NotoSans-Regular.ttf",error.text);while(app_running)sceKernelDelayThread(100000);sceKernelExitGame();return 1;
    }
    if(!st_savedata_prepare(&storage,nonce,&error)||chdir(storage.savedata)) {
        while(app_running){ui_begin();ui_title("Saved data error");ui_wrap(12,40,error.text[0]?error.text:"Cannot open SAVEDATA",0,9,UI_WHITE);ui_end();}
        ui_term();sceKernelExitGame();return 1;
    }
    const char *colon=strchr(storage.game,':');if(colon&&(size_t)(colon-storage.game)+2<=sizeof(photo_device)){memcpy(photo_device,storage.game,(size_t)(colon-storage.game));photo_device[colon-storage.game]=':';photo_device[colon-storage.game+1]=0;}
    st_client_init(&client,&config);st_panels_init(&panels);
    if(st_reactions_load(ST_REACTION_SETTINGS,&error)<0)inline_error(error.text);
    if(st_theme_load(ST_THEME_SETTINGS,&error)<0)inline_error(error.text);
    sceCtrlSetSamplingCycle(0);sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    scePowerSetClockFrequency(333,333,166);
    entropy_ok=entropy_init_savedata(nonce,&error);st_wipe(nonce,sizeof(nonce));if(!entropy_ok)inline_error(error.text);
    network_ok=network_init(&error);if(!network_ok)inline_error(error.text);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int saved=st_session_load(&client,"session.json",&error);if(saved<0)inline_error(error.text);
    if(saved>0){ready_session=1;panels.nav_tab=panels.nav_cursor=0;st_panels_select_tab(&panels);}
    wifi_init(&wifi);if(network_ok)wifi_poll(&wifi,st_millis(),1);
    connect_api();
    while(app_running) {
        tick();st_panels_reconcile(&panels,&client);st_panels_animate(&panels,&client,st_millis());
        st_media_frame(&media_cache);ui_media_cache(&media_cache);
        ui_begin();if(call_screen)call_draw();else st_panels_draw(&panels,&client,ui_confirm_label(),ui_cancel_label());ui_end();unsigned b=ui_buttons();
        if(call_screen){call_input(b);continue;}
        if(panels.page==ST_IMAGE){if(b&ui_cancel_button())image_back();continue;}
        st_panels_scroll_chat(&panels,&client,ui_analog_y(),st_millis());
        int auth=panels.page==ST_LOGIN||panels.page==ST_REGISTER||panels.page==ST_ONBOARD;
        if(!auth&&panels.page!=ST_PHOTOS && (b&(PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER))) {
            enum StPane previous=panels.focus;st_panels_focus(&panels,b&PSP_CTRL_LTRIGGER?-1:1);
            if(previous==ST_CONTENT&&panels.focus==ST_MEMBERS)enqueue(ST_JOB_MEMBERS,panels.channel,NULL,NULL,0);
            if(previous==ST_MEMBERS&&panels.focus!=ST_MEMBERS)close_members();
            continue;
        }
        if(b&ui_cancel_button()) {
            if(panels.page==ST_PHOTOS&&panels.focus==ST_CONTENT){photo_parent();continue;}
            if(!auth&&thread>=0&&!mutation(active.type)&&active.type!=ST_JOB_MEDIA){cancel_active();panels.loading=panels.lists_loading=0;}
            if(auth){if(panels.page==ST_REGISTER){panels.page=ST_LOGIN;panels.cursor=0;st_wipe(panels.password,sizeof(panels.password));changed_view();}else{cancel_active();panels.loading=0;}}
            else{enum StPage previous=panels.page;enum StPane focus=panels.focus;st_panels_back(&panels);if(previous!=panels.page)changed_view();if(focus==ST_MEMBERS)close_members();}
            continue;
        }
        int media_cursor=panels.cursor;
        if(panels.focus==ST_CONTENT&&panels.page==ST_PHOTOS){st_panels_photo_move(&panels,(b&PSP_CTRL_LEFT?-1:0)+(b&PSP_CTRL_RIGHT?1:0),(b&PSP_CTRL_UP?-1:0)+(b&PSP_CTRL_DOWN?1:0),(b&PSP_CTRL_START?1:0)+(b&PSP_CTRL_SELECT?-1:0));}
        else if(panels.focus==ST_CONTENT&&panels.page==ST_REACTIONS){st_panels_emoji_move(&panels,(b&PSP_CTRL_LEFT?-1:0)+(b&PSP_CTRL_RIGHT?1:0),(b&PSP_CTRL_UP?-1:0)+(b&PSP_CTRL_DOWN?1:0),(b&PSP_CTRL_START?1:0)+(b&PSP_CTRL_SELECT?-1:0));}
        else {if(b&PSP_CTRL_UP)st_panels_move(&panels,&client,-1);if(b&PSP_CTRL_DOWN)st_panels_move(&panels,&client,1);}
        if(panels.page==ST_PHOTOS&&media_cursor/ST_PHOTO_PAGE!=panels.cursor/ST_PHOTO_PAGE){changed_view();st_media_frame(&media_cache);}
        if(panels.page==ST_ATTACHMENTS&&media_cursor!=panels.cursor){panels.scroll=0;changed_view();}
        if(panels.focus==ST_CONTENT&&panels.page==ST_ATTACHMENTS){if((b&PSP_CTRL_RIGHT)&&panels.scroll<100)panels.scroll++;if((b&PSP_CTRL_LEFT)&&panels.scroll>0)panels.scroll--;}
        if(b&ui_confirm_button())activate();
        if(b&PSP_CTRL_SQUARE){if(panels.focus==ST_CONTENT&&panels.page==ST_VOICE)voice_listen();else media_button();}
        if((b&PSP_CTRL_TRIANGLE)&&panels.focus==ST_CONTENT&&panels.page==ST_DMS){int i=st_panels_channel(&panels,&client,panels.cursor);if(i>=0&&strcmp(client.channels[i].type,"SavedMessages"))notifications_open(client.channels[i].id,0);continue;}
        if((b&PSP_CTRL_TRIANGLE)&&panels.focus<=ST_NAV) {
            if(panels.focus==ST_RAIL&&panels.rail_cursor>0){int selected=panels.rail_cursor;st_panels_select_rail(&panels,&client);panels.rail_cursor=selected;}
            if(panels.server[0]){panels.page=ST_SERVER_ACTIONS;panels.focus=ST_CONTENT;panels.cursor=0;changed_view();continue;}
        }
        if(panels.focus==ST_CONTENT&&panels.page==ST_VOICE&&(b&PSP_CTRL_TRIANGLE)){voice_text_chat();continue;}
        if(panels.focus==ST_CONTENT&&(panels.page==ST_CHAT||panels.page==ST_DETAIL)) {
            if(b&PSP_CTRL_TRIANGLE)compose();
            if(b&PSP_CTRL_RIGHT)reply_selected();
            if((b&PSP_CTRL_LEFT)&&client.message_count){char id[64];const StMessage *m=client.messages+client.message_count-1-(size_t)(panels.page==ST_DETAIL?panels.message:panels.cursor);if(!m->system[0]){st_copy(id,sizeof(id),m->author);open_person(id);}}
            if(b&(PSP_CTRL_START|PSP_CTRL_SELECT))history_button(b);
        }
    }
    /* HOME and close do not join an in-flight syscall or reuse its heap. The
     * firmware tears down this process; the worker alone owns its resources. */
    if(worker_thread>=0||st_call_busy()){st_call_stop();cancel_active();sceKernelExitGame();return 0;}
    for(int i=0;i<queued;i++)st_wipe(queue+i,sizeof(queue[i]));
    st_wipe(draft,sizeof(draft));st_wipe(&panels,sizeof(panels));
    st_photos_clear(&photos);st_audio_term();st_notices_clear(&live.notices);st_media_clear(&media_cache);st_client_clear(&client);st_transport_reset();curl_global_cleanup();network_term();if(entropy_ok)entropy_term();ui_term();sceKernelExitGame();return 0;
}

#endif
