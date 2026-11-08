#ifndef ST_UI_H
#define ST_UI_H
#include <stddef.h>
#include "ui_draw.h"
int ui_init(const char *font);
void ui_begin(void);
void ui_end(void);
void ui_background(void);
void ui_voice_pause(int paused);
int ui_voice_idle(void);
void ui_background_yield(unsigned render_us);
void ui_term(void);
unsigned ui_buttons(void);
int ui_analog_y(void);
void ui_controls_init(void);
void ui_buttons_release(void);
int ui_button_swap(void);
int ui_osk(const char *title,char *text,size_t cap,int max_units);
int ui_network(void);
unsigned ui_confirm_button(void);
unsigned ui_cancel_button(void);
const char *ui_confirm_label(void);
const char *ui_cancel_label(void);
extern volatile int app_running;
#endif
