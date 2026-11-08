#include "sound.h"
#include "disk_cache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vorbis/vorbisfile.h>
#ifdef __PSP__
#include <malloc.h>
#endif
typedef struct {const unsigned char *data;size_t length,at;} Input;
static size_t input_read(void *out,size_t size,size_t count,void *source) {
    Input *i=source;if(!size)return 0;size_t n=(i->length-i->at)/size;if(n>count)n=count;
    memcpy(out,i->data+i->at,n*size);i->at+=n*size;return n;
}
static int input_seek(void *source,ogg_int64_t offset,int origin) {
    Input *i=source;ogg_int64_t base=origin==SEEK_SET?0:origin==SEEK_CUR?(ogg_int64_t)i->at:origin==SEEK_END?(ogg_int64_t)i->length:-1;
    if(base<0||offset<-base||offset>(ogg_int64_t)i->length-base)return -1;
    i->at=(size_t)(base+offset);return 0;
}
static long input_tell(void *source){return (long)((Input *)source)->at;}
void st_sound_free(StSound *s){free(s->pcm);memset(s,0,sizeof(*s));}
static int decode_limit(const unsigned char *data,size_t n,unsigned seconds,StSound *out,StError *err) {
    memset(out,0,sizeof(*out));if(n<4||n>128*1024||memcmp(data,"OggS",4))return st_fail(err,"Invalid notification sound");
    Input input={data,n,0};OggVorbis_File vf;
    ov_callbacks cb={input_read,input_seek,NULL,input_tell};
    if(ov_open_callbacks(&input,&vf,NULL,0,cb)<0)return st_fail(err,"Invalid Ogg notification sound");
    vorbis_info *info=ov_info(&vf,-1);ogg_int64_t total=ov_pcm_total(&vf,-1);
    int ok=info&&ov_streams(&vf)==1&&(info->channels==1||info->channels==2)&&info->rate>=8000&&info->rate<=48000&&total>0&&total<=info->rate*(long)seconds;
    int channels=ok?info->channels:0;long rate=ok?info->rate:0;size_t bytes=ok?(size_t)total*(size_t)channels*2:0,at=0;
    int16_t *raw=ok?malloc(bytes):NULL;if(!raw)ok=0;
    while(ok&&at<bytes) {
        if(st_cancelled()){ok=0;break;}int section=0;size_t want=bytes-at;if(want>4096)want=4096;
        /* Request signed 16-bit little-endian PCM explicitly on both targets.
         * The SDK Tremor build clipped/corrupted samples on Allegrex. */
        long got=ov_read(&vf,(char *)raw+at,(int)want,0,2,1,&section);
        if(got<=0||section!=0){ok=0;break;}at+=(size_t)got;
    }
    ov_clear(&vf);
    if(ok) {
        size_t frames=(size_t)((uint64_t)total*ST_SOUND_RATE/(unsigned)rate);
        size_t size=(frames+1023)/1024*1024*2*sizeof(*out->pcm);
#ifdef __PSP__
        out->pcm=memalign(64,size);
#else
        out->pcm=malloc(size);
#endif
        if(!out->pcm)ok=0;else memset(out->pcm,0,size);
        if(ok){out->frames=frames;for(size_t f=0;f<frames;f++) {
            uint64_t pos=(uint64_t)f*(unsigned)rate;size_t a=(size_t)(pos/ST_SOUND_RATE),b=a+1<(size_t)total?a+1:a;int mix=(int)(pos%ST_SOUND_RATE);
            for(int c=0;c<2;c++){int ca=c<channels?c:0;int64_t value=(int64_t)raw[a*(size_t)channels+ca]*(ST_SOUND_RATE-mix)+(int64_t)raw[b*(size_t)channels+ca]*mix;out->pcm[f*2+c]=(int16_t)(value/ST_SOUND_RATE);}
        }}
    }
    free(raw);if(!ok){st_sound_free(out);return st_fail(err,st_cancelled()?"Cancelled":"Unsupported sound (Ogg Vorbis, mono/stereo, duration limit)");}return 1;
}
int st_sound_decode(const unsigned char *data,size_t n,StSound *out,StError *err){return decode_limit(data,n,2,out,err);}
int st_effect_decode(const unsigned char *data,size_t n,StSound *out,StError *err){return decode_limit(data,n,3,out,err);}
int st_sound_url(const char *app,char *out,size_t cap) {
    char host[256],path[512];if(!st_url_split(app,"https://",host,sizeof(host),path,sizeof(path))||strpbrk(app,"?#"))return 0;
    size_t n=strlen(app);while(n&&app[n-1]=='/')n--;
    return snprintf(out,cap,"%.*s/assets/sounds/message_sound.ogg",(int)n,app)<(int)cap;
}
static int fetch_url(const StClient *c,int offline,int effect,const char *url,StSound *out,StError *err) {
    memset(out,0,sizeof(*out));
    StDiskEntry entry;st_stage("Sound cache read");
    if(st_disk_read(ST_DISK_DIR,url,&entry)) {
        int ok=effect?st_effect_decode(entry.data,entry.length,out,err):st_sound_decode(entry.data,entry.length,out,err);st_disk_free(&entry);if(ok)return 1;
        if(st_cancelled())return 0;
        st_disk_remove(ST_DISK_DIR,url);
    }
    if(offline)return st_fail(err,"Notification sound is not cached");
    unsigned char *data=NULL;size_t n=0;
    int ok=st_download(&c->config,url,&data,&n,128*1024,5000,err);
    if(ok){st_stage("Sound decode");ok=effect?st_effect_decode(data,n,out,err):st_sound_decode(data,n,out,err);}
    if(ok){StError disk={0};st_disk_write(ST_DISK_DIR,url,data,n,NULL,&disk);}free(data);return ok;
}

int st_sound_fetch(const StClient *c,int offline,StSound *out,StError *err) {
    char url[1100];if(!st_sound_url(c->app_url,url,sizeof(url)))return st_fail(err,"No secure web sound endpoint");return fetch_url(c,offline,0,url,out,err);
}
int st_sound_fetch_effect(const StClient *c,enum StEffect effect,StSound *out,StError *err) {
    static const char *files[]={"mute","unmute","deafen","undeafen","user_join_voice","user_leave_voice"};
    if(effect<0||effect>=ST_FX_COUNT)return st_fail(err,"Unknown sound");
    char base[1100],url[1100];if(!st_sound_url(c->app_url,base,sizeof(base)))return st_fail(err,"No secure web sound endpoint");
    char *slash=strrchr(base,'/');if(!slash)return 0;*slash=0;
    if(snprintf(url,sizeof(url),"%s/%s.ogg",base,files[effect])>=(int)sizeof(url))return st_fail(err,"Sound URL limit");
    return fetch_url(c,0,1,url,out,err);
}
