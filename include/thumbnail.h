#ifndef ST_THUMBNAIL_H
#define ST_THUMBNAIL_H
#include "photos.h"
#include "media.h"
/* Bounded first-frame preview. Never changes the selected original file. */
int st_photo_thumbnail(const char *path,const char *expected,StImage *out,StError *err);
#endif
