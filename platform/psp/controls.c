#include "ui.h"
#include <pspctrl.h>
#include <psputility_sysparam.h>

static unsigned old_buttons;
static int analog_y;
int ui_analog_y(void){return analog_y;}
static int button_swap=1;
void ui_controls_init(void) {
    int configured=1; button_swap=1;
    if(sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP,&configured)>=0 &&
       (configured==0 || configured==1))button_swap=configured;
    ui_buttons_release();
}
int ui_button_swap(void){return button_swap;}
unsigned ui_confirm_button(void){return button_swap?PSP_CTRL_CROSS:PSP_CTRL_CIRCLE;}
unsigned ui_cancel_button(void){return button_swap?PSP_CTRL_CIRCLE:PSP_CTRL_CROSS;}
const char *ui_confirm_label(void){return button_swap?UI_KEY_CROSS:UI_KEY_CIRCLE;}
const char *ui_cancel_label(void){return button_swap?UI_KEY_CIRCLE:UI_KEY_CROSS;}
unsigned ui_buttons(void) {
    SceCtrlData pad;
    if(sceCtrlReadBufferPositive(&pad,1)<1)return 0;
    analog_y=(int)pad.Ly-128;
    unsigned changed=pad.Buttons&~old_buttons;old_buttons=pad.Buttons;return changed;
}
void ui_buttons_release(void) {
    SceCtrlData pad;
    if(sceCtrlReadBufferPositive(&pad,1)>0)old_buttons=pad.Buttons;
}
