#ifndef ST_AUDIO_H
#define ST_AUDIO_H
#include "sound.h"
/* Ownership transfers on install. The UI only signals; firmware output blocks
 * in a separate thread, in 1024-frame chunks, never in the network worker. */
void st_audio_install(StSound *sound);
void st_audio_play(void);
void st_audio_effect(enum StEffect effect);
void st_audio_install_effects(StSound effects[ST_FX_COUNT]);
void st_audio_term(void);
#endif
