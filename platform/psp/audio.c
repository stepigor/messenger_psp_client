#include "audio.h"
#include <pspkernel.h>
#include <pspaudio.h>
#include <stdlib.h>
#include <string.h>
static StSound clip,effects[ST_FX_COUNT];static int thread=-1,channel=-1;static volatile int playing,pending,stop;
static int playback(SceSize n,void *p) {
    (void)n;(void)p;
    while(!stop) {
        if(!pending){sceKernelSleepThread();continue;}int chosen=pending;pending=0;playing=1;__sync_synchronize();
        const StSound *sound=chosen==1?&clip:effects+chosen-2;
        for(size_t at=0;at<sound->frames&&!stop&&!(pending>=2);at+=1024)if(sceAudioOutputBlocking(channel,PSP_AUDIO_VOLUME_MAX,sound->pcm+at*2)<0)break;
        playing=0;__sync_synchronize();
    }
    return 0;
}
static int prepare(void) {
    if(thread>=0)return 1;
    channel=sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL,1024,PSP_AUDIO_FORMAT_STEREO);if(channel<0)return 0;
    thread=sceKernelCreateThread(ST_APP_NAME " notification",playback,0x12,32*1024,PSP_THREAD_ATTR_USER,NULL);
    if(thread<0){sceAudioChRelease(channel);channel=-1;return 0;}
    stop=pending=playing=0;
    if(sceKernelStartThread(thread,0,NULL)<0){sceKernelDeleteThread(thread);thread=-1;sceAudioChRelease(channel);channel=-1;st_sound_free(&clip);return 0;}
    return 1;
}
void st_audio_install(StSound *sound){if(!clip.pcm&&sound->pcm&&prepare()){clip=*sound;memset(sound,0,sizeof(*sound));}}
void st_audio_install_effects(StSound bank[ST_FX_COUNT]){if(!prepare())return;for(int i=0;i<ST_FX_COUNT;i++)if(!effects[i].pcm){effects[i]=bank[i];memset(bank+i,0,sizeof(bank[i]));}}
void st_audio_effect(enum StEffect effect){if(effect>=0&&effect<ST_FX_COUNT&&effects[effect].pcm&&thread>=0){pending=(int)effect+2;__sync_synchronize();sceKernelWakeupThread(thread);}}
void st_audio_play(void){if(thread>=0&&!playing&&!pending){pending=1;__sync_synchronize();sceKernelWakeupThread(thread);}}
void st_audio_term(void) {
    if(thread>=0){stop=1;pending=0;__sync_synchronize();sceKernelWakeupThread(thread);sceKernelWaitThreadEnd(thread,NULL);sceKernelDeleteThread(thread);thread=-1;}
    if(channel>=0){sceAudioChRelease(channel);channel=-1;}st_sound_free(&clip);for(int i=0;i<ST_FX_COUNT;i++)st_sound_free(effects+i);
}
