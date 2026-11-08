/* Deterministic release typography and PSP-sized PNGs from vendored artwork.
 * Run on the host; no image processing runs on the console. */
#include "stoat.h"
#include <png.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {unsigned char *rgba;int w,h;} Image;
static void check(int ok,const char *message){if(!ok){fprintf(stderr,"XMB assets: %s\n",message);exit(1);}}
static Image load(const char *path) {
    png_image p={0};p.version=PNG_IMAGE_VERSION;check(png_image_begin_read_from_file(&p,path),path);
    check(p.width>0&&p.height>0&&p.width<=4096&&p.height<=4096,"input dimensions");p.format=PNG_FORMAT_RGBA;
    Image im={malloc(PNG_IMAGE_SIZE(p)),(int)p.width,(int)p.height};check(im.rgba!=NULL,"allocation");
    check(png_image_finish_read(&p,NULL,im.rgba,0,NULL),"read PNG");png_image_free(&p);return im;
}
static void scale(const Image *im,unsigned char *rgb,int w,int h,int left,int top,int dw,int dh) {
    for(int y=0;y<dh;y++)for(int x=0;x<dw;x++) {
        /* Area filtering keeps the icon edges smooth at XMB size. */
        unsigned sx0=(unsigned)x*(unsigned)im->w*256/(unsigned)dw,sx1=(unsigned)(x+1)*(unsigned)im->w*256/(unsigned)dw;
        unsigned sy0=(unsigned)y*(unsigned)im->h*256/(unsigned)dh,sy1=(unsigned)(y+1)*(unsigned)im->h*256/(unsigned)dh;
        if(left+x<0||left+x>=w||top+y<0||top+y>=h)continue;
        uint64_t sum[4]={0},weight=0;
        for(unsigned iy=sy0/256;iy<(sy1+255)/256;iy++)for(unsigned ix=sx0/256;ix<(sx1+255)/256;ix++) {
            unsigned xl=sx0>ix*256?sx0:ix*256,xr=sx1<(ix+1)*256?sx1:(ix+1)*256;
            unsigned yt=sy0>iy*256?sy0:iy*256,yb=sy1<(iy+1)*256?sy1:(iy+1)*256;
            uint64_t part=(xr-xl)*(yb-yt);const unsigned char *src=im->rgba+((size_t)iy*im->w+ix)*4;
            weight+=part;for(int c=0;c<4;c++)sum[c]+=src[c]*part;
        }
        unsigned char *dst=rgb+((top+y)*w+left+x)*3;unsigned alpha=(unsigned)(sum[3]/weight);
        for(int c=0;c<3;c++)dst[c]=(unsigned char)(((unsigned)(sum[c]/weight)*alpha+dst[c]*(255-alpha)+127)/255);
    }
}
static int text_width(FT_Face face,const char *text) {int w=0;for(;*text;text++){check(!FT_Load_Char(face,(unsigned char)*text,FT_LOAD_RENDER),"glyph");w+=(int)(face->glyph->advance.x>>6);}return w;}
static void text(FT_Face face,unsigned char *rgb,const char *label,int baseline,unsigned r,unsigned g,unsigned b) {
    int pen=480-12-text_width(face,label);unsigned color[3]={r,g,b};
    for(;*label;label++) {
        check(!FT_Load_Char(face,(unsigned char)*label,FT_LOAD_RENDER),"glyph");FT_GlyphSlot glyph=face->glyph;
        for(unsigned y=0;y<glyph->bitmap.rows;y++)for(unsigned x=0;x<glyph->bitmap.width;x++) {
            int px=pen+glyph->bitmap_left+(int)x,py=baseline-glyph->bitmap_top+(int)y;
            if(px<0||px>=480||py<0||py>=272)continue;
            unsigned alpha=glyph->bitmap.buffer[(int)y*glyph->bitmap.pitch+(int)x];unsigned char *p=rgb+(py*480+px)*3;
            for(int c=0;c<3;c++)p[c]=(unsigned char)((color[c]*alpha+p[c]*(255-alpha)+127)/255);
        }
        pen+=(int)(glyph->advance.x>>6);
    }
}
static void save(const char *path,unsigned char *rgb,int w,int h) {
    FILE *f=fopen(path,"wb");check(f!=NULL,path);png_structp p=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);check(p!=NULL,"PNG writer");png_infop info=png_create_info_struct(p);check(info!=NULL,"PNG metadata");
    if(setjmp(png_jmpbuf(p))){fprintf(stderr,"XMB PNG write failed\n");exit(1);}
    png_init_io(p,f);png_set_IHDR(p,info,(png_uint_32)w,(png_uint_32)h,8,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_text meta={0};meta.compression=PNG_TEXT_COMPRESSION_NONE;meta.key="Description";meta.text=ST_APP_NAME " " ST_APP_VERSION " | API " ST_API_VERSION;png_set_text(p,info,&meta,1);
    png_write_info(p,info);for(int y=0;y<h;y++)png_write_row(p,rgb+y*w*3);png_write_end(p,info);png_destroy_write_struct(&p,&info);check(!fclose(f),"close PNG");
}
int main(int argc,char **argv) {
    check(argc==3,"usage: xmb-assets ICON0.png PIC1.png");
    Image icon=load("assets/xmb/messenger.png");
    unsigned char rgb[480*272*3];
    for(int i=0;i<144*80;i++)memcpy(rgb+i*3,icon.rgba,3);
    scale(&icon,rgb,144,80,36,4,72,72);save(argv[1],rgb,144,80);for(int i=0;i<480*272;i++)memcpy(rgb+i*3,icon.rgba,3);
    FT_Library library;FT_Face face;check(!FT_Init_FreeType(&library),"FreeType");check(!FT_New_Face(library,"assets/NotoSans-Regular.ttf",0,&face),"font");check(!FT_Set_Pixel_Sizes(face,0,11),"font size");
    text(face,rgb,ST_APP_NAME " " ST_APP_VERSION,248,228,240,255);text(face,rgb,"API " ST_API_VERSION,263,164,188,215);
    save(argv[2],rgb,480,272);FT_Done_Face(face);FT_Done_FreeType(library);free(icon.rgba);
    puts("XMB assets: ICON0 144x80, PIC1 480x272, client " ST_APP_VERSION ", API " ST_API_VERSION);return 0;
}
