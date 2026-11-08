#ifndef ST_EMOJI_H
#define ST_EMOJI_H
#include "stoat.h"
/* Longest supported Unicode sequence. Leave text unchanged on a miss. */
int st_emoji_match(const char **text,char id[64]);
int st_custom_emoji_id(const char *id);
int st_custom_emoji_match(const char **text,char id[64]);
int st_emoji_known(const char *id);
size_t st_emoji_count(void);
const char *st_emoji_text(size_t index);
int st_emoji_index(const char *text);
void st_emoji_identity(const char *id,char *out,size_t cap);
int st_emoji_read(const char *path,const char *id,unsigned char **data,size_t *length,StError *err);
#endif
