#ifndef ST_THEME_H
#define ST_THEME_H
#include "stoat.h"
#define ST_THEME_SETTINGS "theme-settings.json"
#define ST_THEME_COLORS 7
typedef struct {
    unsigned text,dim,accent,bg,rail,side,selected,line,reaction,control_bg,control_fg;
} StThemeColors;
extern StThemeColors st_theme;
extern int st_theme_dark,st_theme_accent;
extern const char *const st_theme_names[ST_THEME_COLORS];
extern const unsigned st_theme_seeds[ST_THEME_COLORS];
void st_theme_apply(int dark,int accent);
int st_theme_load(const char *path,StError *err);
int st_theme_save(const char *path,StError *err);
#endif
