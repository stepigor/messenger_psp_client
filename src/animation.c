#include "media.h"
#include <webp/demux.h>
#include <gif_lib.h>
#include <png.h>
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_FRAMES 120
#define SOURCE_SIDE 512

size_t st_image_bytes(const StImage *im){return (size_t)im->width*(size_t)im->height*4*(size_t)(im->frames?im->frames:1);}
const unsigned *st_image_pixels(const StImage *im,uint64_t now) {
    if(im->frames<2)return im->pixels;
    uint64_t total=0;for(int i=0;i<im->frames;i++)total+=im->delay[i];if(!total)return im->pixels;
    uint64_t elapsed=now>=im->started?now-im->started:0;
    int frame=im->frames-1;
    if(!im->loops||elapsed<total*im->loops){elapsed%=total;for(frame=0;frame<im->frames-1&&elapsed>=im->delay[frame];frame++)elapsed-=im->delay[frame];}
    return im->pixels+(size_t)frame*(size_t)im->width*(size_t)im->height;
}
static unsigned duration(unsigned ms){return ms<20?20:ms>10000?10000:ms;}
static int allocate(StImage *im,int w,int h,int maxw,int maxh,int count,unsigned loops,StError *err) {
    if(w<1||h<1||w>SOURCE_SIDE||h>SOURCE_SIDE||count<1||count>SOURCE_FRAMES||maxw<1||maxw>192||maxh<1||maxh>96)return st_fail(err,"Image exceeds animation limits");
    int dw=w,dh=h;if(dw>maxw){dh=dh*maxw/dw;dw=maxw;}if(dh>maxh){dw=dw*maxh/dh;dh=maxh;}if(dw<1)dw=1;if(dh<1)dh=1;
    int limit=maxw>32?8:ST_IMAGE_FRAMES;im->frames=count<limit?count:limit;im->width=dw;im->height=dh;im->loops=loops;im->started=st_millis();
    im->pixels=calloc(st_image_bytes(im),1);return im->pixels?1:st_fail(err,"Out of memory decoding animation");
}
static void sample(StImage *im,const unsigned char *rgba,int w,int h,int frame) {
    unsigned *dst=im->pixels+(size_t)frame*(size_t)im->width*(size_t)im->height;
    for(int y=0;y<im->height;y++)for(int x=0;x<im->width;x++)memcpy(dst+y*im->width+x,rgba+((size_t)(y*h/im->height)*w+x*w/im->width)*4,4);
}
static int animated_webp(const unsigned char *data,size_t n,int w,int h,StImage *im,StError *err) {
    WebPData blob={data,n};WebPDemuxer *demux=WebPDemux(&blob);
    if(!demux)return st_fail(err,"Invalid animated WebP");
    unsigned count=WebPDemuxGetI(demux,WEBP_FF_FRAME_COUNT),cw=WebPDemuxGetI(demux,WEBP_FF_CANVAS_WIDTH),ch=WebPDemuxGetI(demux,WEBP_FF_CANVAS_HEIGHT);
    int ok=count<=SOURCE_FRAMES&&cw<=SOURCE_SIDE&&ch<=SOURCE_SIDE;WebPDemuxDelete(demux);
    if(!ok)return st_fail(err,"Image exceeds animation limits");
    WebPAnimDecoderOptions options;if(!WebPAnimDecoderOptionsInit(&options))return st_fail(err,"WebP decoder unavailable");options.color_mode=MODE_RGBA;options.use_threads=0;
    WebPAnimDecoder *decoder=WebPAnimDecoderNew(&blob,&options);WebPAnimInfo info;
    if(!decoder||!WebPAnimDecoderGetInfo(decoder,&info)){WebPAnimDecoderDelete(decoder);return st_fail(err,"Invalid animated WebP");}
    ok=allocate(im,(int)info.canvas_width,(int)info.canvas_height,w,h,(int)info.frame_count,info.loop_count,err);int timestamp=0,prior=0,last=-1;
    for(unsigned i=0;ok&&i<info.frame_count;i++) {
        if(st_cancelled()){ok=st_fail(err,"Cancelled");break;}unsigned char *rgba;
        if(!WebPAnimDecoderGetNext(decoder,&rgba,&timestamp)){ok=st_fail(err,"Invalid WebP animation frame");break;}
        int dest=(int)(i*(unsigned)im->frames/info.frame_count);if(dest!=last){sample(im,rgba,(int)info.canvas_width,(int)info.canvas_height,dest);last=dest;}
        im->delay[dest]+=duration(timestamp>prior?(unsigned)(timestamp-prior):20);prior=timestamp;
    }
    WebPAnimDecoderDelete(decoder);if(!ok)st_image_free(im);return ok;
}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static unsigned be16(const unsigned char *p){return (unsigned)p[0]<<8|p[1];}
static void put32(unsigned char *p,uint32_t v){p[0]=(unsigned char)(v>>24);p[1]=(unsigned char)(v>>16);p[2]=(unsigned char)(v>>8);p[3]=(unsigned char)v;}
static size_t chunk(unsigned char *out,const char *tag,const unsigned char *data,size_t n) {
    put32(out,(uint32_t)n);memcpy(out+4,tag,4);if(n)memcpy(out+8,data,n);put32(out+8+n,(uint32_t)crc32(0,out+4,(uInt)(n+4)));return n+12;
}
static int rgba_png(const unsigned char *data,size_t n,unsigned char **rgba,int *w,int *h) {
    png_image png;memset(&png,0,sizeof(png));png.version=PNG_IMAGE_VERSION;*rgba=NULL;
    if(!png_image_begin_read_from_memory(&png,data,n)){png_image_free(&png);return 0;}
    if(!png.width||!png.height||png.width>SOURCE_SIDE||png.height>SOURCE_SIDE){png_image_free(&png);return 0;}
    png.format=PNG_FORMAT_RGBA;*rgba=malloc(PNG_IMAGE_SIZE(png));
    if(!*rgba||!png_image_finish_read(&png,NULL,*rgba,0,NULL)){free(*rgba);*rgba=NULL;png_image_free(&png);return 0;}
    *w=(int)png.width;*h=(int)png.height;png_image_free(&png);return 1;
}
typedef struct {uint32_t w,h,x,y;unsigned delay,dispose,blend;size_t start,end;int data;} PngFrame;
static int animated_png(const unsigned char *data,size_t n,int maxw,int maxh,StImage *im,StError *err) {
    static const unsigned char signature[]={137,80,78,71,13,10,26,10};
    if(n<33||memcmp(data,signature,8)||be32(data+8)!=13||memcmp(data+12,"IHDR",4))return st_fail(err,"Invalid profile PNG");
    unsigned cw=be32(data+16),ch=be32(data+20);if(!cw||!ch||cw>SOURCE_SIDE||ch>SOURCE_SIDE)return st_fail(err,"PNG exceeds size limit");
    PngFrame frames[SOURCE_FRAMES];memset(frames,0,sizeof(frames));int count=0,declared=0,seen_data=0,ended=0;unsigned loops=0,sequence=0,chunks=0;
    size_t palette=0,transparency=0;
    for(size_t p=8;p+12<=n;) {
        unsigned size=be32(data+p);const unsigned char *tag=data+p+4,*payload=data+p+8;
        if(++chunks>2048||size>n-p-12||be32(data+p+8+size)!=(uint32_t)crc32(0,tag,(uInt)(size+4)))return st_fail(err,"Invalid PNG chunk");
        if(!memcmp(tag,"acTL",4)){if(declared||seen_data||size!=8)return st_fail(err,"Invalid APNG animation control");unsigned total=be32(payload);if(!total||total>SOURCE_FRAMES)return st_fail(err,"APNG exceeds frame limit");declared=(int)total;loops=be32(payload+4);}
        else if(!memcmp(tag,"fcTL",4)) {
            if(!declared||size!=26||count>=declared||be32(payload)!=sequence++)return st_fail(err,"Invalid APNG frame control");
            if(count){frames[count-1].end=p;if(!frames[count-1].data)return st_fail(err,"Empty APNG frame");}
            PngFrame *f=frames+count++;f->w=be32(payload+4);f->h=be32(payload+8);f->x=be32(payload+12);f->y=be32(payload+16);
            if(!f->w||!f->h||f->w>cw||f->h>ch||f->x>cw-f->w||f->y>ch-f->h||payload[24]>2||payload[25]>1)return st_fail(err,"Invalid APNG frame bounds");
            f->delay=duration(be16(payload+20)*1000/(be16(payload+22)?be16(payload+22):100));f->dispose=payload[24];f->blend=payload[25];f->start=p+size+12;
        } else if(!memcmp(tag,"IDAT",4)){seen_data=1;if(count)frames[count-1].data=1;}
        else if(!memcmp(tag,"fdAT",4)){if(!count||size<4||be32(payload)!=sequence++)return st_fail(err,"Invalid APNG frame data");frames[count-1].data=1;}
        else if(!memcmp(tag,"PLTE",4)){if(seen_data||size>768)return st_fail(err,"Invalid PNG palette");palette=p;}
        else if(!memcmp(tag,"tRNS",4)){if(seen_data||size>256)return st_fail(err,"Invalid PNG transparency");transparency=p;}
        else if(!memcmp(tag,"IEND",4)){if(size||p+12!=n)return st_fail(err,"Invalid PNG end");if(count)frames[count-1].end=p;ended=1;break;}
        p+=size+12;
    }
    if(!ended)return st_fail(err,"Incomplete profile PNG");
    if(!declared) {
        unsigned char *rgba;int w,h;if(!rgba_png(data,n,&rgba,&w,&h))return st_fail(err,"Invalid profile PNG pixels");
        int ok=allocate(im,w,h,maxw,maxh,1,0,err);if(ok)sample(im,rgba,w,h,0);free(rgba);return ok;
    }
    if(count!=declared||!frames[count-1].data)return st_fail(err,"Incomplete APNG frames");
    if(!allocate(im,(int)cw,(int)ch,maxw,maxh,count,loops,err))return 0;
    size_t canvas_size=(size_t)cw*ch*4;unsigned char *canvas=calloc(canvas_size,1),*previous=malloc(canvas_size),*png=malloc(n+128);int ok=canvas&&previous&&png,last=-1;
    if(!ok)st_fail(err,"Out of memory decoding APNG");
    for(int i=0;ok&&i<count;i++) {
        if(st_cancelled()){ok=st_fail(err,"Cancelled");break;}PngFrame *f=frames+i;memcpy(png,signature,8);size_t used=8;unsigned char ihdr[13];memcpy(ihdr,data+16,13);put32(ihdr,f->w);put32(ihdr+4,f->h);used+=chunk(png+used,"IHDR",ihdr,13);
        if(palette){size_t bytes=be32(data+palette)+12;memcpy(png+used,data+palette,bytes);used+=bytes;}
        if(transparency){size_t bytes=be32(data+transparency)+12;memcpy(png+used,data+transparency,bytes);used+=bytes;}
        for(size_t p=f->start;p<f->end;) {unsigned size=be32(data+p);if(!memcmp(data+p+4,"IDAT",4))used+=chunk(png+used,"IDAT",data+p+8,size);else if(!memcmp(data+p+4,"fdAT",4))used+=chunk(png+used,"IDAT",data+p+12,size-4);p+=size+12;}
        used+=chunk(png+used,"IEND",NULL,0);unsigned char *rgba=NULL;int w,h;
        if(!rgba_png(png,used,&rgba,&w,&h)){ok=st_fail(err,"Invalid APNG frame pixels");break;}
        if(f->dispose==2)memcpy(previous,canvas,canvas_size);
        for(unsigned y=0;y<f->h;y++)for(unsigned x=0;x<f->w;x++) {
            unsigned char *dst=canvas+((size_t)(f->y+y)*cw+f->x+x)*4,*src=rgba+((size_t)y*f->w+x)*4;
            if(!f->blend)memcpy(dst,src,4);
            else {unsigned a=src[3],b=dst[3],alpha=a*255+b*(255-a);for(int s=0;s<3;s++)dst[s]=(unsigned char)(alpha?(src[s]*a*255+dst[s]*b*(255-a))/alpha:0);dst[3]=(unsigned char)(alpha/255);}
        }
        free(rgba);int dest=i*im->frames/count;if(dest!=last){sample(im,canvas,(int)cw,(int)ch,dest);last=dest;}im->delay[dest]+=f->delay;
        if(f->dispose==1){for(unsigned y=0;y<f->h;y++)memset(canvas+((size_t)(f->y+y)*cw+f->x)*4,0,(size_t)f->w*4);}else if(f->dispose==2)memcpy(canvas,previous,canvas_size);
    }
    free(canvas);free(previous);free(png);if(!ok)st_image_free(im);return ok;
}
/* Sequential giflib decoding avoids DGifSlurp/SavedImages allocations for
 * every source frame. Only the canvas, disposal backup and sampled output
 * remain resident. Scan first to reject dimensions/frame counts before LZW. */
typedef struct {const unsigned char *data;size_t n,at;} GifInput;
static int gif_read(GifFileType *gif,GifByteType *out,int count) {
    GifInput *in=gif->UserData;if(st_cancelled()||count<0||(size_t)count>in->n-in->at)return 0;
    memcpy(out,in->data+in->at,(size_t)count);in->at+=(size_t)count;return count;
}
static int gif_bounds(const GifFileType *g) {
    const GifImageDesc *f=&g->Image;
    return g->SWidth>0&&g->SHeight>0&&g->SWidth<=SOURCE_SIDE&&g->SHeight<=SOURCE_SIDE&&
        f->Width>0&&f->Height>0&&f->Width<=g->SWidth&&f->Height<=g->SHeight&&
        f->Left>=0&&f->Top>=0&&f->Left<=g->SWidth-f->Width&&f->Top<=g->SHeight-f->Height;
}
static int gif_scan(const unsigned char *data,size_t n,int *count,unsigned *loops,StError *err) {
    GifInput in={data,n,0};int code;GifFileType *g=DGifOpen(&in,gif_read,&code);if(!g)return st_fail(err,"Invalid GIF header");
    int ok=g->SWidth>0&&g->SHeight>0&&g->SWidth<=SOURCE_SIDE&&g->SHeight<=SOURCE_SIDE,ended=0;unsigned records=0;*count=0;*loops=1;
    GifRecordType record;
    while(ok&&DGifGetRecordType(g,&record)==GIF_OK) {
        if(st_cancelled()||++records>2048){ok=0;break;}
        if(record==TERMINATE_RECORD_TYPE){ended=1;break;}
        if(record==IMAGE_DESC_RECORD_TYPE) {
            if(++*count>SOURCE_FRAMES||DGifGetImageHeader(g)==GIF_ERROR||!gif_bounds(g)){ok=0;break;}
            GifByteType *block;int size;if(DGifGetCode(g,&size,&block)==GIF_ERROR){ok=0;break;}
            while(block){if(st_cancelled()||DGifGetCodeNext(g,&block)==GIF_ERROR){ok=0;break;}}
        } else if(record==EXTENSION_RECORD_TYPE) {
            GifByteType *block;int ext;if(DGifGetExtension(g,&ext,&block)==GIF_ERROR){ok=0;break;}
            int loop_ext=ext==APPLICATION_EXT_FUNC_CODE&&block&&block[0]==11&&(!memcmp(block+1,"NETSCAPE2.0",11)||!memcmp(block+1,"ANIMEXTS1.0",11));
            while(block) {
                if(loop_ext&&block[0]==3&&block[1]==1){unsigned repeats=(unsigned)block[2]|(unsigned)block[3]<<8;*loops=repeats?repeats+1:0;}
                if(st_cancelled()||DGifGetExtensionNext(g,&block)==GIF_ERROR){ok=0;break;}
            }
        } else ok=0;
    }
    DGifCloseFile(g,&code);return ok&&ended&&*count?1:st_fail(err,st_cancelled()?"Cancelled":"GIF exceeds limits or is incomplete");
}
static unsigned gif_background(const GifFileType *g,int transparent) {
    if(transparent>=0||!g->SColorMap||g->SBackGroundColor<0||g->SBackGroundColor>=g->SColorMap->ColorCount)return 0;
    GifColorType c=g->SColorMap->Colors[g->SBackGroundColor];return 0xff000000u|c.Red|(unsigned)c.Green<<8|(unsigned)c.Blue<<16;
}
static int gif_row(GifFileType *g,unsigned *canvas,int y,int transparent,GifPixelType row[SOURCE_SIDE]) {
    if(st_cancelled()||DGifGetLine(g,row,g->Image.Width)==GIF_ERROR)return 0;
    ColorMapObject *map=g->Image.ColorMap?g->Image.ColorMap:g->SColorMap;if(!map)return 0;
    unsigned *dst=canvas+(size_t)(g->Image.Top+y)*g->SWidth+g->Image.Left;
    for(int x=0;x<g->Image.Width;x++) {
        if(row[x]==transparent)continue;
        if(row[x]>=map->ColorCount)return 0;
        GifColorType c=map->Colors[row[x]];dst[x]=0xff000000u|c.Red|(unsigned)c.Green<<8|(unsigned)c.Blue<<16;
    }
    return 1;
}
static int animated_gif(const unsigned char *data,size_t n,int maxw,int maxh,StImage *im,StError *err) {
    int count=0,code;unsigned loops=1;st_stage("GIF scan");if(!gif_scan(data,n,&count,&loops,err))return 0;
    GifInput in={data,n,0};GifFileType *g=DGifOpen(&in,gif_read,&code);if(!g)return st_fail(err,"Invalid GIF header");
    int ok=allocate(im,g->SWidth,g->SHeight,maxw,maxh,count,loops,err),index=0,last=-1,ended=0;
    size_t bytes=(size_t)g->SWidth*g->SHeight*4;
    unsigned *canvas=ok?calloc(bytes,1):NULL,*previous=ok?malloc(bytes):NULL;
    if(ok&&(!canvas||!previous))ok=st_fail(err,"Out of memory decoding GIF");
    int transparent=-1,dispose=0;unsigned delay=100;GifRecordType record;GifPixelType row[SOURCE_SIDE];st_stage("GIF decode");
    while(ok&&DGifGetRecordType(g,&record)==GIF_OK) {
        if(st_cancelled()){ok=0;break;}
        if(record==TERMINATE_RECORD_TYPE){ended=1;break;}
        if(record==EXTENSION_RECORD_TYPE) {
            GifByteType *block;int ext;if(DGifGetExtension(g,&ext,&block)==GIF_ERROR){ok=0;break;}
            if(ext==GRAPHICS_EXT_FUNC_CODE&&block){if(block[0]!=4){ok=0;break;}dispose=(block[1]>>2)&7;if(dispose>3){ok=0;break;}transparent=block[1]&1?block[4]:-1;delay=duration(((unsigned)block[2]|(unsigned)block[3]<<8)*10);}
            while(block)if(st_cancelled()||DGifGetExtensionNext(g,&block)==GIF_ERROR){ok=0;break;}
        } else if(record==IMAGE_DESC_RECORD_TYPE) {
            if(index>=count||DGifGetImageHeader(g)==GIF_ERROR||!gif_bounds(g)){ok=0;break;}
            if(!index){unsigned bg=gif_background(g,transparent);for(size_t p=0;p<bytes/4;p++)canvas[p]=bg;}
            if(dispose==3)memcpy(previous,canvas,bytes);
            if(g->Image.Interlace){static const int start[]={0,4,2,1},step[]={8,8,4,2};for(int pass=0;ok&&pass<4;pass++)for(int y=start[pass];ok&&y<g->Image.Height;y+=step[pass])ok=gif_row(g,canvas,y,transparent,row);}
            else for(int y=0;ok&&y<g->Image.Height;y++)ok=gif_row(g,canvas,y,transparent,row);
            if(!ok)break;
            int dest=index*im->frames/count;if(dest!=last){sample(im,(unsigned char *)canvas,g->SWidth,g->SHeight,dest);last=dest;}im->delay[dest]+=delay;index++;
            if(dispose==2){unsigned bg=gif_background(g,transparent);for(int y=0;y<g->Image.Height;y++)for(int x=0;x<g->Image.Width;x++)canvas[(size_t)(g->Image.Top+y)*g->SWidth+g->Image.Left+x]=bg;}
            else if(dispose==3)memcpy(canvas,previous,bytes);
            transparent=-1;dispose=0;delay=100;
        } else ok=0;
    }
    DGifCloseFile(g,&code);free(canvas);free(previous);ok=ok&&ended&&index==count;
    if(!ok){st_image_free(im);if(!err->text[0])st_fail(err,st_cancelled()?"Cancelled":"Invalid GIF animation pixels");}return ok;
}

int st_profile_image_decode(const unsigned char *data,size_t n,int w,int h,StImage *im,StError *err) {
    memset(im,0,sizeof(*im));if(!data||n>512*1024)return st_fail(err,"Image exceeds download limit");
    if(n>=6&&(!memcmp(data,"GIF87a",6)||!memcmp(data,"GIF89a",6)))return animated_gif(data,n,w,h,im,err);
    if(n>=8&&!memcmp(data,"\x89PNG\r\n\x1a\n",8))return animated_png(data,n,w,h,im,err);
    WebPBitstreamFeatures features;if(WebPGetFeatures(data,n,&features)!=VP8_STATUS_OK)return st_fail(err,"Image must be WebP, PNG/APNG or GIF");
    return features.has_animation?animated_webp(data,n,w,h,im,err):st_image_decode(data,n,w,h,im,err);
}
