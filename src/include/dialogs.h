/* ============================================================================
 * NexxoN OS - Shared modal dialog primitives (v1.0)
 * ----------------------------------------------------------------------------
 * Three modal-dialog helpers reusable across the GUI subsystems:
 *
 *   dialog_input    - text-edit prompt with OK/Cancel via Enter/Esc.
 *   dialog_yes_no   - confirmation prompt (Y/N) for destructive actions.
 *   dialog_info     - read-only information popup (Properties, errors).
 *
 * Each helper:
 *   1. Creates a centred non-resizable modal window sized to fit the text
 *      (BUG 4: dynamic width / multi-line wrap so labels never overflow).
 *   2. Pushes itself onto the WM modal stack so background clicks are
 *      ignored until the dialog closes.
 *   3. Drains the keyboard ring buffer before entering its input loop so
 *      stale keystrokes cannot auto-answer (BUG 2 hardening).
 *   4. Ticks wm_tick() while waiting on a keystroke so the compositor
 *      keeps painting and the mouse cursor stays responsive.
 *   5. Detects external window destruction (title-bar X) and bails cleanly.
 *
 * dialog_yes_no accepts an expanded set of "yes" / "no" tokens (Y / y /
 * Enter / Space) and (N / n / Esc / Backspace) so a single stuck modifier
 * or odd keyboard layout cannot soft-lock the user.
 * ============================================================================ */
#ifndef NEXXON_DIALOGS_H
#define NEXXON_DIALOGS_H

#include "types.h"

/* Modal text-input dialog.  Returns true if the user confirmed with Enter
 * (and `out` is populated with up to out_sz-1 characters), or false on
 * Esc / Ctrl+C / title-bar close.  initial may be NULL (treated as ""). */
bool dialog_input(const char *title, const char *prompt,
                  const char *initial, char *out, size_t out_sz);

/* Modal yes/no confirmation.  line2 may be NULL (suppressed if so). */
bool dialog_yes_no(const char *title, const char *line1, const char *line2);

/* Modal information dialog.  Closes on any key or the title-bar X. */
void dialog_info (const char *title, const char *const *lines, int n_lines);

/* Modal icon list chooser: a clickable row per option (each with a type-aware
 * icon from labels[i] / icon_cmds[i]).  Returns the clicked index, or -1 on
 * Cancel / Esc / close.  icon_cmds may be NULL (icons inferred from labels). */
int  dialog_choice(const char *title, const char *prompt,
                   const char *const *labels, const char *const *icon_cmds, int n);

#endif /* NEXXON_DIALOGS_H */
