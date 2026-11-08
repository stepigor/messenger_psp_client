#include "thumbnail.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <png.h>
#include <jpeglib.h>
#include <gif_lib.h>
#include <webp/demux.h>

#define THUMB_SIDE 8192u
#define THUMB_PIXELS (16u*1024u*1024u)
typedef struct {
    FILE *file;StImage image;unsigned char *row,*data;
    png_structp png;png_infop info;
    struct jpeg_decompress_struct jpeg;struct jpeg_error_mgr jpeg_error;
    int jpeg_created; jmp_buf jump;uint64_t deadline;size_t read;
} Thumb;
static int interrupted(Thumb *t){return st_cancelled()||st_millis()>=t->deadline;}
static int allocate(Thumb *t,unsigned width,unsigned height){
    if(!width||!height||width>THUMB_SIDE||height>THUMB_SIDE||(uint64_t)width*height>THUMB_PIXELS)return 0;
    unsigned w=width,h=height;if(w>ST_PHOTO_WIDTH){h=h*ST_PHOTO_WIDTH/w;w=ST_PHOTO_WIDTH;}if(h>ST_PHOTO_HEIGHT){w=w*ST_PHOTO_HEIGHT/h;h=ST_PHOTO_HEIGHT;}
    if(!w)w=1;
    if(!h)h=1;
    t->image.width=(int)w;t->image.height=(int)h;t->image.frames=1;
    t->image.pixels=calloc((size_t)w*h,4);return t->image.pixels!=NULL;
}
static void png_failure(png_structp p,const char *s){(void)s;Thumb *t=png_get_error_ptr(p);longjmp(t->jump,1);}
static void thumb_png_warning(png_structp p,const char *s){(void)p;(void)s;}
static void png_input(png_structp p,png_bytep data,png_size_t n){
    Thumb *t=png_get_io_ptr(p);if(interrupted(t)||n>ST_PHOTO_LIMIT-t->read||fread(data,1,n,t->file)!=n)png_error(p,"Read failed");t->read+=n;
}
static int png_thumb(Thumb *t){
    t->png=png_create_read_struct(PNG_LIBPNG_VER_STRING,t,png_failure,thumb_png_warning);if(!t->png)return 0;
    t->info=png_create_info_struct(t->png);if(!t->info)return 0;
    png_set_read_fn(t->png,t,png_input);png_set_crc_action(t->png,PNG_CRC_ERROR_QUIT,PNG_CRC_ERROR_QUIT);
#ifdef PNG_SET_USER_LIMITS_SUPPORTED
    png_set_user_limits(t->png,THUMB_SIDE,THUMB_SIDE);png_set_chunk_malloc_max(t->png,128*1024);png_set_chunk_cache_max(t->png,32);
#endif
    png_read_info(t->png,t->info);unsigned width=png_get_image_width(t->png,t->info),height=png_get_image_height(t->png,t->info);
    if(!allocate(t,width,height))return 0;
    int type=png_get_color_type(t->png,t->info),bits=png_get_bit_depth(t->png,t->info),alpha=type&PNG_COLOR_MASK_ALPHA;
    if(bits==16)png_set_strip_16(t->png);
    if(type==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(t->png);
    if(type==PNG_COLOR_TYPE_GRAY&&bits<8)png_set_expand_gray_1_2_4_to_8(t->png);
    if(png_get_valid(t->png,t->info,PNG_INFO_tRNS)){png_set_tRNS_to_alpha(t->png);alpha=1;}
    if(type==PNG_COLOR_TYPE_GRAY||type==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(t->png);
    if(!alpha)png_set_filler(t->png,255,PNG_FILLER_AFTER);
    int passes=png_set_interlace_handling(t->png);png_read_update_info(t->png,t->info);
    size_t bytes=png_get_rowbytes(t->png,t->info);if(bytes!=(size_t)width*4)return 0;
    t->row=calloc(bytes,1);if(!t->row)return 0;
    static const unsigned xs[]={0,4,0,2,0,1,0},ys[]={0,0,4,0,2,0,1},xd[]={8,8,4,4,2,2,1},yd[]={8,8,8,4,4,2,2};
    for(int pass=0;pass<passes;pass++)for(unsigned y=0;y<height;y++){
        if(interrupted(t))return 0;
        png_read_row(t->png,t->row,NULL);
        if(passes>1&&(y<ys[pass]||(y-ys[pass])%yd[pass]))continue;
        for(int dy=0;dy<t->image.height;dy++)if((unsigned)dy*height/(unsigned)t->image.height==y)for(int dx=0;dx<t->image.width;dx++){
            unsigned x=(unsigned)dx*width/(unsigned)t->image.width;
            if(passes>1&&(x<xs[pass]||(x-xs[pass])%xd[pass]))continue;
            memcpy(t->image.pixels+(size_t)dy*t->image.width+dx,t->row+x*4,4);
        }
    }
    png_read_end(t->png,t->info);return 1;
}
static void jpeg_failure(j_common_ptr j){Thumb *t=j->client_data;longjmp(t->jump,1);}
static void jpeg_message(j_common_ptr j,int level){if(level<0)jpeg_failure(j);}
static void jpeg_silent(j_common_ptr j){(void)j;}
static int jpeg_thumb(Thumb *t){
    t->jpeg.err=jpeg_std_error(&t->jpeg_error);t->jpeg_error.error_exit=jpeg_failure;t->jpeg_error.emit_message=jpeg_message;t->jpeg_error.output_message=jpeg_silent;
    t->jpeg.client_data=t;t->jpeg_created=1;jpeg_create_decompress(&t->jpeg);t->jpeg.mem->max_memory_to_use=2*1024*1024;
    jpeg_stdio_src(&t->jpeg,t->file);if(jpeg_read_header(&t->jpeg,TRUE)!=JPEG_HEADER_OK)return 0;
    unsigned w=t->jpeg.image_width,h=t->jpeg.image_height;
    if(!w||!h||w>THUMB_SIDE||h>THUMB_SIDE||(uint64_t)w*h>THUMB_PIXELS||t->jpeg.data_precision!=8||(t->jpeg.num_components!=1&&t->jpeg.num_components!=3))return 0;
    /* Multiple scans need whole-image coefficients. Keep them in RAM below a
     * fixed limit; never ask libjpeg for a backing file on the card. */
    if(jpeg_has_multiple_scans(&t->jpeg)&&(uint64_t)w*h>256*1024)return 0;
    t->jpeg.scale_num=1;t->jpeg.scale_denom=1;
    while(t->jpeg.scale_denom<8&&(w/t->jpeg.scale_denom>ST_PHOTO_WIDTH*2||h/t->jpeg.scale_denom>ST_PHOTO_HEIGHT*2))t->jpeg.scale_denom*=2;
    t->jpeg.out_color_space=JCS_RGB;t->jpeg.dct_method=JDCT_IFAST;t->jpeg.do_fancy_upsampling=FALSE;
    if(interrupted(t)||!jpeg_start_decompress(&t->jpeg))return 0;
    w=t->jpeg.output_width;h=t->jpeg.output_height;
    if(t->jpeg.output_components!=3||!allocate(t,w,h))return 0;
    t->row=malloc((size_t)w*3);if(!t->row)return 0;
    while(t->jpeg.output_scanline<h){
        if(interrupted(t))return 0;
        unsigned y=t->jpeg.output_scanline;JSAMPROW row=t->row;if(jpeg_read_scanlines(&t->jpeg,&row,1)!=1)return 0;
        for(int dy=0;dy<t->image.height;dy++)if((unsigned)dy*h/(unsigned)t->image.height==y)for(int dx=0;dx<t->image.width;dx++){
            unsigned x=(unsigned)dx*w/(unsigned)t->image.width;const unsigned char *p=t->row+x*3;t->image.pixels[(size_t)dy*t->image.width+dx]=0xff000000u|p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16;
        }
    }
    return jpeg_finish_decompress(&t->jpeg);
}
static unsigned le16(const unsigned char *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t le32(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned component(unsigned value,unsigned mask){if(!mask)return 0;while(!(mask&1)){mask>>=1;value>>=1;}return (unsigned)((uint64_t)(value&mask)*255/mask);}
static int bmp_thumb(Thumb *t){
    unsigned char header[138]={0};if(fread(header,1,54,t->file)!=54)return 0;
    uint32_t offset=le32(header+10),dib=le32(header+14),w=le32(header+18),raw_height=le32(header+22),compression=le32(header+30);int top=(raw_height&0x80000000u)!=0;
    unsigned h=top?0u-raw_height:raw_height,bits=le16(header+28);uint64_t row=((uint64_t)w*bits+31)/32*4;
    if(dib<40||dib>124||le16(header+26)!=1||(bits!=8&&bits!=16&&bits!=24&&bits!=32)||(compression&&!(compression==3&&(bits==16||bits==32)))||offset<14+dib||row>ST_PHOTO_LIMIT||!allocate(t,w,h))return 0;
    if((uint64_t)offset+row*h>ST_PHOTO_LIMIT)return 0;
    unsigned masks[3]={bits==16?0x7c00u:0xff0000u,bits==16?0x3e0u:0xff00u,bits==16?0x1fu:0xffu};
    if(compression==3){if(offset<66||fseek(t->file,54,SEEK_SET)||fread(header,1,12,t->file)!=12)return 0;for(int i=0;i<3;i++){masks[i]=le32(header+i*4);if(!masks[i])return 0;}}
    unsigned char palette[1024];unsigned colours=0;
    if(bits==8){colours=le32(header+46);if(!colours)colours=256;if(colours>256||offset<14+dib+colours*4||fseek(t->file,14+dib,SEEK_SET)||fread(palette,4,colours,t->file)!=colours)return 0;}
    t->row=malloc((size_t)row);if(!t->row)return 0;
    for(int dy=0;dy<t->image.height;dy++){
        if(interrupted(t))return 0;
        unsigned sy=(unsigned)dy*h/(unsigned)t->image.height;uint64_t pos=offset+(uint64_t)(top?sy:h-1-sy)*row;
        if(fseek(t->file,(long)pos,SEEK_SET)||fread(t->row,1,(size_t)row,t->file)!=row)return 0;
        for(int dx=0;dx<t->image.width;dx++){
            unsigned sx=(unsigned)dx*w/(unsigned)t->image.width,r,g,b;
            if(bits==8){unsigned index=t->row[sx];if(index>=colours)return 0;b=palette[index*4];g=palette[index*4+1];r=palette[index*4+2];}
            else if(bits==24||(!compression&&bits==32)){const unsigned char *p=t->row+(size_t)sx*(bits/8);b=p[0];g=p[1];r=p[2];}
            else {unsigned value=bits==16?le16(t->row+sx*2):le32(t->row+sx*4);r=component(value,masks[0]);g=component(value,masks[1]);b=component(value,masks[2]);}
            t->image.pixels[(size_t)dy*t->image.width+dx]=0xff000000u|r|g<<8|b<<16;
        }
    }
    return 1;
}
static int gif_input(GifFileType *gif,GifByteType *out,int count){Thumb *t=gif->UserData;if(count<0||interrupted(t)||(size_t)count>ST_PHOTO_LIMIT-t->read)return 0;int n=(int)fread(out,1,(size_t)count,t->file);t->read+=(size_t)n;return n;}
static int gif_thumb(Thumb *t){
    int code=0;GifFileType *gif=DGifOpen(t,gif_input,&code);if(!gif)return 0;
    int ok=0,transparent=-1;GifRecordType type;
    if(!allocate(t,(unsigned)gif->SWidth,(unsigned)gif->SHeight))goto done;
    while(!interrupted(t)&&DGifGetRecordType(gif,&type)==GIF_OK){
        if(type==TERMINATE_RECORD_TYPE)break;
        if(type==EXTENSION_RECORD_TYPE){int label;GifByteType *ext;if(DGifGetExtension(gif,&label,&ext)!=GIF_OK)break;if(label==GRAPHICS_EXT_FUNC_CODE&&ext&&ext[0]==4&&(ext[1]&1))transparent=ext[4];while(ext)if(DGifGetExtensionNext(gif,&ext)!=GIF_OK)goto done;}
        if(type!=IMAGE_DESC_RECORD_TYPE)continue;
        if(DGifGetImageDesc(gif)!=GIF_OK)break;
        GifImageDesc *im=&gif->Image;ColorMapObject *map=im->ColorMap?im->ColorMap:gif->SColorMap;
        if(!map||im->Left<0||im->Top<0||im->Width<1||im->Height<1||im->Left>gif->SWidth-im->Width||im->Top>gif->SHeight-im->Height)break;
        t->row=malloc((size_t)im->Width);if(!t->row)break;static const int starts[]={0,4,2,1},steps[]={8,8,4,2};
        for(int pass=0;pass<(im->Interlace?4:1);pass++)for(int y=im->Interlace?starts[pass]:0;y<im->Height;y+=im->Interlace?steps[pass]:1){
            if(interrupted(t)||DGifGetLine(gif,t->row,im->Width)!=GIF_OK)goto done;
            for(int dy=0;dy<t->image.height;dy++)if(dy*gif->SHeight/t->image.height==im->Top+y)for(int dx=0;dx<t->image.width;dx++){
                int x=dx*gif->SWidth/t->image.width-im->Left;if(x<0||x>=im->Width)continue;unsigned index=t->row[x];if(index>=(unsigned)map->ColorCount)goto done;if((int)index==transparent)continue;
                GifColorType p=map->Colors[index];t->image.pixels[(size_t)dy*t->image.width+dx]=0xff000000u|p.Red|(unsigned)p.Green<<8|(unsigned)p.Blue<<16;
            }
        }
        ok=1;break; /* Thumbnails intentionally decode the first frame only. */
    }
done:DGifCloseFile(gif,&code);return ok;
}
static int webp_thumb(Thumb *t,size_t size,StError *err){
    if(size>512*1024)return 0;
    t->data=malloc(size);if(!t->data||fread(t->data,1,size,t->file)!=size||interrupted(t))return 0;
    WebPData data={t->data,size};WebPDemuxer *demux=WebPDemux(&data);if(!demux)return 0;
    unsigned w=WebPDemuxGetI(demux,WEBP_FF_CANVAS_WIDTH),h=WebPDemuxGetI(demux,WEBP_FF_CANVAS_HEIGHT);WebPIterator frame={0};StImage part={0};
    int ok=allocate(t,w,h)&&WebPDemuxGetFrame(demux,1,&frame)&&st_image_decode(frame.fragment.bytes,frame.fragment.size,ST_PHOTO_WIDTH,ST_PHOTO_HEIGHT,&part,err);
    if(ok)for(int dy=0;dy<t->image.height;dy++)for(int dx=0;dx<t->image.width;dx++){
        int x=(int)((unsigned)dx*w/(unsigned)t->image.width)-frame.x_offset,y=(int)((unsigned)dy*h/(unsigned)t->image.height)-frame.y_offset;
        if(x>=0&&x<frame.width&&y>=0&&y<frame.height)t->image.pixels[(size_t)dy*t->image.width+dx]=part.pixels[(size_t)(y*part.height/frame.height)*part.width+x*part.width/frame.width];
    }
    st_image_free(&part);WebPDemuxReleaseIterator(&frame);WebPDemuxDelete(demux);return ok;
}
int st_photo_thumbnail(const char *path,const char *expected,StImage *out,StError *err){
    memset(out,0,sizeof(*out));memset(err,0,sizeof(*err));char key[33];uint64_t size;const char *mime;
    if(!st_photo_info(path,&size,&mime,err)||!st_photo_key(path,key,err)||strcmp(expected,key))return st_fail(err,"Photo changed or unavailable");
    Thumb *t=calloc(1,sizeof(*t));if(!t)return st_fail(err,"Out of memory");t->deadline=st_millis()+5000;t->file=fopen(path,"rb");int ok=0;
    if(t->file&&!setjmp(t->jump)){
        st_stage("Photo thumbnail");
        if(!strcmp(mime,"image/png"))ok=png_thumb(t);
        else if(!strcmp(mime,"image/jpeg"))ok=jpeg_thumb(t);
        else if(!strcmp(mime,"image/bmp"))ok=bmp_thumb(t);
        else if(!strcmp(mime,"image/gif"))ok=gif_thumb(t);
        else ok=webp_thumb(t,(size_t)size,err);
    }
    if(ok&&(interrupted(t)||!st_photo_key(path,key,err)||strcmp(expected,key)))ok=0;
    if(t->png)png_destroy_read_struct(&t->png,t->info?&t->info:NULL,NULL);
    if(t->jpeg_created)jpeg_destroy_decompress(&t->jpeg);
    if(t->file)fclose(t->file);
    free(t->row);free(t->data);
    if(ok){*out=t->image;memset(&t->image,0,sizeof(t->image));}else if(!err->text[0])st_fail(err,st_cancelled()?"Cancelled":"Preview unavailable");
    st_image_free(&t->image);free(t);return ok;
}
