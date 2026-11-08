#ifndef ST_UI_DRAW_H
#define ST_UI_DRAW_H
#include "media.h"
#include "theme.h"
#define UI_WHITE (st_theme.text)
#define UI_DIM (st_theme.dim)
#define UI_ACCENT (st_theme.accent)
#define UI_BG (st_theme.bg)
#define UI_RAIL (st_theme.rail)
#define UI_SIDE (st_theme.side)
#define UI_SELECTED (st_theme.selected)
#define UI_LINE (st_theme.line)
#define UI_REACTION (st_theme.reaction)
#define UI_ONLINE 0xff71b657u
#define UI_IDLE 0xff56bcf0u
#define UI_FOCUS 0xffda9a86u
#define UI_BUSY 0xff5a5befu
#define UI_OFFLINE 0xffaaa39au
/* Internal hint tokens, drawn as coloured symbols on their own backgrounds. */
#define UI_KEY_CROSS "\x01"
#define UI_KEY_CIRCLE "\x02"
#define UI_KEY_SQUARE "\x03"
#define UI_KEY_TRIANGLE "\x04"
#define UI_KEY_L "\x05"
#define UI_KEY_R "\x06"
#define UI_KEY_START "\x07"
#define UI_KEY_SELECT "\x08"
#define UI_KEY_DPAD "\x0b"
#define UI_KEY_RIGHT "\x0c"
#define UI_KEY_ANALOG "\x0e"
int ui_draw_init(const char *font);
void ui_draw_frame(unsigned *pixels,int stride);
void ui_bitmap(const StImage *image,int x,int y,int width,int height,unsigned opacity);
void ui_draw_term(void);
void ui_media_cache(StMediaCache *cache);
int ui_image(const char *tag,const char *id,int x,int y,int width,int height,int round);
int ui_image_state(const char *tag,const char *id);
/* Image crop: 0 fit, 1 circular cover, 2 rectangular cover (profile banner). */
int ui_image_ref(const StMediaRequest *key,int x,int y,int width,int height,int round);
enum UiIcon { UI_ICON_NONE,UI_ICON_MESSAGE,UI_ICON_PEOPLE,UI_ICON_USER,UI_ICON_SETTINGS,UI_ICON_CACHE,UI_ICON_PALETTE,UI_ICON_DEVICES,UI_ICON_TEXT,UI_ICON_TRASH,UI_ICON_LIMIT,UI_ICON_SUN,UI_ICON_MOON,UI_ICON_COLOUR,UI_ICON_EMAIL,UI_ICON_KEY,UI_ICON_INVITE,UI_ICON_LOGIN,UI_ICON_LOGOUT,UI_ICON_ADD_USER,UI_ICON_REMOVE_USER,UI_ICON_BLOCK,UI_ICON_UNBLOCK,UI_ICON_FLAG,UI_ICON_SERVERS,UI_ICON_HASH,UI_ICON_FILE,UI_ICON_IMAGE,UI_ICON_ONLINE,UI_ICON_FOCUS,UI_ICON_BUSY,UI_ICON_INVISIBLE,UI_ICON_VOICE,UI_ICON_MIC,UI_ICON_MIC_OFF,UI_ICON_AUDIO_OFF,UI_ICON_FOLDER };
void ui_icon(enum UiIcon icon,int x,int y,unsigned color);
void ui_icon_size(enum UiIcon icon,int x,int y,int size,unsigned color);
void ui_clip(int left,int top,int right,int bottom);
void ui_clip_reset(void);
void ui_circle(int x,int y,int diameter,unsigned color);
void ui_round_rect(int x,int y,int width,int height,int radius,unsigned color);
void ui_presence(int x,int y,int size,enum StPresence presence,unsigned background);
int ui_text_width(const char *text);
void ui_loader(int x,int y,int width,const char *label);
void ui_rect(int x,int y,int width,int height,unsigned color);
void ui_text(int x,int y,const char *text,unsigned color);
void ui_text_clip(int x,int y,int width,const char *text,unsigned color);
void ui_role_text_clip(int x,int y,int width,const char *text,const char *style,unsigned fallback);
int ui_wrap_area(int x,int y,int width,const char *text,int skip,int lines,unsigned color);
int ui_line_count(const char *text,int width);
void ui_mentions(const StClient *client,const char *server);
void ui_mentions_reset(void);
int ui_mention_next(char id[64]);
int ui_markdown_area(int x,int y,int width,const char *text,int skip,int lines,unsigned color);
void ui_markdown_inline(int x,int y,int width,const char *text,unsigned color);
int ui_markdown_lines(const char *text,int width);
int ui_wrap(int x,int y,const char *text,int skip,int lines,unsigned color);
void ui_title(const char *title);
void ui_footer(const char *text);
#endif
