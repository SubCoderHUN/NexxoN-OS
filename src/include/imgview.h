/* ============================================================================
 * NexxoN OS - Image Viewer  (imgview)
 * ============================================================================ */
#ifndef NEXXON_IMGVIEW_H
#define NEXXON_IMGVIEW_H

#include "types.h"

bool imgview_open      (void);
void imgview_tick      (void);
void imgview_open_file (const char *path);

#endif /* NEXXON_IMGVIEW_H */
