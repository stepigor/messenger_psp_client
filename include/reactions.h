#ifndef ST_REACTIONS_H
#define ST_REACTIONS_H
#include "stoat.h"
#define ST_RECENT_REACTIONS 24
#define ST_REACTION_SETTINGS "recent-reactions.json"
/* Picker positions are independent of the stable Unicode pack indices. */
void st_reactions_bind(const StClient *client);
int st_reactions_count(void);
int st_reactions_move(int position,int columns,int dx,int dy,int page);
const char *st_reactions_key(int emoji);
const char *st_reactions_group(int position);
int st_reactions_group_id(int position);
void st_reactions_used_key(const char *key);
void st_reactions_reset(void);
int st_reactions_at(int position);
int st_reactions_position(int emoji);
void st_reactions_used(int emoji);
int st_reactions_load(const char *path,StError *err);
int st_reactions_save(const char *path,StError *err);
#endif
