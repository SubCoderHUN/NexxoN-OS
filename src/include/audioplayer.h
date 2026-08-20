#ifndef NEXXON_AUDIOPLAYER_H
#define NEXXON_AUDIOPLAYER_H

#include "types.h"

bool audioplayer_open      (void);
bool audioplayer_active    (void);
void audioplayer_tick      (void);
void audioplayer_open_file (const char *path);

#endif /* NEXXON_AUDIOPLAYER_H */
