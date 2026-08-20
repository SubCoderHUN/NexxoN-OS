/* ============================================================================
 * NexxoN OS - DOOM application (PureDOOM core in a WM window)
 * ============================================================================ */
#ifndef NEXXON_DOOMAPP_H
#define NEXXON_DOOMAPP_H

#include "types.h"

/* Open (or refocus) the DOOM window.  The first open runs the full DOOM
 * engine init from the embedded shareware IWAD. */
bool doomapp_open(void);

/* Pump one engine frame + present (self-paced to 35 fps internally).
 * Called from the shell idle loop like the other animated apps. */
void doomapp_tick(void);

#endif /* NEXXON_DOOMAPP_H */
