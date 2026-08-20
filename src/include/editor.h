/* ============================================================================
 * NexxoN OS - Full-screen text editor  (NexxoN Edit)
 * ----------------------------------------------------------------------------
 * A classic edit/nano-style modal editor that takes over the whole screen:
 *
 *   Row 0          dark-cyan title bar ("NexxoN Edit -- <filename>")
 *   Row 1..N-2     editor body on BLUE background, line numbers in YELLOW
 *   Row N-1        gray status bar with key shortcuts
 *
 * Cursor moves with arrow keys; PgUp/PgDn scroll a page; Home/End jump to
 * line start/end; Backspace / Del / Enter behave as expected; F2 saves and
 * ESC exits (asks for confirmation if there are unsaved changes).
 *
 * The editor speaks NXFS directly through nxfs_read_file / nxfs_write_file
 * so anything it edits persists across reboots.
 * ============================================================================ */
#ifndef NEXXON_EDITOR_H
#define NEXXON_EDITOR_H

#include "types.h"

/* Open the file referenced by `inode`.  `display_name` is what the title
 * bar shows.  The editor consumes keystrokes synchronously and returns
 * only when the user hits ESC (or saves + exits with F2 then ESC). */
void editor_run(uint32_t inode, const char *display_name);

/* Same editor over a MOUNTED-volume path ("/usb0/notes.txt"): loads via
 * vfs_read and F2 saves back via vfs_write_file - pendrive text files are
 * edited in place. */
void editor_run_vfs(const char *vfs_path, const char *display_name);

#endif /* NEXXON_EDITOR_H */
