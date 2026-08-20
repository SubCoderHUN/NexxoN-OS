/* ============================================================================
 * NexxoN OS - PS/2 keyboard driver  (v2.0)
 * ----------------------------------------------------------------------------
 * Translates AT scan-code set 1 (the format every BIOS still emits with USB
 * Legacy Emulation on) into one of:
 *
 *   * Printable 7-bit ASCII / ISO-8859-2 byte 0x20..0xFF for normal keys.
 *   * One of the KEY_* constants below for special keys (cursor arrows,
 *     function keys, navigation block).
 *
 * The "make code = released" / 0xE0 extended prefix logic lives entirely in
 * the IRQ1 ISR; consumers just call keyboard_(wait_)getc() and get a clean
 * stream of keystrokes.  A small ring buffer decouples the IRQ from the
 * shell / editor / read_line() pacing.
 * ============================================================================ */
#ifndef NEXXON_KEYBOARD_H
#define NEXXON_KEYBOARD_H

#include "types.h"

#define KBD_BUF_SIZE 256

/* ASCII control codes already-meaningful */
#define KEY_NONE        0
#define KEY_BACKSPACE   '\b'        /* 0x08 */
#define KEY_TAB         '\t'        /* 0x09 */
#define KEY_ENTER       '\n'        /* 0x0A */
#define KEY_ESCAPE      27          /* 0x1B */

/* Extended / non-ASCII keys are returned in the 0x80..0x9F private range.
 * Latin-1 / ISO-8859-2 accented characters live at 0xA0..0xFF and never
 * collide with these. */
#define KEY_UP          0x80
#define KEY_DOWN        0x81
#define KEY_LEFT        0x82
#define KEY_RIGHT       0x83
#define KEY_HOME        0x84
#define KEY_END         0x85
#define KEY_PGUP        0x86
#define KEY_PGDN        0x87
#define KEY_DEL         0x88
#define KEY_INSERT      0x89

#define KEY_F1          0x90
#define KEY_F2          0x91
#define KEY_F3          0x92
#define KEY_F4          0x93
#define KEY_F5          0x94
#define KEY_F6          0x95
#define KEY_F7          0x96
#define KEY_F8          0x97
#define KEY_F9          0x98
#define KEY_F10         0x99
#define KEY_F11         0x9A
#define KEY_F12         0x9B

/* Out-of-band keystroke pushed by the IRQ when the user presses Ctrl+C
 * (or Ctrl+Break).  Distinct from the 0x03 ETX byte a US keyboard would
 * normally produce for ^C so consumers can branch on intent ("the user
 * wants to abort whatever is running") regardless of layout. */
#define KEY_CTRL_C      0x9C
/* TASK 25: Ctrl+X (cut) and Ctrl+V (paste).  read_line and the editor
 * route these into the clipboard daemon API. */
#define KEY_CTRL_X      0x9D
#define KEY_CTRL_V      0x9E
/* TASK 18: NexxoN Edit Ctrl+F find. */
#define KEY_CTRL_F      0x9F

/* Keyboard layout selector. */
typedef enum {
    KBD_LAYOUT_US = 0,
    KBD_LAYOUT_HU = 1,
} kbd_layout_t;

void          keyboard_init        (void);

/* Shared-i8042 plumbing.  ps2_dispatch() drains the controller output
 * buffer (port 0x60) and routes each byte to the keyboard or mouse handler
 * by the status-register AUX bit; it is called from IRQ1, IRQ12 and the
 * IRQ0/PIT safety drain.  keyboard_handle_byte() processes one scan-code
 * byte that ps2_dispatch() already read. */
void          ps2_dispatch         (void);
void          keyboard_handle_byte (uint8_t sc);
/* Suppress ps2_dispatch() while a driver owns the controller for a polled
 * command/response handshake (mouse_init).  Re-enable when done. */
void          ps2_dispatch_inhibit (bool on);

int           keyboard_getc        (void);          /* non-blocking */
int           keyboard_wait_getc   (void);          /* blocking      */
bool          keyboard_has_data    (void);

/* Layout management - the keyboard ISR auto-toggles on AltGr+H / AltGr+A
 * but the shell / editor can also flip it programmatically. */
kbd_layout_t  keyboard_get_layout  (void);
void          keyboard_set_layout  (kbd_layout_t l);

/* Returns a transient pointer to a human-readable label if the layout
 * changed since the last call; otherwise NULL.  Used by the shell to print
 * a notification line between command prompts. */
const char   *keyboard_pop_layout_change(void);

/* ---- Ctrl+C abort plumbing ------------------------------------------- *
 * The IRQ sets a sticky g_abort_requested flag (and also pushes KEY_CTRL_C
 * into the buffer so any wait-for-key loop wakes up immediately) when the
 * user presses Ctrl+C with the shell window active.  Long-running tasks
 * (installsys's per-sector loop, NXScript's interpreter, file I/O) poll
 * keyboard_abort_requested() at safe points and unwind cleanly when it
 * fires.  Synchronous readers (wait_yes_no, read_line, editor_wait_key)
 * branch on KEY_CTRL_C from the buffer instead.  Both paths converge on
 * keyboard_clear_abort() once the abort has been honoured. */
bool          keyboard_abort_requested(void);
void          keyboard_clear_abort     (void);

/* Drain every byte currently in the ring buffer.  Used by shell prompts
 * to discard stale keys (e.g. a queued Enter from the previous command)
 * before they accidentally answer an interactive prompt. */
void          keyboard_drain          (void);

/* Win+Del global shortcut request (Task 33).  Returns true once when
 * the user has pressed Win+Del, then resets so the shell idle loop can
 * open Task Manager exactly once per press. */
bool          keyboard_taskmgr_requested(void);

/* True for exactly one poll after the Windows key was tapped on its own
 * (not as part of a Win+<key> combo); the shell idle loop consumes it to
 * toggle the Start menu. */
bool          keyboard_startmenu_requested(void);

/* Modifier key state — readable from any driver without going through the
 * key-character stream.  Used by apps that need Shift/Ctrl+click. */
bool          keyboard_shift_held(void);
bool          keyboard_ctrl_held (void);

/* Raw make/break observer for games: fired for EVERY key transition with
 * the set-1-normalised make code, the extended (0xE0) flag and the
 * direction.  Observe-only — cooked decoding continues unchanged.  Pass
 * NULL to unhook. */
typedef void (*keyboard_raw_hook_t)(uint8_t make, bool ext, bool down);
void          keyboard_set_raw_hook(keyboard_raw_hook_t fn);

#endif /* NEXXON_KEYBOARD_H */
