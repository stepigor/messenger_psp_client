/* Only the network owner touches TLS/SRTP. The microphone publishes encoded
 * packets through a bounded SPSC queue; the speaker consumes a PCM FIFO.
 * UI never joins native capture/output or network workers. */
#include "call.h"
#include "voice_codec.h"
#include "live.h"
#include <pspkernel.h>
#include <pspaudio.h>
#include <opus/opus.h>
#include <stdio.h>
#include <string.h>
typedef struct {unsigned length,generation;uint64_t created;unsigned char bytes[1275];} MicPacket;
static struct {
    StConfig config;StCallTicket endpoint;char channel[64],user[64];StRtc *rtc;StRtcStats stats;char failure[128];
    int worker,audio,microphone,lock,output_channel,loopback;volatile int stop,done,audio_done,mic_done,token_ready,mic_muted,deafen,input_pending;
    volatile unsigned mic_peak,mic_level,mic_generation,write_packet,read_packet,loop_write,loop_read;unsigned underruns,capture_blocks,encoded,mic_dropped,max_encode_us;uint64_t encode_us;
    MicPacket packets[16];int16_t loop_pcm[8192];
    struct {char user[64];int volume,restore;} volumes[ST_RTC_MEMBERS];unsigned volume_count,revision;
} call={.worker=-1,.audio=-1,.microphone=-1,.lock=-1,.output_channel=-1};
static int16_t output[2][2048] __attribute__((aligned(64)));
static int16_t capture[2][1024] __attribute__((aligned(64)));
static void lock(void){sceKernelWaitSema(call.lock,1,NULL);}
static void unlock(void){sceKernelSignalSema(call.lock,1);}
static size_t loop_read(int16_t *out,size_t wanted){unsigned at=call.loop_read,w=call.loop_write;__sync_synchronize();size_t n=w-at;if(n>wanted)n=wanted;for(size_t i=0;i<n;i++)out[i]=call.loop_pcm[(at+i)&8191];__sync_synchronize();call.loop_read=at+(unsigned)n;return n;}
static void loop_write(const int16_t *in,size_t n){unsigned w=call.loop_write;if(n>8192-(w-call.loop_read))return;for(size_t i=0;i<n;i++)call.loop_pcm[(w+i)&8191]=in[i];__sync_synchronize();call.loop_write=w+(unsigned)n;}
static int audio_worker(SceSize size,void *argument) {
    (void)size;(void)argument;int channel=sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL,1024,PSP_AUDIO_FORMAT_STEREO);
    if(channel>=0) {
        call.output_channel=channel;unsigned phase=0,buffer=0;int16_t source[512],a=0,b=0;size_t at=0,n=0;
        while(!call.stop) {
            int16_t *p=output[buffer];buffer^=1;
            for(unsigned i=0;i<1024;i++) {
                int value=a+(int)(((int64_t)b-a)*phase/44100);if(call.deafen)value=0;p[i*2]=p[i*2+1]=(int16_t)value;phase+=16000;
                if(phase>=44100){phase-=44100;a=b;if(at==n){n=call.loopback?loop_read(source,512):st_rtc_pcm(call.rtc,source,512);at=0;if(!n)call.underruns++;}b=at<n?source[at++]:0;}
            }
            if(sceAudioOutput(channel,PSP_AUDIO_VOLUME_MAX,p)<0){st_call_error("Voice audio output failed.");break;}
            unsigned waits=0;int remaining;
            while((remaining=sceAudioGetChannelRestLen(channel))>0&&!call.stop&&waits++<250)sceKernelDelayThread(1000);
            if(remaining<0||waits>=250){st_call_error("Voice audio output timed out.");break;}
        }
        for(unsigned i=0;i<100&&sceAudioGetChannelRestLen(channel)>0;i++)sceKernelDelayThread(1000);
        if(sceAudioGetChannelRestLen(channel)==0&&sceAudioChRelease(channel)>=0)call.output_channel=-1;
    }else st_call_error("No audio channel available.");
    __sync_synchronize();call.audio_done=1;return 0;
}
static int microphone_worker(SceSize size,void *argument) {
    (void)size;(void)argument;int error;OpusEncoder *encoder=call.loopback?NULL:opus_encoder_create(ST_VOICE_RATE,1,OPUS_APPLICATION_VOIP,&error);
    if(!call.loopback&&(!encoder||opus_encoder_ctl(encoder,OPUS_SET_BITRATE(ST_VOICE_BITRATE))||opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(ST_VOICE_COMPLEXITY))||opus_encoder_ctl(encoder,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))||opus_encoder_ctl(encoder,OPUS_SET_DTX(0)))){st_call_error("Microphone encoder unavailable.");goto end;}
    if(sceAudioInputInit(0,4096,0)<0){st_call_error("Microphone initialization failed.");goto end;}
    StVoiceResampler resampler={0};int16_t samples[1024],pending[1280];size_t used=0;unsigned buffer=0,generation=call.mic_generation;
    while(!call.stop) {
        if(call.mic_muted||call.deafen){call.mic_peak=0;sceKernelDelayThread(2000);continue;}
        if(!call.input_pending){if(sceAudioInput(1024,44100,capture[buffer])<0){st_call_error("Microphone capture failed.");break;}call.input_pending=1;}
        uint64_t deadline=(uint64_t)sceKernelGetSystemTimeWide()+1000000;int result;
        while((result=sceAudioPollInputEnd())>0&&(uint64_t)sceKernelGetSystemTimeWide()<deadline)sceKernelDelayThread(1000);
        if(result!=0){st_call_error("Microphone capture timed out.");break;}call.input_pending=0;
        if(call.stop)break;unsigned completed=buffer;buffer^=1;
        if(call.mic_muted||call.deafen||generation!=call.mic_generation){generation=call.mic_generation;memset(&resampler,0,sizeof(resampler));used=0;if(encoder)opus_encoder_ctl(encoder,OPUS_RESET_STATE);continue;}
        /* Arm the next firmware block before resampling/encoding this one. */
        if(sceAudioInput(1024,44100,capture[buffer])<0){st_call_error("Microphone capture failed.");break;}call.input_pending=1;call.capture_blocks++;
        unsigned peak=0;uint64_t energy=0;for(unsigned i=0;i<1024;i++){int v=capture[completed][i];energy+=(uint64_t)((int64_t)v*v);if(v<0)v=-v;if((unsigned)v>peak)peak=(unsigned)v;}call.mic_peak=peak;call.mic_level=(unsigned)(energy/1024);
        size_t n=st_voice_stream(&resampler,capture[completed],1024,samples,1024);
        if(call.loopback){loop_write(samples,n);continue;}
        if(n>1280-used){st_call_error("Microphone sample queue overflow.");break;}memcpy(pending+used,samples,n*2);used+=n;
        while(used>=ST_VOICE_FRAME&&!call.stop) {
            unsigned char packet[1275];uint64_t start=(uint64_t)sceKernelGetSystemTimeWide();int bytes=opus_encode(encoder,pending,ST_VOICE_FRAME,packet,sizeof(packet));unsigned elapsed=(unsigned)((uint64_t)sceKernelGetSystemTimeWide()-start);call.encode_us+=elapsed;if(elapsed>call.max_encode_us)call.max_encode_us=elapsed;call.encoded++;
            if(bytes<=0){st_call_error("Microphone encoding failed.");break;}
            unsigned w=call.write_packet;if(w-call.read_packet<16){MicPacket *p=call.packets+(w&15);memcpy(p->bytes,packet,(size_t)bytes);p->length=(unsigned)bytes;p->generation=generation;p->created=(uint64_t)sceKernelGetSystemTimeWide()/1000;__sync_synchronize();call.write_packet=w+1;}else call.mic_dropped++;
            used-=ST_VOICE_FRAME;memmove(pending,pending+ST_VOICE_FRAME,used*2);
        }
    }
end:
    if(encoder)opus_encoder_destroy(encoder);call.mic_peak=0;
    if(call.input_pending){for(unsigned i=0;i<100&&sceAudioPollInputEnd()!=0;i++)sceKernelDelayThread(1000);if(sceAudioPollInputEnd()==0)call.input_pending=0;}
    __sync_synchronize();call.mic_done=1;return 0;
}
static void report(void){FILE *f=fopen("calldiag.txt","wb");if(!f)return;StRtcStats *s=&call.stats;fprintf(f,ST_APP_NAME " %s\nState %d\nRTP %u decoded %u concealed %u late %u auth-errors %u PCM-dropped %u mixed-clipped %u\nSTUN requests %u replies %u DTLS %u signals %u\nMic sent %u dropped %u queued-dropped %u encoded %u blocks %u max-encode %u us total-encode %llu us\nOutput underflows %u\nError %s\n",ST_APP_VERSION,s->state,s->received,s->decoded,s->concealed,s->late,s->auth_errors,s->pcm_dropped,s->mix_clipped,s->stun_requests,s->stun_replies,s->dtls_steps,s->signals,s->mic_packets,s->mic_dropped,call.mic_dropped,call.encoded,call.capture_blocks,call.max_encode_us,(unsigned long long)call.encode_us,call.underruns,s->error);fclose(f);}
static void start_audio_threads(int microphone) {
    if(call.audio<0){call.audio_done=0;call.audio=sceKernelCreateThread(ST_APP_NAME " voice audio",audio_worker,0x12,32*1024,PSP_THREAD_ATTR_USER,NULL);if(call.audio<0||sceKernelStartThread(call.audio,0,NULL)<0){if(call.audio>=0)sceKernelDeleteThread(call.audio);call.audio=-1;call.audio_done=1;st_call_error("Cannot start voice audio.");}}
    if(microphone&&call.microphone<0&&!call.stop){call.mic_done=0;call.microphone=sceKernelCreateThread(ST_APP_NAME " microphone",microphone_worker,0x15,64*1024,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);if(call.microphone<0||sceKernelStartThread(call.microphone,0,NULL)<0){if(call.microphone>=0)sceKernelDeleteThread(call.microphone);call.microphone=-1;call.mic_done=1;st_call_error("Cannot start microphone.");}}
}
static int network_worker(SceSize size,void *argument) {
    (void)size;(void)argument;StError e={0};unsigned revision=0;int priority_raised=0;uint64_t snapshot_at=0,presence_at=0;enum StRtcState old_state=ST_RTC_CLOSED;
    if(!call.loopback)call.rtc=st_rtc_new(&call.config,call.endpoint.url,call.endpoint.address,call.user,&e);
    if(!call.rtc&&!call.loopback){lock();call.stats.state=ST_RTC_FAILED;st_copy(call.stats.error,sizeof(call.stats.error),e.text);unlock();}
    while((call.rtc||call.loopback)&&!call.stop) {
        if(call.loopback){lock();call.stats.state=ST_RTC_LISTEN;call.stats.publish_ready=1;unlock();start_audio_threads(1);sceKernelDelayThread(1000);continue;}
        if(call.token_ready){lock();int ok=st_rtc_authorize(call.rtc,call.endpoint.token);st_wipe(call.endpoint.token,sizeof(call.endpoint.token));call.token_ready=0;unlock();if(!ok)break;}
        lock();st_rtc_controls(call.rtc,call.mic_muted,call.deafen);if(revision!=call.revision){for(unsigned i=0;i<call.volume_count;i++)st_rtc_volume(call.rtc,call.volumes[i].user,call.volumes[i].volume);revision=call.revision;}unlock();
        uint64_t presence_now=(uint64_t)sceKernelGetSystemTimeWide()/1000;
        if(presence_now>=presence_at){presence_at=presence_now+20;if(!st_ws_presence_step(&e)){st_call_error(e.text);break;}}
        if(!st_rtc_step(call.rtc))break;
        unsigned at=call.read_packet,w=call.write_packet;__sync_synchronize();while(at!=w){MicPacket *p=call.packets+(at&15);if(p->generation==call.mic_generation&&(uint64_t)sceKernelGetSystemTimeWide()/1000-p->created<=120&&!call.mic_muted&&!call.deafen)st_rtc_send_opus(call.rtc,p->bytes,p->length);at++;}__sync_synchronize();call.read_packet=at;
        uint64_t now=(uint64_t)sceKernelGetSystemTimeWide()/1000;enum StRtcState state=st_rtc_state(call.rtc);
        if(now>=snapshot_at||state!=old_state){StRtcStats stats=st_rtc_stats(call.rtc);for(unsigned i=0;i<stats.member_count;i++)if(!strcmp(stats.members[i].user,stats.self)){stats.members[i].speaking=call.mic_level>22500&&!call.mic_muted&&!call.deafen;stats.members[i].level=call.mic_peak;}lock();call.stats=stats;unlock();snapshot_at=now+40;old_state=state;}
        if(state==ST_RTC_LISTEN&&!priority_raised){sceKernelChangeThreadPriority(sceKernelGetThreadId(),0x1b);priority_raised=1;}
        if(state==ST_RTC_LISTEN)start_audio_threads(st_rtc_publishing(call.rtc));sceKernelDelayThread(1000);
    }
    if(!call.loopback)st_ws_live_close();
    if(call.rtc){StRtcStats stats=st_rtc_stats(call.rtc);st_rtc_close(call.rtc);lock();call.stats=stats;unlock();}
    lock();if(call.failure[0]){call.stats.state=ST_RTC_FAILED;st_copy(call.stats.error,sizeof(call.stats.error),call.failure);}else if(call.stop)call.stats.state=ST_RTC_CLOSED;unlock();call.stop=1;
    while(!call.audio_done||!call.mic_done)sceKernelDelayThread(1000);
    if(call.audio>=0){sceKernelWaitThreadEnd(call.audio,NULL);sceKernelDeleteThread(call.audio);call.audio=-1;}if(call.microphone>=0){sceKernelWaitThreadEnd(call.microphone,NULL);sceKernelDeleteThread(call.microphone);call.microphone=-1;}
    st_rtc_free(call.rtc);call.rtc=NULL;st_wipe(call.endpoint.token,sizeof(call.endpoint.token));st_wipe(call.packets,sizeof(call.packets));st_wipe(call.loop_pcm,sizeof(call.loop_pcm));report();__sync_synchronize();call.done=1;return 0;
}
void st_call_poll(void) {
    if(call.worker>=0&&call.done){SceKernelThreadInfo info;memset(&info,0,sizeof(info));info.size=sizeof(info);if(sceKernelReferThreadStatus(call.worker,&info)>=0&&(info.status&PSP_THREAD_STOPPED)){sceKernelDeleteThread(call.worker);call.worker=-1;sceKernelDeleteSema(call.lock);call.lock=-1;}}
    if(call.worker<0&&call.output_channel>=0&&sceAudioGetChannelRestLen(call.output_channel)==0&&sceAudioChRelease(call.output_channel)>=0)call.output_channel=-1;
    if(call.worker<0&&call.input_pending&&sceAudioPollInputEnd()==0)call.input_pending=0;
}
static int start(const StConfig *config,const StCallTicket *endpoint,const char *channel,const char *user,int loopback) {
    st_call_poll();if(call.worker>=0||call.output_channel>=0||call.input_pending)return 0;
    call.lock=sceKernelCreateSema("Voice state",0,1,1,NULL);if(call.lock<0)return 0;
    call.config=*config;call.endpoint=*endpoint;st_copy(call.channel,64,channel);st_copy(call.user,64,user);memset(&call.stats,0,sizeof(call.stats));call.failure[0]=0;call.loopback=loopback;
    call.stop=call.done=call.token_ready=call.mic_muted=call.deafen=0;call.mic_generation++;call.audio_done=call.mic_done=1;call.audio=call.microphone=-1;call.volume_count=call.revision=0;
    call.write_packet=call.read_packet=call.loop_read=call.loop_write=call.underruns=call.capture_blocks=call.encoded=call.mic_dropped=call.max_encode_us=0;call.encode_us=0;
    call.worker=sceKernelCreateThread(ST_APP_NAME " voice network",network_worker,0x24,192*1024,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);
    if(call.worker<0||sceKernelStartThread(call.worker,0,NULL)<0){if(call.worker>=0)sceKernelDeleteThread(call.worker);call.worker=-1;sceKernelDeleteSema(call.lock);call.lock=-1;return 0;}return 1;
}
int st_call_start(const StConfig *c,const StCallTicket *t,const char *channel,const char *user){return start(c,t,channel,user,0);}
int st_call_loopback(void){StConfig c={0};StCallTicket t={0};return start(&c,&t,"Microphone loopback","",1);}
int st_call_busy(void){return call.worker>=0&&!call.done;}
int st_call_needs_token(void){return st_call_busy()&&!call.stop&&!call.token_ready&&!call.loopback&&st_call_snapshot().state==ST_RTC_AUTH;}
int st_call_token(const char *token){if(!st_call_needs_token()||strlen(token)>=sizeof(call.endpoint.token))return 0;lock();if(call.stop||call.token_ready||call.stats.state!=ST_RTC_AUTH){unlock();return 0;}st_copy(call.endpoint.token,sizeof(call.endpoint.token),token);__sync_synchronize();call.token_ready=1;unlock();return 1;}
void st_call_error(const char *message){if(call.lock>=0){lock();st_copy(call.failure,sizeof(call.failure),message);unlock();}call.stop=1;}
void st_call_stop(void){call.stop=1;__sync_synchronize();}
void st_call_microphone(int muted){if(call.lock>=0){lock();call.mic_muted=muted;call.mic_generation++;unlock();}}
void st_call_deafen(int deafened){if(call.lock>=0){lock();call.deafen=deafened;call.mic_generation++;unlock();}}
void st_call_volume(const char *user,int volume){if(call.lock<0)return;if(volume<0)volume=0;if(volume>200)volume=200;lock();unsigned i;for(i=0;i<call.volume_count;i++)if(!strcmp(call.volumes[i].user,user))break;if(i<ST_RTC_MEMBERS){if(i==call.volume_count){call.volume_count++;st_copy(call.volumes[i].user,64,user);call.volumes[i].restore=100;}call.volumes[i].volume=volume;if(volume)call.volumes[i].restore=volume;call.revision++;}unlock();}
int st_call_restore_volume(const char *user){int volume=100;if(call.lock>=0){lock();for(unsigned i=0;i<call.volume_count;i++)if(!strcmp(call.volumes[i].user,user))volume=call.volumes[i].restore;unlock();}return volume;}
unsigned st_call_mic_peak(void){return call.mic_peak;}
StRtcStats st_call_snapshot(void){StRtcStats s;if(call.lock>=0){lock();s=call.stats;unlock();}else s=call.stats;return s;}
const char *st_call_channel(void){return call.channel;}
const char *st_call_user(void){return call.user;}
