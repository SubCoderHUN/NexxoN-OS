/* ============================================================================
 * NexxoN OS - Interactive shell
 * ----------------------------------------------------------------------------
 * Reads keyboard input, displays an idle spinner while waiting, and dispatches
 * commands defined in shell.c.
 * ============================================================================ */
#ifndef NEXXON_SHELL_H
#define NEXXON_SHELL_H

void shell_run(void);                   /* never returns */

/* Programmatically execute a single shell command line.  Used by the
 * desktop environment so double-clicking an icon, or activating a
 * future hotkey-bound action, dispatches through the same code path as
 * a user-typed command.  The input is copied internally so the caller's
 * buffer can be on the stack or in a transient register. */
void shell_exec(const char *cmdline);

#endif /* NEXXON_SHELL_H */
