/* The user-facing microphone utility is a live loopback only. */
#include "voice_test.h"
#include "call.h"
#include "call_view.h"
#include "ui.h"
void st_voice_test_poll(void){st_call_poll();}
void st_voice_test_open(void) {
    ui_voice_pause(1);
    while(app_running&&!ui_voice_idle()) {ui_background();ui_begin();ui_startup(NULL,1000,"Preparing microphone...");ui_end();if(ui_buttons()&ui_cancel_button()){ui_voice_pause(0);return;}}
    if(!st_call_loopback()){ui_voice_pause(0);return;}
    while(app_running) {
        ui_background();st_call_poll();StRtcStats stats=st_call_snapshot();StCallView view={.stats=&stats,.loopback=1,.mic_peak=st_call_mic_peak()};
        ui_begin();st_call_view_draw(&view,ui_confirm_label(),ui_cancel_label());if(stats.state==ST_RTC_FAILED)ui_text_clip(20,227,440,stats.error,UI_BUSY);ui_end();
        if(ui_buttons()&ui_cancel_button())break;
    }
    st_call_stop();ui_voice_pause(0);ui_buttons_release();
}
