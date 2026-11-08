#ifndef ST_SOUND_H
#define ST_SOUND_H
#include "stoat.h"
#define ST_SOUND_RATE 44100
#define ST_SOUND_FRAMES (ST_SOUND_RATE*2)
enum StEffect {ST_FX_MUTE,ST_FX_UNMUTE,ST_FX_DEAFEN,ST_FX_UNDEAFEN,ST_FX_JOIN,ST_FX_LEAVE,ST_FX_COUNT};
typedef struct {int16_t *pcm;size_t frames;} StSound;
void st_sound_free(StSound *sound);
int st_effect_decode(const unsigned char *data,size_t length,StSound *out,StError *error);
int st_sound_decode(const unsigned char *data,size_t length,StSound *out,StError *err);
int st_sound_url(const char *app,char *out,size_t cap);
int st_sound_fetch_effect(const StClient *client,enum StEffect effect,StSound *out,StError *error);
int st_sound_fetch(const StClient *client,int offline,StSound *out,StError *err);
#endif
