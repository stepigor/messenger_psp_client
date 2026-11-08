#include "stoat.h"
#include "ui.h"
#include <pspkernel.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspctrl.h>
#include <psputility.h>
#include <pspnet_apctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int __attribute__((aligned(16))) list[8192];
static void *back;
static uint64_t frame_started;
int ui_init(const char *font) {
    if(!ui_draw_init(font))return 0;
    ui_controls_init();
    sceGuInit();sceGuStart(GU_DIRECT,list);
    sceGuDrawBuffer(GU_PSM_8888,(void *)0,512);sceGuDispBuffer(480,272,(void *)(512*272*4),512);
    sceGuOffset(2048-240,2048-136);sceGuViewport(2048,2048,480,272);
    sceGuScissor(0,0,480,272);sceGuEnable(GU_SCISSOR_TEST);sceGuDisable(GU_DEPTH_TEST);
    sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuDisplay(GU_TRUE);
    return 1;
}
void ui_begin(void) {
    frame_started=(uint64_t)sceKernelGetSystemTimeWide();
    sceGuStart(GU_DIRECT,list);sceGuClearColor(UI_BG);sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();sceGuSync(0,0);
    ui_draw_frame((unsigned *)(((uintptr_t)sceGeEdramGetAddr()+(uintptr_t)back)|0x40000000u),512);
}
void ui_end(void) {
    uint64_t rendered=(uint64_t)sceKernelGetSystemTimeWide()-frame_started;
    sceDisplayWaitVblankStart();back=sceGuSwapBuffers();
    ui_background_yield(rendered>50000?50000:(unsigned)rendered);
}
static void dialog_base(pspUtilityDialogCommon *b,size_t size) {
    memset(b,0,sizeof(*b));b->size=(unsigned)size;
    b->language=PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    b->buttonSwap=ui_button_swap();
    b->graphicsThread=17;b->accessThread=19;b->fontThread=18;b->soundThread=16;
}
int ui_osk(const char *title,char *text,size_t cap,int max_units) {
    uint16_t desc[128],in[2001],out[2001];
    st_utf8_to_utf16(title,desc,128);st_utf8_to_utf16(text,in,2001);memset(out,0,sizeof(out));
    SceUtilityOskData data;memset(&data,0,sizeof(data));
    data.language=PSP_UTILITY_OSK_LANGUAGE_RUSSIAN;
    data.lines=1;data.unk_24=1;data.inputtype=PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc=desc;data.intext=in;data.outtext=out;data.outtextlength=2001;data.outtextlimit=max_units;
    SceUtilityOskParams params;memset(&params,0,sizeof(params));dialog_base(&params.base,sizeof(params));params.datacount=1;params.data=&data;
    if(max_units<1 || max_units>2000 || sceUtilityOskInitStart(&params)<0){st_wipe(in,sizeof(in));st_wipe(out,sizeof(out));return 0;}
    int started=0,shutdown=0;
    while(1) {
        ui_background();ui_begin();ui_title(title);
        int status=sceUtilityOskGetStatus();
        if(status!=PSP_UTILITY_DIALOG_NONE)started=1;
        if(status==PSP_UTILITY_DIALOG_VISIBLE)sceUtilityOskUpdate(1);
        if(status==PSP_UTILITY_DIALOG_QUIT && !shutdown){sceUtilityOskShutdownStart();shutdown=1;}
        ui_end();if(started && status==PSP_UTILITY_DIALOG_NONE)break;
    }
    int ok=app_running && data.result!=PSP_UTILITY_OSK_RESULT_CANCELLED;
    char converted[ST_TEXT_MAX]={0};
    if(ok)ok=st_utf16_to_utf8(out,converted,sizeof(converted)) && strlen(converted)<cap;
    if(ok)memcpy(text,converted,strlen(converted)+1);
    st_wipe(converted,sizeof(converted));
    st_wipe(in,sizeof(in));st_wipe(out,sizeof(out));ui_buttons_release();return ok;
}
int ui_network(void) {
    pspUtilityNetconfData data;memset(&data,0,sizeof(data));dialog_base(&data.base,sizeof(data));
    data.action=PSP_NETCONF_ACTION_CONNECTAP;
    if(sceUtilityNetconfInitStart(&data)<0)return 0;
    int started=0,shutdown=0;
    while(1) {
        ui_begin();ui_title("Connect to Wi-Fi");int status=sceUtilityNetconfGetStatus();
        if(status!=PSP_UTILITY_DIALOG_NONE)started=1;
        if(status==PSP_UTILITY_DIALOG_VISIBLE)sceUtilityNetconfUpdate(1);
        if(status==PSP_UTILITY_DIALOG_QUIT && !shutdown){sceUtilityNetconfShutdownStart();shutdown=1;}
        ui_end();if(started && status==PSP_UTILITY_DIALOG_NONE)break;
    }
    ui_buttons_release();int state=0;sceNetApctlGetState(&state);return app_running && state==4;
}
void ui_term(void) {ui_draw_term();sceGuTerm();}
