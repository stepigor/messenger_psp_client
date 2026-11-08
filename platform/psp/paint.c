#include "stoat.h"
#include "emoji.h"
#include "markdown.h"
#include "ui_draw.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>
#include <math.h>

static FT_Library library;
static FT_Face face;
static unsigned *frame;
static int stride;
static int clip_l,clip_t,clip_r=480,clip_b=272;
void ui_clip(int l,int t,int r,int b){clip_l=l<0?0:l;clip_t=t<0?0:t;clip_r=r>480?480:r;clip_b=b>272?272:b;}
void ui_clip_reset(void){ui_clip(0,0,480,272);}
static StMediaCache *images;
static StRoleGradient role_gradient;
static int gradient_active,gradient_x,gradient_y,gradient_width;
static unsigned gradient_colour(int px,int py) {
    float dx=role_gradient.dx,dy=role_gradient.dy,extent=fabsf(dx)*gradient_width+fabsf(dy)*19;
    float origin=fminf(0,dx*gradient_width)+fminf(0,dy*19);
    float at=extent>0?((px-gradient_x)*dx+(py-gradient_y)*dy-origin)/extent:0;
    if(at<=role_gradient.stops[0])return role_gradient.colours[0];
    for(int i=1;i<role_gradient.count;i++)if(at<=role_gradient.stops[i]) {
        float span=role_gradient.stops[i]-role_gradient.stops[i-1],t=span>0?(at-role_gradient.stops[i-1])/span:1;
        unsigned color=0;for(int s=0;s<32;s+=8){float a=(float)((role_gradient.colours[i-1]>>s)&255),b=(float)((role_gradient.colours[i]>>s)&255);color|=(unsigned)(a+(b-a)*t+.5f)<<s;}return color;
    }
    return role_gradient.colours[role_gradient.count-1];
}
void ui_media_cache(StMediaCache *cache){images=cache;}
int ui_image_state(const char *tag,const char *id){if(images)for(int i=0;i<ST_MEDIA_SLOTS;i++)if(!strcmp(images->slots[i].key.tag,tag)&&!strcmp(images->slots[i].key.id,id))return images->slots[i].state;return 0;}
static int image_area_ref(const StMediaRequest *key,int x,int y,int width,int height,int round,int clip_left,int clip_top,int clip_right,int clip_bottom) {
    if(!frame || !images || width<1 || height<1)return 0;
    if(clip_left<clip_l)clip_left=clip_l;
    if(clip_top<clip_t)clip_top=clip_t;
    if(clip_right>clip_r)clip_right=clip_r;
    if(clip_bottom>clip_b)clip_bottom=clip_b;
    const StImage *im=st_media_get_ref(images,key);if(!im||!im->pixels)return 0;
    const unsigned *pixels=st_image_pixels(im,st_millis());
    int w=width,h=height;
    if(!round){if(im->width*h>im->height*w)h=im->height*w/im->width;else w=im->width*h/im->height;}
    if(w<1||h<1)return 0;
    x+=(width-w)/2;y+=(height-h)/2;
    int source_w=im->width,source_h=im->height,left=0,top=0;
    if(round){if(source_w*h>source_h*w){source_w=source_h*w/h;left=(im->width-source_w)/2;}else{source_h=source_w*h/w;top=(im->height-source_h)/2;}}
    for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++) {
        int px=x+dx,py=y+dy;
        if(px<0||px>=480||py<0||py>=272||px<clip_left||py<clip_top||px>=clip_right||py>=clip_bottom)continue;
        if(round==1){int rx=2*dx+1-w,ry=2*dy+1-h;if(rx*rx*h*h+ry*ry*w*w>w*w*h*h)continue;}
        unsigned v=pixels[(top+dy*source_h/h)*im->width+left+dx*source_w/w],alpha=v>>24,bg=frame[py*stride+px],mixed=0xff000000;
        for(int shift=0;shift<24;shift+=8)mixed|=((((v>>shift)&255)*alpha+((bg>>shift)&255)*(255-alpha))/255)<<shift;
        frame[py*stride+px]=mixed;
    }
    return 1;
}
static int image_area(const char *tag,const char *id,int x,int y,int width,int height,int round,int left,int top,int right,int bottom){StMediaRequest key={0};st_copy(key.tag,sizeof(key.tag),tag);st_copy(key.id,sizeof(key.id),id);return image_area_ref(&key,x,y,width,height,round,left,top,right,bottom);}
int ui_image(const char *tag,const char *id,int x,int y,int width,int height,int round){return image_area(tag,id,x,y,width,height,round,0,0,480,272);}
int ui_image_ref(const StMediaRequest *key,int x,int y,int width,int height,int round){return image_area_ref(key,x,y,width,height,round,0,0,480,272);}
void ui_bitmap(const StImage *image,int x,int y,int w,int h,unsigned opacity) {
    if(!image||!image->pixels||!frame||w<1||h<1)return;
    if(opacity>1000)opacity=1000;
    for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++) {
        int px=x+dx,py=y+dy;if(px<clip_l||px>=clip_r||py<clip_t||py>=clip_b)continue;
        unsigned v=image->pixels[(dy*image->height/h)*image->width+dx*image->width/w],alpha=(v>>24)*opacity/1000,bg=frame[py*stride+px],mixed=0xff000000;
        for(int shift=0;shift<24;shift+=8)mixed|=((((v>>shift)&255)*alpha+((bg>>shift)&255)*(255-alpha))/255)<<shift;
        frame[py*stride+px]=mixed;
    }
}
typedef struct { unsigned char *data;int width,height,left,top,advance; } Glyph;
static Glyph *cache[1281];
static Glyph *glyph(uint32_t cp) {
    if(!face)return NULL;
    unsigned cache_slot=cp;
    if(cp==0x2022)cache_slot=1280;else if(cp>=1280){cp='?';cache_slot=cp;}
    if(cache[cache_slot])return cache[cache_slot];
    if(FT_Load_Char(face,cp,FT_LOAD_RENDER))return NULL;
    FT_GlyphSlot slot=face->glyph;
    Glyph *g=calloc(1,sizeof(*g));if(!g)return NULL;
    g->width=(int)slot->bitmap.width;g->height=(int)slot->bitmap.rows;
    g->left=slot->bitmap_left;g->top=slot->bitmap_top;g->advance=(int)(slot->advance.x>>6);
    g->data=malloc((size_t)g->width*(size_t)g->height+1);if(!g->data){free(g);return NULL;}
    for(int y=0;y<g->height;y++)memcpy(g->data+y*g->width,slot->bitmap.buffer+y*slot->bitmap.pitch,(size_t)g->width);
    cache[cache_slot]=g;return g;
}
static void paint_style(int x,int y,Glyph *g,unsigned color,int left,int top,int right,int bottom,unsigned style) {
    if(!g || !frame)return;
    for(int gy=0;gy<g->height;gy++)for(int gx=0;gx<g->width;gx++) {
        int px=x+g->left+gx+((style&ST_MD_ITALIC)?(g->height-gy)/5:0),py=y+15-g->top+gy;
        if(px<0||px>=480||py<0||py>=272 || px<left||px>=right||py<top||py>=bottom||px<clip_l||px>=clip_r||py<clip_t||py>=clip_b)continue;
        if(gradient_active)color=gradient_colour(px,py);
        unsigned a=g->data[gy*g->width+gx]*((color>>24)&255)/255;if(!a)continue;
        unsigned bg=frame[py*stride+px],v=0xff000000u;
        for(int s=0;s<24;s+=8)v|=((((color>>s)&255)*a+((bg>>s)&255)*(255-a))/255)<<s;
        frame[py*stride+px]=v;
    }
}
static void paint(int x,int y,Glyph *g,unsigned color,int left,int top,int right,int bottom){paint_style(x,y,g,color,left,top,right,bottom,0);}
int ui_draw_init(const char *font) {
    st_theme_apply(st_theme_dark,st_theme_accent);
    if(FT_Init_FreeType(&library))return 0;
    if(FT_New_Face(library,font,0,&face)){FT_Done_FreeType(library);library=NULL;return 0;}
    if(FT_Set_Pixel_Sizes(face,0,15)){FT_Done_Face(face);FT_Done_FreeType(library);face=NULL;library=NULL;return 0;}
    return 1;
}
void ui_draw_frame(unsigned *pixels,int row_stride){frame=pixels;stride=row_stride;ui_clip_reset();}
void ui_rect(int x,int y,int width,int height,unsigned color) {
    if(!frame || width<=0 || height<=0)return;
    int right=x+width,bottom=y+height;
    if(x<clip_l)x=clip_l;
    if(y<clip_t)y=clip_t;
    if(right>clip_r)right=clip_r;
    if(bottom>clip_b)bottom=clip_b;
    for(int row=y;row<bottom;row++)for(int col=x;col<right;col++)frame[row*stride+col]=color;
}
static void coverage_pixel(int x,int y,unsigned color,unsigned coverage) {
    if(!frame||!coverage||x<clip_l||x>=clip_r||y<clip_t||y>=clip_b)return;
    unsigned bg=frame[y*stride+x],v=0xff000000;
    for(int shift=0;shift<24;shift+=8)v|=((((color>>shift)&255)*coverage+((bg>>shift)&255)*(4-coverage))/4)<<shift;
    frame[y*stride+x]=v;
}
void ui_circle(int x,int y,int diameter,unsigned color) {
    if(diameter<1||diameter>192)return;
    int radius2=diameter*diameter*4;
    for(int dy=0;dy<diameter;dy++)for(int dx=0;dx<diameter;dx++) {
        int rx=4*dx+2-2*diameter,ry=4*dy+2-2*diameter;unsigned coverage=0;
        for(int sy=-1;sy<=1;sy+=2)for(int sx=-1;sx<=1;sx+=2)coverage+=(rx+sx)*(rx+sx)+(ry+sy)*(ry+sy)<=radius2;
        coverage_pixel(x+dx,y+dy,color,coverage);
    }
}
void ui_round_rect(int x,int y,int width,int height,int radius,unsigned color) {
    if(width<1||height<1)return;
    if(radius<0)radius=0;
    if(radius>width/2)radius=width/2;
    if(radius>height/2)radius=height/2;
    for(int dy=0;dy<height;dy++)for(int dx=0;dx<width;dx++) {
        int cx=dx<radius?2*radius-(2*dx+1):dx>=width-radius?2*dx+1-2*(width-radius):0;
        int cy=dy<radius?2*radius-(2*dy+1):dy>=height-radius?2*dy+1-2*(height-radius):0;
        if(cx*cx+cy*cy<=4*radius*radius)ui_rect(x+dx,y+dy,1,1,color);
    }
}
void ui_presence(int x,int y,int size,enum StPresence presence,unsigned background) {
    unsigned color=presence==ST_ONLINE?UI_ONLINE:presence==ST_IDLE?UI_IDLE:presence==ST_FOCUS?UI_FOCUS:presence==ST_BUSY?UI_BUSY:UI_OFFLINE;
    ui_circle(x,y,size,color);
    if(presence==ST_INVISIBLE||presence==ST_OFFLINE)ui_circle(x+2,y+2,size-4,background);
    else if(presence==ST_IDLE)ui_circle(x+size/3,y,size*2/3,background);
    else if(presence==ST_BUSY)ui_rect(x+2,y+size/2-1,size-4,2,background);
    else if(presence==ST_FOCUS){ui_rect(x+2,y+size/2-1,size-4,2,background);ui_rect(x+size/2-1,y+2,2,size-4,background);}
}
static int stroke(int x,int y,int ax,int ay,int bx,int by) {
    int vx=bx-ax,vy=by-ay,wx=x-ax,wy=y-ay,length=vx*vx+vy*vy,along=wx*vx+wy*vy,cross=wx*vy-wy*vx;
    if(along<0)return wx*wx+wy*wy==0;
    if(along>length){wx=x-bx;wy=y-by;return wx*wx+wy*wy==0;}
    return 4*cross*cross<=length;
}
static int icon_ring(int x,int y,int cx,int cy,int radius){int dx=2*(x-cx)+1,dy=2*(y-cy)+1,r=dx*dx+dy*dy;return r>=4*(radius-1)*(radius-1)&&r<=4*radius*radius;}
static int icon_pixel(enum UiIcon icon,int x,int y) {
#define L(a,b,c,d) stroke(x,y,a,b,c,d)
#define R(a,b,c) icon_ring(x,y,a,b,c)
    switch(icon) {
        case UI_ICON_MESSAGE:return L(2,2,13,2)||L(13,2,13,10)||L(13,10,7,10)||L(7,10,3,14)||L(3,14,3,10)||L(3,10,2,10)||L(2,10,2,2)||L(5,5,10,5)||L(5,7,9,7);
        case UI_ICON_PEOPLE:return R(5,4,3)||R(11,5,2)||L(1,14,1,11)||L(1,11,4,9)||L(4,9,7,9)||L(7,9,10,11)||L(10,11,10,14)||L(12,10,14,12)||L(14,12,14,14);
        case UI_ICON_USER:case UI_ICON_ADD_USER:case UI_ICON_REMOVE_USER:return R(6,4,3)||L(1,14,1,11)||L(1,11,4,9)||L(4,9,8,9)||L(8,9,10,11)||L(10,11,10,14)||(icon!=UI_ICON_USER&&L(11,7,15,7))||(icon==UI_ICON_ADD_USER&&L(13,5,13,9));
        case UI_ICON_SETTINGS:return R(8,8,5)||R(8,8,2)||L(8,0,8,3)||L(8,12,8,15)||L(0,8,3,8)||L(12,8,15,8)||L(2,2,4,4)||L(12,12,14,14)||L(2,14,4,12)||L(12,4,14,2);
        case UI_ICON_CACHE:return L(2,2,13,2)||L(2,3,2,13)||L(13,3,13,13)||L(2,6,13,6)||L(2,10,13,10)||L(2,14,13,14)||(x==11&&(y==4||y==8||y==12));
        case UI_ICON_PALETTE:return R(8,8,7)||R(4,7,1)||R(6,4,1)||R(10,4,1)||R(12,7,1)||L(5,12,8,12);
        case UI_ICON_DEVICES:return L(1,2,14,2)||L(1,2,1,10)||L(14,2,14,10)||L(1,10,14,10)||L(8,10,8,14)||L(4,14,12,14);
        case UI_ICON_TEXT:return L(2,3,13,3)||L(2,7,13,7)||L(2,11,9,11);
        case UI_ICON_TRASH:return L(2,3,13,3)||L(5,1,10,1)||L(3,5,4,14)||L(12,5,11,14)||L(4,14,11,14)||L(6,6,6,11)||L(9,6,9,11);
        case UI_ICON_LIMIT:return R(8,8,6)||L(8,3,8,8)||L(8,8,12,10);
        case UI_ICON_SUN:return R(8,8,3)||L(8,0,8,2)||L(8,13,8,15)||L(0,8,2,8)||L(13,8,15,8)||L(2,2,3,3)||L(12,12,14,14)||L(2,14,3,13)||L(12,3,14,1);
        case UI_ICON_MOON:return (R(8,8,6)&&!(x>6&&y<9))||L(5,2,5,7)||L(5,7,9,10)||L(9,10,14,10);
        case UI_ICON_COLOUR:return R(8,8,6)||R(8,8,3);
        case UI_ICON_EMAIL:return L(1,3,14,3)||L(1,3,1,12)||L(14,3,14,12)||L(1,12,14,12)||L(1,3,8,8)||L(8,8,14,3);
        case UI_ICON_KEY:return R(5,5,4)||L(8,8,14,14)||L(12,12,14,10)||L(14,14,15,12);
        case UI_ICON_INVITE:return L(2,3,13,3)||L(2,3,2,12)||L(13,3,13,12)||L(2,12,13,12)||L(8,5,8,10)||L(5,8,11,8);
        case UI_ICON_LOGIN:case UI_ICON_LOGOUT:return L(8,1,14,1)||L(14,1,14,14)||L(8,14,14,14)||L(1,8,10,8)||(icon==UI_ICON_LOGIN?(L(7,5,10,8)||L(10,8,7,11)):(L(4,5,1,8)||L(1,8,4,11)));
        case UI_ICON_BLOCK:case UI_ICON_UNBLOCK:return R(8,8,6)||(icon==UI_ICON_BLOCK?L(4,12,12,4):(L(4,8,7,11)||L(7,11,12,5)));
        case UI_ICON_FLAG:return L(3,1,3,15)||L(3,2,12,2)||L(12,2,10,5)||L(10,5,12,8)||L(12,8,3,8);
        case UI_ICON_SERVERS:return L(2,2,13,2)||L(2,2,2,6)||L(13,2,13,6)||L(2,6,13,6)||L(2,9,13,9)||L(2,9,2,13)||L(13,9,13,13)||L(2,13,13,13)||(x==4&&(y==4||y==11));
        case UI_ICON_VOICE:case UI_ICON_AUDIO_OFF:return L(1,6,4,6)||L(4,6,8,2)||L(8,2,8,14)||L(8,14,4,10)||L(4,10,1,10)||L(1,10,1,6)||(icon==UI_ICON_VOICE?(L(11,4,14,7)||L(14,7,14,9)||L(14,9,11,12)):L(10,5,15,11));
        case UI_ICON_MIC:case UI_ICON_MIC_OFF:return L(6,2,9,2)||L(6,2,6,8)||L(9,2,9,8)||L(6,8,9,8)||L(3,6,3,9)||L(3,9,6,12)||L(6,12,9,12)||L(9,12,12,9)||L(12,9,12,6)||L(8,12,8,15)||(icon==UI_ICON_MIC_OFF&&L(1,1,14,14));
        case UI_ICON_HASH:return L(6,1,4,14)||L(11,1,9,14)||L(2,5,14,5)||L(1,10,13,10);
        case UI_ICON_FOLDER:return L(1,4,1,13)||L(1,13,14,13)||L(14,13,14,5)||L(14,5,7,5)||L(7,5,5,3)||L(5,3,1,3)||L(1,3,1,4)||L(1,6,14,6);
        case UI_ICON_FILE:return L(3,1,9,1)||L(9,1,13,5)||L(13,5,13,14)||L(13,14,3,14)||L(3,14,3,1)||L(9,1,9,5)||L(9,5,13,5);
        case UI_ICON_IMAGE:return L(1,2,14,2)||L(1,2,1,13)||L(14,2,14,13)||L(1,13,14,13)||L(2,12,6,7)||L(6,7,10,12)||L(9,10,12,8)||L(12,8,14,11)||R(11,5,1);
        case UI_ICON_ONLINE:return R(8,8,5);
        case UI_ICON_FOCUS:return R(8,8,6)||L(8,4,8,12)||L(4,8,12,8);
        case UI_ICON_BUSY:return R(8,8,6)||L(4,8,12,8);
        case UI_ICON_INVISIBLE:return R(8,8,3)||L(1,8,4,4)||L(4,4,11,4)||L(11,4,15,8)||L(1,8,4,12)||L(4,12,11,12)||L(11,12,15,8)||L(1,14,14,1);
        default:return 0;
    }
#undef L
#undef R
}
void ui_icon(enum UiIcon icon,int x,int y,unsigned color) {
    for(int dy=0;dy<16;dy++)for(int dx=0;dx<16;dx++)if(icon_pixel(icon,dx,dy))ui_rect(x+dx,y+dy,1,1,color);
}

void ui_icon_size(enum UiIcon icon,int x,int y,int size,unsigned color){
    if(size<1||size>96)return;
    for(int dy=0;dy<size;dy++)for(int dx=0;dx<size;dx++)if(icon_pixel(icon,dx*16/size,dy*16/size))ui_rect(x+dx,y+dy,1,1,color);
}

static int key_pixel(unsigned key,int x,int y) {
    if(key==1)return stroke(x,y,4,5,11,12)||stroke(x,y,11,5,4,12);
    if(key==2)return icon_ring(x,y,8,9,5);
    if(key==3)return stroke(x,y,4,5,11,5)||stroke(x,y,11,5,11,12)||stroke(x,y,11,12,4,12)||stroke(x,y,4,12,4,5);
    if(key==4)return stroke(x,y,8,4,3,12)||stroke(x,y,3,12,12,12)||stroke(x,y,12,12,8,4);
    if(key==11)return stroke(x,y,4,5,4,12)||stroke(x,y,4,5,2,7)||stroke(x,y,4,5,6,7)||stroke(x,y,11,5,11,12)||stroke(x,y,11,12,9,10)||stroke(x,y,11,12,13,10);
    if(key==5)return stroke(x,y,5,5,5,12)||stroke(x,y,5,12,11,12);
    if(key==6)return stroke(x,y,5,5,5,12)||stroke(x,y,5,5,11,5)||stroke(x,y,11,5,11,8)||stroke(x,y,11,8,5,8)||stroke(x,y,7,8,11,12);
    if(key==7)return x>=6&&x<=11&&abs(y-9)<=((11-x)/2);
    if(key==8)return x>=5&&x<=11&&y>=7&&y<=10;
    if(key==14)return icon_ring(x,y,8,9,6)||((x-7)*(x-7)+(y-8)*(y-8)<6);
    if(key==12)return stroke(x,y,3,8,12,8)||stroke(x,y,8,4,12,8)||stroke(x,y,12,8,8,12);
    return 0;
}
void ui_loader(int x,int y,int width,const char *label) {
    unsigned phase=(unsigned)(st_millis()/160)%4;
    for(int i=0;i<4;i++)ui_circle(x+i*7,y+7,4,(unsigned)i==phase?UI_ACCENT:UI_DIM);
    ui_text_clip(x+32,y,width-32,label,UI_DIM);
}
static const StClient *mention_client;
static char mention_server[64];static int mention_width=176;
static struct {char id[64];uint64_t retry;int wanted;} mention_requests[16];
void ui_mentions_reset(void){memset(mention_requests,0,sizeof(mention_requests));mention_client=NULL;mention_server[0]=0;}
void ui_mentions(const StClient *client,const char *server){mention_client=client;st_copy(mention_server,sizeof(mention_server),server?server:"");for(int i=0;i<16;i++)mention_requests[i].wanted=0;}
static int mention_known(const char *id){if(mention_client)for(size_t i=0;i<mention_client->user_count;i++)if(!strcmp(mention_client->users[i].id,id)&&mention_client->users[i].name[0])return 1;return 0;}
static void mention_want(const char *id) {
    if(mention_known(id))return;
    for(int i=0;i<16;i++)if(!strcmp(mention_requests[i].id,id)){mention_requests[i].wanted=1;return;}
    for(int i=0;i<16;i++)if(!mention_requests[i].id[0]||(!mention_requests[i].wanted&&st_millis()>=mention_requests[i].retry)){
        st_copy(mention_requests[i].id,64,id);mention_requests[i].retry=0;mention_requests[i].wanted=1;return;
    }
}
int ui_mention_next(char id[64]) {
    for(int i=0;i<16;i++)if(mention_requests[i].wanted&&st_millis()>=mention_requests[i].retry&&!mention_known(mention_requests[i].id)) {
        st_copy(id,64,mention_requests[i].id);mention_requests[i].retry=st_millis()+60000;return 1;
    }
    return 0;
}
typedef struct { uint32_t cp;Glyph *glyph;int advance,emoji,key,mention;char id[64],label[129]; } TextToken;
static TextToken token_next(const char **text) {
    TextToken t={0};
    unsigned char key=(unsigned char)**text;
    if((key>=1&&key<=8)||key==11||key==12||key==14){(*text)++;t.key=key;t.advance=18;return t;}
    if(st_custom_emoji_match(text,t.id)){t.emoji=2;t.advance=18;return t;}
    if(st_emoji_match(text,t.id)){t.emoji=1;t.advance=18;return t;}
    t.cp=st_utf8_next(text);
    if(t.cp=='\t'){Glyph *space=glyph(' ');t.advance=space?space->advance*4:16;return t;}
    if(t.cp=='\n'||t.cp=='\r'||t.cp==0xfe0e||t.cp==0xfe0f||t.cp==0x200d)return t;
    t.glyph=glyph(t.cp);t.advance=t.glyph?t.glyph->advance:9;return t;
}
static void paint_token(int x,int y,const TextToken *t,unsigned color,int left,int top,int right,int bottom) {
    if(t->mention) {
        int width=t->advance;if(width>right-x)width=right-x;
        if(width<1||y<top||y+18>bottom)return;
        if(t->mention==1)mention_want(t->id);
        ui_round_rect(x,y+1,width,17,5,UI_REACTION);
        int label_x=x+5;
        if(t->mention==2){ui_icon(UI_ICON_HASH,x+3,y+1,UI_ACCENT);label_x=x+22;}
        else if(width>44) {
            StMediaRequest key={.tag="avatars"};const StUser *user=NULL;
            if(mention_client)for(size_t i=0;i<mention_client->user_count;i++)if(!strcmp(mention_client->users[i].id,t->id)){user=mention_client->users+i;break;}
            if(user){st_copy(key.id,64,user->avatar);st_copy(key.filename,sizeof(key.filename),user->avatar_file);key.animated=user->avatar_animated;}
            if(!key.id[0]){st_copy(key.tag,sizeof(key.tag),"default");st_copy(key.id,64,t->id);}
            ui_image_ref(&key,x+2,y+1,16,16,1);label_x=x+21;
        }
        const StRole *role=t->mention==1&&mention_client?st_member_role(mention_client,mention_server,t->id,0):NULL;
        const char *colour=role?role->colour:NULL;
        ui_role_text_clip(label_x,y,x+width-label_x-4,t->label,colour,UI_ACCENT);return;
    }
    if(t->key){
        static const unsigned colours[]={0,0xffe8a45cu,0xff7a6ef3u,0xffd98fc7u,0xffacdc72u};
        unsigned ink=t->key<=4?colours[t->key]:0xffeeeeeeu;
        for(int dy=1;dy<17;dy++)for(int dx=0;dx<16;dx++) {
            int rx=2*dx+1-16,ry=2*(dy-1)+1-16;
            if(rx*rx+ry*ry>256)continue;
            if(x+dx>=left&&x+dx<right&&y+dy>=top&&y+dy<bottom)ui_rect(x+dx,y+dy,1,1,key_pixel((unsigned)t->key,dx,dy)?ink:0xff37312cu);
        }
        return;
    }
    if(!t->emoji){paint(x,y,t->glyph,color,left,top,right,bottom);return;}
    if(image_area(t->emoji==2?"emojis":"emoji",t->id,x,y,18,18,0,left,top,right,bottom))return;
    /* Keep cluster width while a worker loads its pixels; never split/reflow it. */
    for(int dy=3;dy<15;dy++)for(int dx=3;dx<15;dx++) {
        int px=x+dx,py=y+dy;
        if(px<left||px>=right||py<top||py>=bottom)continue;
        if(dx==3||dx==14||dy==3||dy==14)ui_rect(px,py,1,1,UI_DIM);
    }
}
static int measure(const char *text) {
    int width=0;
    while(*text && *text!='\n'){TextToken t=token_next(&text);width+=t.advance;}
    return width;
}
int ui_text_width(const char *text){return measure(text);}
void ui_text_clip(int x,int y,int width,const char *text,unsigned color) {
    if(width<=0)return;
    int end=x+width,trim=measure(text)>width,limit=end-(trim?measure("..."):0),start=x;
    while(*text && *text!='\n') {
        TextToken t=token_next(&text);if(x+t.advance>limit)break;
        paint_token(x,y,&t,color,start,y,end,y+19);x+=t.advance;
    }
    if(trim)for(int i=0;i<3;i++){Glyph *g=glyph('.');paint(x,y,g,color,start,y,end,y+19);x+=g?g->advance:4;}
}
void ui_text(int x,int y,const char *text,unsigned color){ui_text_clip(x,y,470-x,text,color);}
void ui_role_text_clip(int x,int y,int width,const char *text,const char *style,unsigned fallback) {
    if(style&&st_role_gradient(style,&role_gradient)) {
        gradient_x=x;gradient_y=y;gradient_width=measure(text);if(gradient_width>width)gradient_width=width;
        if(gradient_width<1)gradient_width=1;
        gradient_active=1;ui_text_clip(x,y,width,text,fallback);gradient_active=0;
    }else {unsigned color=fallback;if(style)st_role_colour(style,&color);ui_text_clip(x,y,width,text,color);}
}
void ui_title(const char *title){ui_text(10,4,title,UI_ACCENT);}
void ui_footer(const char *text){ui_text_clip(8,252,464,text,UI_DIM);}
int ui_wrap_area(int x,int y,int width,const char *text,int skip,int lines,unsigned color) {
    if(width<=0)return 0;
    int row=0,cx=x,word_start=1;
    while(*text) {
        const char *start=text;
        TextToken t=token_next(&text);uint32_t cp=t.cp;
        if(cp=='\n'){row++;cx=x;word_start=1;continue;}
        if(cp=='\r')continue;
        if(word_start && cp!=' ' && cp!='\t') {
            const char *word=start;int word_width=0;
            while(*word && *word!=' ' && *word!='\t' && *word!='\n' && *word!='\r') {
                TextToken next=token_next(&word);word_width+=next.advance;
            }
            if(cx>x && cx+word_width>x+width){row++;cx=x;}
        }
        word_start=cp==' '||cp=='\t';
        if(word_start && cx==x)continue;
        if(cx>x && cx+t.advance>x+width){row++;cx=x;}
        if(word_start && cx==x)continue;
        if(row>=skip && row<skip+lines)
            paint_token(cx,y+(row-skip)*19,&t,color,x,y,x+width,y+lines*19);
        cx+=t.advance;
    }
    return row+1;
}
int ui_line_count(const char *text,int width){return ui_wrap_area(0,0,width,text,0,0,UI_WHITE);}
static TextToken rich_token(const char **p,const StMarkdown *m,unsigned *style) {
    *style=m->style[*p-m->text];TextToken t;
    if(*style&ST_MD_CODE){memset(&t,0,sizeof(t));if(st_emoji_match(p,t.id)){t.emoji=1;t.advance=18;}else{t.cp=st_utf8_next(p);if(t.cp=='\t'){Glyph *space=glyph(' ');t.advance=space?space->advance*4:16;}else if(t.cp!='\n'&&t.cp!='\r'){t.glyph=glyph(t.cp);t.advance=t.glyph?t.glyph->advance:9;}}}
    else {
        memset(&t,0,sizeof(t));
        if(!(*style&ST_MD_LITERAL)&&(st_mention_match(p,t.id)?(t.mention=1):st_channel_mention_match(p,t.id)?(t.mention=2):0)) {
            const char *name=t.mention==1?(mention_client?st_context_name(mention_client,mention_server,t.id):t.id):"Unknown channel";
            if(t.mention==2&&mention_client)for(size_t i=0;i<mention_client->channel_count;i++)if(!strcmp(mention_client->channels[i].id,t.id)){name=st_channel_name(mention_client,mention_client->channels+i);break;}
            st_copy(t.label,sizeof(t.label),t.mention==1&&!strcmp(name,t.id)?"Unknown User":name);
            t.advance=ui_text_width(t.label)+27;if(t.advance>mention_width)t.advance=mention_width;
        }else t=token_next(p);
    }
    if(*style&ST_MD_BOLD)t.advance++;
    return t;
}
static void paint_rich(int x,int y,const TextToken *t,unsigned style,unsigned color,int left,int top,int right,int bottom){
    unsigned ink=style&(ST_MD_HEADING|ST_MD_LINK)?UI_ACCENT:color;
    if(style&ST_MD_CODE)ui_rect(x,y+1,t->advance,17,UI_SIDE);
    if(!t->emoji&&!t->key&&!t->mention){paint_style(x,y,t->glyph,ink,left,top,right,bottom,style);if(style&ST_MD_BOLD)paint_style(x+1,y,t->glyph,ink,left,top,right,bottom,style);}
    else paint_token(x,y,t,ink,left,top,right,bottom);
    int length=t->advance;if(x+length>right)length=right-x;
    if(style&ST_MD_STRIKE)ui_rect(x,y+10,length,1,ink);
    if(style&ST_MD_LINK)ui_rect(x,y+17,length,1,ink);
}
static TextToken inline_token(const char **p,const StMarkdown *m,unsigned *style){
    TextToken t=rich_token(p,m,style);
    if(t.cp=='\n'||t.cp=='\r'){memset(&t,0,sizeof(t));t.cp=' ';t.glyph=glyph(' ');t.advance=t.glyph?t.glyph->advance:4;}
    return t;
}
void ui_markdown_inline(int x,int y,int width,const char *text,unsigned color){
    if(width<=0)return;
    StMarkdown m;st_markdown_parse(text,&m);mention_width=width<176?width:176;
    const char *p=m.text;int total=0;while(*p){unsigned style;TextToken t=inline_token(&p,&m,&style);total+=t.advance;}
    int ellipsis=ui_text_width("..."),trim=total>width,right=x+width,limit=trim?right-ellipsis:right,at=x;
    mention_width=limit-x<176?limit-x:176;if(mention_width<0)mention_width=0;
    p=m.text;while(*p){unsigned style;TextToken t=inline_token(&p,&m,&style);if(at+t.advance>limit)break;paint_rich(at,y,&t,style,color,x,y,limit,y+19);at+=t.advance;}
    if(trim)ui_text_clip(limit<x?x:limit,y,width<ellipsis?width:ellipsis,"...",color);
}
int ui_markdown_area(int x,int y,int width,const char *text,int skip,int lines,unsigned color) {
    if(width<=0)return 0;
    mention_width=width<176?width:176;
    StMarkdown m;st_markdown_parse(text,&m);const char *p=m.text;
    int row=0,cx=x,word_start=1;
    while(*p) {
        const char *start=p;unsigned style;TextToken t=rich_token(&p,&m,&style);
        int left=x+((style&ST_MD_QUOTE)?8:0),right=x+width;
        if(t.cp=='\n'){row++;cx=x;word_start=1;continue;}if(t.cp=='\r')continue;
        if(cx==x)cx=left;
        if(word_start&&t.cp!=' '&&t.cp!='\t') {
            const char *wordp=start;int length=0;
            while(*wordp&&*wordp!=' '&&*wordp!='\t'&&*wordp!='\n'&&*wordp!='\r'){unsigned flags;TextToken word=rich_token(&wordp,&m,&flags);length+=word.advance;}
            if(cx>left&&cx+length>right){row++;cx=left;}
        }
        word_start=t.cp==' '||t.cp=='\t';if(word_start&&cx==left&&!(style&ST_MD_CODE))continue;
        if(cx>left&&cx+t.advance>right){row++;cx=left;}if(word_start&&cx==left&&!(style&ST_MD_CODE))continue;
        if(row>=skip&&row<skip+lines) {
            int yy=y+(row-skip)*19;
            if(style&ST_MD_QUOTE)ui_rect(x,yy+1,2,17,UI_ACCENT);
            paint_rich(cx,yy,&t,style,color,left,y,right,y+lines*19);
        }
        cx+=t.advance;
    }
    return row+1;
}
int ui_markdown_lines(const char *text,int width){return ui_markdown_area(0,0,width,text,0,0,UI_WHITE);}
int ui_wrap(int x,int y,const char *text,int skip,int lines,unsigned color){
    return ui_wrap_area(x,y,468-x,text,skip,lines,color);
}
void ui_draw_term(void) {
    for(size_t i=0;i<sizeof(cache)/sizeof(*cache);i++)if(cache[i]){free(cache[i]->data);free(cache[i]);cache[i]=NULL;}
    if(face)FT_Done_Face(face);
    if(library)FT_Done_FreeType(library);
    face=NULL;library=NULL;frame=NULL;images=NULL;ui_mentions_reset();
}
