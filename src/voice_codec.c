#include "voice_codec.h"
#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int16_t bounded(int64_t n){return n>32767?32767:n<-32768?-32768:(int16_t)n;}
uint32_t st_voice_output_frames(uint32_t samples,unsigned rate) {
    if(!rate)return 0;
    uint64_t frames=(uint64_t)samples*44100/rate;
    return frames>UINT32_MAX?0:(uint32_t)frames;
}
int16_t st_voice_sample(const int16_t *in,size_t count,size_t index,unsigned from,unsigned to) {
    if(!count||!from||!to)return 0;
    uint64_t p=(uint64_t)index*from;size_t a=(size_t)(p/to);unsigned mix=(unsigned)(p%to);
    if(a>=count)return in[count-1];
    size_t b=a+1<count?a+1:a;
    return bounded(((int64_t)in[a]*(to-mix)+(int64_t)in[b]*mix)/to);
}
size_t st_voice_downsample(const int16_t *in,size_t count,int16_t *out,size_t capacity) {
    if(!in||!out||!count)return 0;
    /* Hamming-windowed sinc, 6.5 kHz cutoff, Q15 unity DC gain. */
    static const int h[31]={54,26,-48,-145,-152,53,408,578,173,-787,-1599,-1178,1126,4806,8244,9650,8244,4806,1126,-1178,-1599,-787,173,578,408,53,-152,-145,-48,26,54};
    size_t n=count/44100*ST_VOICE_RATE+(count%44100)*ST_VOICE_RATE/44100;if(n>capacity)return 0;
    for(size_t i=0;i<n;i++) {
        uint64_t pos=(uint64_t)i*44100;size_t a=(size_t)(pos/ST_VOICE_RATE);unsigned mix=(unsigned)(pos%ST_VOICE_RATE);int64_t sum=0;
        for(int j=0;j<31;j++) {
            int64_t k=(int64_t)a+j-15;size_t x=k<0?0:(uint64_t)k>=count?count-1:(size_t)k,y=x+1<count?x+1:x;
            sum+=(int64_t)h[j]*((int64_t)in[x]*(ST_VOICE_RATE-mix)+(int64_t)in[y]*mix)/ST_VOICE_RATE;
        }
        out[i]=bounded(sum/32768);
    }
    return n;
}
size_t st_voice_stream(StVoiceResampler *s,const int16_t *input,size_t count,int16_t *output,size_t capacity) {
    static const int h[31]={54,26,-48,-145,-152,53,408,578,173,-787,-1599,-1178,1126,4806,8244,9650,8244,4806,1126,-1178,-1599,-787,173,578,408,53,-152,-145,-48,26,54};
    if(!s||!input||!output||!count)return 0;
    if(!s->initialized){for(unsigned i=0;i<15;i++)s->input[i]=input[0];s->count=15;s->position=15*16000;s->initialized=1;}
    if(count>4096-s->count)return 0;
    memcpy(s->input+s->count,input,count*2);s->count+=count;size_t written=0;
    while(written<capacity) {
        unsigned at=s->position/16000,mix=s->position%16000;if(at+16>=s->count)break;int64_t a=0,b=0;
        for(unsigned i=0;i<31;i++){a+=(int64_t)h[i]*s->input[at+i-15];b+=(int64_t)h[i]*s->input[at+i-14];}
        output[written++]=bounded((a*(16000-mix)+b*mix)/(16000LL*32768));s->position+=44100;
    }
    unsigned keep=s->position/16000;if(keep>15){unsigned drop=keep-15;if(drop>s->count)drop=(unsigned)s->count;memmove(s->input,s->input+drop,(s->count-drop)*2);s->count-=drop;s->position-=drop*16000;}
    return written;
}
static uint64_t now(const StVoiceHooks *h,int cpu){return h&&h->clock?h->clock(h->context,cpu):UINT64_MAX;}
static unsigned elapsed(uint64_t a,uint64_t b){return a==UINT64_MAX||b<a?0:b-a>UINT_MAX?UINT_MAX:(unsigned)(b-a);}
int st_voice_roundtrip(const int16_t *in,size_t count,int16_t *out,StVoiceTiming *t,const StVoiceHooks *h) {
    if(!t)return 0;
    memset(t,0,sizeof(*t));if(!in||!out||!count||count>ST_VOICE_RATE*10)return 0;
    int error;OpusEncoder *enc=opus_encoder_create(ST_VOICE_RATE,1,OPUS_APPLICATION_VOIP,&error);
    if(!enc)return 0;
    OpusDecoder *dec=opus_decoder_create(ST_VOICE_RATE,1,&error);int ok=dec!=NULL;opus_int32 lookahead=0;
    if(ok)ok=opus_encoder_ctl(enc,OPUS_SET_BITRATE(ST_VOICE_BITRATE))==OPUS_OK&&
        opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(ST_VOICE_COMPLEXITY))==OPUS_OK&&
        opus_encoder_ctl(enc,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))==OPUS_OK&&
        opus_encoder_ctl(enc,OPUS_SET_DTX(0))==OPUS_OK&&
        opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&lookahead))==OPUS_OK;
    t->lookahead=(int)lookahead;
    if(t->lookahead<0||t->lookahead>ST_VOICE_RATE)ok=0;
    size_t frames=(count+(size_t)t->lookahead+ST_VOICE_FRAME-1)/ST_VOICE_FRAME,written=0;
    for(size_t f=0;ok&&f<frames;f++) {
        if(h&&h->progress&&!h->progress(h->context,(unsigned)f,(unsigned)frames)){ok=-1;break;}
        int16_t input[ST_VOICE_FRAME]={0},output[ST_VOICE_FRAME];unsigned char packet[1276];
        size_t at=f*ST_VOICE_FRAME,n=at<count?count-at:0;if(n>ST_VOICE_FRAME)n=ST_VOICE_FRAME;
        if(n)memcpy(input,in+at,n*sizeof(*input));
        uint64_t start=now(h,0),cpu=now(h,1);
        int bytes=opus_encode(enc,input,ST_VOICE_FRAME,packet,sizeof(packet));
        uint64_t cpu2=now(h,1),middle=now(h,0),cpu3=now(h,1);
        int samples=bytes>0?opus_decode(dec,packet,bytes,output,ST_VOICE_FRAME,0):-1;
        uint64_t cpu4=now(h,1),end=now(h,0);
        if(bytes<=0||samples!=ST_VOICE_FRAME){ok=0;break;}
        unsigned encode=elapsed(start,middle),decode=elapsed(middle,end),wall=elapsed(start,end);
        t->encode_us+=encode;t->decode_us+=decode;t->frames++;t->bytes+=(unsigned)bytes;
        if(encode>t->max_encode_us)t->max_encode_us=encode;
        if(decode>t->max_decode_us)t->max_decode_us=decode;
        if(wall>t->max_frame_us)t->max_frame_us=wall;
        if(wall>20000)t->late_frames++;
        if(cpu!=UINT64_MAX&&cpu2!=UINT64_MAX&&cpu3!=UINT64_MAX&&cpu4!=UINT64_MAX&&cpu2>=cpu&&cpu4>=cpu3) {
            t->encode_cpu_us+=cpu2-cpu;t->decode_cpu_us+=cpu4-cpu3;t->cpu_frames++;
        }
        for(int i=0;i<samples;i++)if(at+(size_t)i>=(size_t)t->lookahead&&written<count)out[written++]=output[i];
    }
    if(dec)opus_decoder_destroy(dec);
    opus_encoder_destroy(enc);
    return ok==1&&written!=count?0:ok;
}
