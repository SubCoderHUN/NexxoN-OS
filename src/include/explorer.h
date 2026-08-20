/* ============================================================================
 * NexxoN OS - NexxoN Explorer (graphical NXFS file manager)  (v1.0)
 * ----------------------------------------------------------------------------
 * A WM-hosted window that walks the NXFS hierarchy and renders the active
 * directory as a clickable list of folders + files.  Designed to share state
 * with the shell - the explorer reads and writes nxfs_set_cwd() so when the
 * user navigates a folder in the GUI the next shell prompt reflects that
 * path automatically.
 *
 * Mouse interactions:
 *   Single-click on entry  - selects (highlights) the row.
 *   Double-click on folder - navigates into it (updates nxfs_cwd).
 *   Double-click on file   - opens NexxoN Edit (or runs as NXScript if the
 *                            filename ends in ".nx").
 *   Double-click on "[..]" - navigates to parent.
 *   Right-click on entry   - spawns the explorer context menu (Task 4 path).
 *
 * Singleton: there is at most one explorer window in the system.  Opening
 * an already-open instance just brings it back to focus and refreshes the
 * listing - matching every desktop OS's expectation.
 * ============================================================================ */
#ifndef NEXXON_EXPLORER_H
#define NEXXON_EXPLORER_H

#include "types.h"

/* Idempotent.  If no window exists, creates one and lays out the current
 * directory.  If a window already exists, refocuses + refreshes it.
 * Returns true on success (the window is alive after the call), false if
 * the WM could not allocate a slot or framebuffer space. */
bool explorer_open    (void);
bool explorer_active  (void);

/* Re-read the active NXFS cwd and rebuild the entry table.  Used by
 * Task 5 after Delete / Rename to refresh the listing - safe to call
 * even when no explorer window is open (no-op in that case). */
void explorer_refresh (void);

/* Hot-plug hook (called by the PnP daemon on every VFS mount/unmount):
 * refreshes the Drives sidebar, and if the directory being browsed just
 * vanished with its volume, falls back to Home instead of showing a dead
 * listing.  No-op when no explorer window is open. */
void explorer_on_mounts_changed(void);

#endif /* NEXXON_EXPLORER_H */
