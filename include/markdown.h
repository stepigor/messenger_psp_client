#ifndef ST_MARKDOWN_H
#define ST_MARKDOWN_H
#include "stoat.h"
enum StMarkdownStyle { ST_MD_BOLD=1,ST_MD_ITALIC=2,ST_MD_STRIKE=4,ST_MD_CODE=8,ST_MD_QUOTE=16,ST_MD_HEADING=32,ST_MD_LINK=64,ST_MD_LITERAL=128 };
typedef struct { char text[ST_TEXT_MAX];unsigned char style[ST_TEXT_MAX];size_t length; } StMarkdown;
int st_mention_match(const char **text,char id[64]);
int st_channel_mention_match(const char **text,char id[64]);
void st_markdown_parse(const char *text,StMarkdown *out);
#endif
