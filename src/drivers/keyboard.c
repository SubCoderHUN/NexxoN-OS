/* ============================================================================
 * NexxoN OS - PS/2 keyboard driver (v2.0)
 * ----------------------------------------------------------------------------
 * Highlights over v1:
 *   * Tracks the 0xE0 extended-byte prefix and emits dedicated KEY_* codes
 *     for cursor arrows, navigation cluster, and F1..F12 - enabling shell
 *     history recall and the full-screen editor's cursor movement.
 *   * Tracks left vs. right Alt: right Alt (== AltGr) is the layout-toggle
 *     modifier and must NOT also flip arbitrary characters.
 *   * Ships a US-QWERTY (default) and a Hungarian 102-key (QWERTZ) layout.
 *     Toggle live with AltGr+H -> Hungarian, AltGr+A -> US.  The user-space
 *     side picks up the change via keyboard_pop_layout_change().
 *
 *  Hungarian glyphs are emitted as ISO-8859-2 bytes (0xC1, 0xE1, 0xD5, ...).
 *  The font.c glyph table renders those byte values.
 * ============================================================================ */
#include "keyboard.h"
#include "mouse.h"
#include "irq.h"
#include "isr.h"
#include "io.h"
#include "debug.h"
#include "string.h"
#include "window.h"
#include "apps.h"
#include "pit.h"
#include "pic.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* ---------- Modifier state (IRQ-only writer) ----------------------------- */
static volatile bool g_shift_l = false;
static volatile bool g_shift_r = false;
static volatile bool g_caps    = false;
static volatile bool g_ctrl_l  = false;
static volatile bool g_ctrl_r  = false;
static volatile bool g_alt_l   = false;
static volatile bool g_alt_gr  = false;   /* right Alt, scan code 0xE0 0x38 */
static volatile bool g_meta    = false;   /* Win key (0xE0 0x5B/0x5C)        */

/* Sticky flag set by the ISR when Win+Del is pressed; consumed by the
 * shell idle loop which then opens the Task Manager. */
static volatile bool g_taskmgr_request = false;
bool keyboard_taskmgr_requested(void) {
    if (g_taskmgr_request) { g_taskmgr_request = false; return true; }
    return false;
}

/* Win-key TAP detection: a lone press+release of the Windows key toggles
 * the Start menu, but Win+<key> shortcuts (e.g. Win+Del) must NOT.  We set
 * g_meta_used the moment any other key is pressed while Win is held, and on
 * Win release request the toggle only if it was never used in a combo. */
static volatile bool g_meta_used        = false;
static volatile bool g_startmenu_request = false;
bool keyboard_startmenu_requested(void) {
    if (g_startmenu_request) { g_startmenu_request = false; return true; }
    return false;
}

/* Becomes true for exactly one ISR pass when the 0xE0 prefix is seen. */
static volatile bool g_extended = false;

/* ---- Scan-code SET 2 support (BIOS USB-legacy / no-8042-translation) -------
 * NexxoN's decoder is written for SET 1 (XT) scan codes, which is what a PC
 * 8042 delivers when its translation bit is on (the normal case, incl. QEMU).
 * On some bare-metal boxes the firmware USB-legacy SMM handler emulates the
 * keyboard but hands the OS RAW SET 2 codes (8042 translation off).  Set 2
 * differs two ways that wreck a set-1 decoder:
 *   1. Different make codes  -> every key maps to the wrong symbol.
 *   2. Break = 0xF0 PREFIX + code (not the high-bit on the make), so an
 *      unaware decoder sees the make, then re-sees the code after 0xF0 and
 *      registers the key TWICE (the "everything types double" symptom).
 * We auto-detect set 2 the instant a 0xF0 byte appears (it never occurs in a
 * set-1 stream except the practically-unused Int-2/Katakana key), then
 * translate each set-2 make code to its set-1 equivalent and fold the pending
 * release into the set-1 high-bit convention so the rest of the decoder works
 * unchanged.  Pure software: we never poke the 8042 command byte, so this
 * cannot disturb a firmware-emulated keyboard that is already half-working. */
static volatile bool g_set2          = false;  /* latched once 0xF0 is seen   */
static volatile bool g_set2_release  = false;  /* next code is a key release  */

/* Raw make/break observer (set-1 normalised).  Games (DOOM) need real
 * key-down/key-up pairs which the cooked char ring cannot provide. */
static keyboard_raw_hook_t g_raw_hook = NULL;
void keyboard_set_raw_hook(keyboard_raw_hook_t fn) { g_raw_hook = fn; }

/* Standard 8042 "set 2 -> set 1" translation table (index = set-2 make code).
 * 0 = no set-1 equivalent / unused.  Matches the table the 8042 applies in
 * hardware when its translation bit is enabled. */
static const uint8_t k_set2_to_set1[256] = {
    [0x01]=0x43,[0x03]=0x3F,[0x04]=0x3D,[0x05]=0x3B,[0x06]=0x3C,[0x07]=0x58,
    [0x09]=0x44,[0x0A]=0x42,[0x0B]=0x40,[0x0C]=0x3E,[0x0D]=0x0F,[0x0E]=0x29,
    [0x11]=0x38,[0x12]=0x2A,[0x14]=0x1D,[0x15]=0x10,[0x16]=0x02,
    [0x1A]=0x2C,[0x1B]=0x1F,[0x1C]=0x1E,[0x1D]=0x11,[0x1E]=0x03,
    [0x21]=0x2E,[0x22]=0x2D,[0x23]=0x20,[0x24]=0x12,[0x25]=0x05,[0x26]=0x04,
    [0x29]=0x39,[0x2A]=0x2F,[0x2B]=0x21,[0x2C]=0x14,[0x2D]=0x13,[0x2E]=0x06,
    [0x31]=0x31,[0x32]=0x30,[0x33]=0x23,[0x34]=0x22,[0x35]=0x15,[0x36]=0x07,
    [0x3A]=0x32,[0x3B]=0x24,[0x3C]=0x16,[0x3D]=0x08,[0x3E]=0x09,
    [0x41]=0x33,[0x42]=0x25,[0x43]=0x17,[0x44]=0x18,[0x45]=0x0B,[0x46]=0x0A,
    [0x49]=0x34,[0x4A]=0x35,[0x4B]=0x26,[0x4C]=0x27,[0x4D]=0x19,[0x4E]=0x0C,
    [0x52]=0x28,[0x54]=0x1A,[0x55]=0x0D,
    [0x58]=0x3A,[0x59]=0x36,[0x5A]=0x1C,[0x5B]=0x1B,[0x5D]=0x2B,
    [0x61]=0x56,[0x66]=0x0E,
    [0x69]=0x4F,[0x6B]=0x4B,[0x6C]=0x47,
    [0x70]=0x52,[0x71]=0x53,[0x72]=0x50,[0x73]=0x4C,[0x74]=0x4D,[0x75]=0x48,
    [0x76]=0x01,[0x77]=0x45,[0x78]=0x57,[0x79]=0x4E,[0x7A]=0x51,[0x7B]=0x4A,
    [0x7C]=0x37,[0x7D]=0x49,[0x7E]=0x46,[0x83]=0x41,
};

/* Ring buffer.  Single producer (IRQ1), single consumer (read_line / editor). */
static volatile uint8_t  g_buf [KBD_BUF_SIZE];
static volatile uint32_t g_head = 0;
static volatile uint32_t g_tail = 0;

static volatile kbd_layout_t g_layout = KBD_LAYOUT_US;
static volatile const char  *g_pending_notice = NULL;

/* User-tweakable repeat rate index (0=slow, 1=normal, 2=fast).  Pure
 * informational for now - the controller's own typematic register is
 * still default; the Gépház settings code just persists this. */
int g_kbd_repeat_rate = 1;

/* Sticky abort flag.  Set by the IRQ when Ctrl+C is pressed; cleared by
 * keyboard_clear_abort() after the consumer (shell main loop, installsys
 * step, NXScript interpreter, ...) has acted on it.  Volatile so the
 * compiler doesn't cache reads inside a long-running poll loop. */
static volatile bool g_abort_requested = false;

/* ---------- US-QWERTY (set 1) ASCII tables ------------------------------- */
static const uint8_t k_us_normal[128] = {
    0,    27,  '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0,    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,    '\\','z','x','c','v','b','n','m',',','.','/',
    0,    '*',  0,  ' ',
};

static const uint8_t k_us_shift[128] = {
    0,    27,  '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0,    'A','S','D','F','G','H','J','K','L',':','"', '~',
    0,    '|','Z','X','C','V','B','N','M','<','>','?',
    0,    '*',  0,  ' ',
};

/* ---------- Hungarian 102-key QWERTZ (set 1) --------------------------------
 *
 * Three tables, all using C99 designated initializers so zero-count errors
 * (the previous bug that put 0xED at index 0x50 instead of 0x56) are
 * structurally impossible — each entry is addressed by its scan code directly.
 *
 * k_hu_normal  : unshifted characters
 * k_hu_shift   : shifted characters (Shift held)
 * k_hu_altgr   : AltGr layer (right Alt held) — programmer symbols
 *
 * Key positions of interest:
 *   Scan 0x29 = OEM_3 (left of '1')        -> '0' (normal), '§' (shifted)
 *   Scan 0x2B = hash/backslash row          -> ű (normal),  Ű (shifted)
 *   Scan 0x56 = 102nd extra key (ISO)       -> í (normal),  Í (shifted)
 *
 * All accented bytes are ISO-8859-2; font.c has glyphs for every one of them.
 */

/* --- Normal (unshifted) --------------------------------------------------- */
static const uint8_t k_hu_normal[128] = {
    [0x01] = 27,                    /* Esc */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = 0xF6,                  /* ö */
    [0x0C] = 0xFC,                  /* ü */
    [0x0D] = 0xF3,                  /* ó */
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'z', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 0xF5,                  /* ő */
    [0x1B] = 0xFA,                  /* ú */
    [0x1C] = '\n',
    /* 0x1D = L Ctrl (modifier, not a character) */
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = 0xE9,                  /* é */
    [0x28] = 0xE1,                  /* á */
    [0x29] = '0',                   /* OEM_3: the "0 / §" key left of '1' */
    /* 0x2A = L Shift */
    [0x2B] = 0xFB,                  /* ű  (hash/backslash position on 102-key) */
    [0x2C] = 'y', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '-',
    /* 0x36 = R Shift */
    [0x37] = '*',
    /* 0x38 = L Alt */
    [0x39] = ' ',
    /* 0x3A–0x55 = CapsLock, F1-F10, NumLock, ScrollLock, keypad — no char */
    [0x56] = 0xED,                  /* í  (102nd ISO key between L-Shift and Y) */
};

/* --- Shifted --------------------------------------------------------------- */
static const uint8_t k_hu_shift[128] = {
    [0x01] = 27,
    [0x02] = '\'', [0x03] = '"',  [0x04] = '+',  [0x05] = '!',
    [0x06] = '%',  [0x07] = '/',  [0x08] = '=',  [0x09] = '(',
    [0x0A] = ')',
    [0x0B] = 0xD6,                  /* Ö */
    [0x0C] = 0xDC,                  /* Ü */
    [0x0D] = 0xD3,                  /* Ó */
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Z', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P',
    [0x1A] = 0xD5,                  /* Ő */
    [0x1B] = 0xDA,                  /* Ú */
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = 0xC9,                  /* É */
    [0x28] = 0xC1,                  /* Á */
    [0x29] = 0xA7,                  /* § (shifted OEM_3; 0xA7 in Latin-2) */
    [0x2B] = 0xDB,                  /* Ű */
    [0x2C] = 'Y', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '?', [0x34] = ':', [0x35] = '_',
    [0x37] = '*',
    [0x39] = ' ',
    [0x56] = 0xCD,                  /* Í */
};

/* --- AltGr (right Alt) — programmer symbols for HU layout ----------------- */
/* When AltGr is held the ISR looks here first (HU layout only).             */
static const uint8_t k_hu_altgr[128] = {
    [0x10] = '\\',  /* AltGr+Q = backslash     */
    [0x11] = '|',   /* AltGr+W = pipe          */
    [0x21] = '[',   /* AltGr+F = [             */
    [0x22] = ']',   /* AltGr+G = ]             */
    [0x30] = '{',   /* AltGr+B = {             */
    [0x31] = '}',   /* AltGr+N = }             */
    [0x32] = '@',   /* AltGr+M = at-sign       */
    [0x33] = ';',   /* AltGr+, = semicolon     */
    [0x34] = '>',   /* AltGr+. = greater-than  */
    /* The "IS/I/Y" 102nd key (scancode 0x56), located between L-Shift and Y
     * on a Hungarian QWERTZ keyboard, is the canonical home of < and > in
     * the Microsoft Hungarian layout.  Without these two lines the user
     * has no way at all to type angle brackets, since AltGr+. only covers
     * the '>' side and '<' was unreachable on every key.            */
    [0x56] = '<',   /* AltGr+IS = less-than                          */
};

/* --- Shift+AltGr (HU layout): shifted programmer symbols -----------------
 * Looked up when both Shift AND AltGr (right Alt) are held.  This lets the
 * Hungarian layout deliver '>' on Shift+AltGr+IS so it matches the same
 * key as '<' (industry-standard placement). */
static const uint8_t k_hu_altgr_shift[128] = {
    [0x33] = '<',   /* Shift+AltGr+, = less-than (alternative path)  */
    [0x34] = '>',   /* Shift+AltGr+. = greater-than                  */
    [0x56] = '>',   /* Shift+AltGr+IS = greater-than                 */
};

/* ---------- Helpers ------------------------------------------------------ */
static inline void buf_push(uint8_t c) {
    uint32_t next = (g_head + 1) % KBD_BUF_SIZE;
    if (next == g_tail) {
        /* Ring buffer full.  Drop the byte and log it so we can correlate
         * "where did my keystroke go?" reports with a real overflow. */
        debug_printf("[kbd] buffer full, dropped 0x%02x\n", c);
        return;
    }
    g_buf[g_head] = c;
    g_head = next;
}

static inline bool shift_active(void) { return g_shift_l || g_shift_r; }
static inline bool ctrl_active (void) { return g_ctrl_l  || g_ctrl_r;  }

static const uint8_t *current_table(bool with_shift) {
    if (g_layout == KBD_LAYOUT_HU) {
        return with_shift ? k_hu_shift : k_hu_normal;
    }
    return with_shift ? k_us_shift : k_us_normal;
}

static void notify_layout_changed(void) {
    g_pending_notice = (g_layout == KBD_LAYOUT_HU)
        ? "[ layout switched -> Hungarian (HU) ]"
        : "[ layout switched -> US English (EN) ]";
    debug_printf("[kbd] %s\n", g_pending_notice);
}

/* Returns the special key code for an extended scan code (after 0xE0), or
 * 0 if the code does not map to one of our supported keys. */
static uint8_t map_extended(uint8_t make) {
    switch (make) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DEL;
        case 0x1C: return KEY_ENTER;       /* numpad Enter */
        case 0x35: return '/';             /* numpad /     */
        default:   return 0;
    }
}

/* Returns the special key code for an F-key make code, or 0 otherwise. */
static uint8_t map_function_key(uint8_t make) {
    switch (make) {
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:   return 0;
    }
}

/* ---------- i8042 byte processing (called by ps2_dispatch) -------------- */
/* Process ONE scan-code byte that ps2_dispatch() already read from the
 * shared i8042 output buffer.  Split out from the raw IRQ1 entry so the
 * SAME routine handles bytes whether they arrived via IRQ1, or via the
 * IRQ0 (PIT) safety-drain when IRQ1/IRQ12 delivery is unreliable (e.g.
 * under VirtualBox / on some bare-metal 8042s). */
void keyboard_handle_byte(uint8_t sc) {
    /* Any keystroke kicks the screensaver - resets idle counter and
     * deactivates the saver if it was running. */
    screensaver_kick();

    /* 0xE0 = next byte is an extended scan code (same prefix in set 1 & 2). */
    if (sc == 0xE0) { g_extended = true; return; }
    /* 0xE1 (Pause/Break) - swallow the whole 6-byte sequence by ignoring. */
    if (sc == 0xE1) return;

    /* 0xF0 = SET 2 break prefix.  Its appearance proves the controller is
     * handing us raw set 2 (firmware USB-legacy with 8042 translation off):
     * latch set-2 mode and mark the next code as a release.  (0xF0 never
     * starts a set-1 byte except the practically-unused Int-2/Katakana key.) */
    if (sc == 0xF0) { g_set2 = true; g_set2_release = true; return; }

    /* In set-2 mode, translate this make code to its set-1 equivalent and fold
     * the pending release into the set-1 high-bit break convention, so the
     * rest of this set-1-oriented function works unchanged. */
    if (g_set2) {
        uint8_t s1 = k_set2_to_set1[sc];
        bool rel = g_set2_release;
        g_set2_release = false;
        if (s1 == 0) { g_extended = false; return; }   /* unknown -> drop */
        sc = (uint8_t)(s1 | (rel ? 0x80 : 0x00));
    }

    bool is_break = (sc & 0x80) != 0;
    uint8_t make  = sc & 0x7F;
    bool ext      = g_extended;
    g_extended    = false;

    /* ---- Raw make/break tap (games need real key-down/key-up pairs) ----
     * Fired BEFORE modifier tracking so the observer sees ctrl/alt/shift
     * transitions too.  Observe-only: normal decoding continues below. */
    if (g_raw_hook) g_raw_hook(make, ext, !is_break);

    /* ---- Modifier tracking (consumes both make and break) -------------- */
    if (make == 0x2A && !ext) { g_shift_l = !is_break; return; }
    if (make == 0x36 && !ext) { g_shift_r = !is_break; return; }
    if (make == 0x1D) {
        if (ext) g_ctrl_r = !is_break;
        else     g_ctrl_l = !is_break;
        return;
    }
    if (make == 0x38) {
        if (ext) g_alt_gr = !is_break;
        else     g_alt_l  = !is_break;
        return;
    }
    /* Win / Meta key tracking.  Both make AND break are consumed here so
     * the modifier flag clears the instant the user lifts the key — without
     * this, pressing Win once latched g_meta forever, and any later Del
     * keystroke was mis-fired as Win+Del = "open Task Manager". */
    if (ext && (make == 0x5B || make == 0x5C)) {
        if (is_break) {
            /* Lone Win tap (no other key used) toggles the Start menu. */
            if (g_meta && !g_meta_used) g_startmenu_request = true;
            g_meta = false;
        } else {
            g_meta      = true;
            g_meta_used = false;     /* begin tracking a potential tap */
        }
        return;
    }
    if (make == 0x3A) {                              /* Caps Lock toggle */
        if (!is_break && !ext) g_caps = !g_caps;
        return;
    }

    /* Any non-modifier key pressed while Win is held turns the Win press
     * into a combo (Win+X), so releasing Win must NOT toggle the Start
     * menu.  Reached only after the modifier blocks have returned. */
    if (!is_break && g_meta) g_meta_used = true;

    if (is_break) return;                            /* only emit on press */

    /* ---- Ctrl+C emergency abort --------------------------------------- *
     * Scancode 0x2E is the 'C' key on US/HU set-1.  When pressed with
     * either Ctrl down we DO NOT push the literal 0x03 (ETX) byte that
     * a US keyboard would naturally produce - that turned out to be too
     * easy to confuse with regular text input.  Instead we always push
     * the out-of-band KEY_CTRL_C code so the focused window's input
     * loop can wake up and branch on intent (the editor uses this to
     * trigger its "discard or save?" prompt, for example).
     *
     * STRICT FOCUS RULE (Task 2): the sticky g_abort_requested flag,
     * which long-running shell jobs (NXScript loops, installsys, NXFS
     * scans) poll cooperatively, is ONLY set when the SHELL window is
     * focused.  This guarantees that "Ctrl+C kills the running process
     * inside the terminal" without polluting the abort state when a
     * Ctrl+C was actually meant for the editor or another future GUI
     * input consumer.
     *
     * Layout-agnostic: works on US, HU, anything that uses set-1 0x2E
     * for the C key. */
    if (!ext && make == 0x2E && ctrl_active()) {
        buf_push(KEY_CTRL_C);
        if (wm_is_shell_focused()) {
            g_abort_requested = true;
        }
        return;
    }

    /* TASK 25: Ctrl+X / Ctrl+V emit out-of-band codes so read_line /
     * editor consumers can route to the clipboard without colliding
     * with the literal 'x' / 'v' characters they'd otherwise insert. */
    if (!ext && make == 0x2D && ctrl_active()) {   /* X */
        buf_push(KEY_CTRL_X);
        return;
    }
    if (!ext && make == 0x2F && ctrl_active()) {   /* V */
        buf_push(KEY_CTRL_V);
        return;
    }

    /* ---- AltGr handling ------------------------------------------------ */
    if (g_alt_gr && !ext) {
        /* H / A: layout toggle (always, regardless of current layout). */
        if (make == 0x23 /* H */) {
            if (g_layout != KBD_LAYOUT_HU) {
                g_layout = KBD_LAYOUT_HU;
                notify_layout_changed();
            }
            return;
        }
        if (make == 0x1E /* A */) {
            if (g_layout != KBD_LAYOUT_US) {
                g_layout = KBD_LAYOUT_US;
                notify_layout_changed();
            }
            return;
        }
        /* Any other key with AltGr in HU layout: look up the AltGr table.
         * Shift-AltGr is its own table so combinations like Shift+AltGr+IS
         * = '>' work cleanly without polluting the plain AltGr layer.
         * We return unconditionally so the key is never also emitted as
         * its normal/shifted character (which was the previous fall-
         * through bug). */
        if (g_layout == KBD_LAYOUT_HU && make < 128) {
            uint8_t ag = shift_active()
                       ? k_hu_altgr_shift[make]
                       : k_hu_altgr[make];
            /* Fall back to the un-shifted AltGr table when no shifted
             * mapping exists - users expect Shift+AltGr+Q to still type
             * a backslash even though we don't bind it explicitly. */
            if (!ag && shift_active()) ag = k_hu_altgr[make];
            if (ag) buf_push(ag);
        }
        return;
    }

    /* ---- Extended (cursor + navigation + numpad Enter) ----------------- *
     * Note: Win-key make/break handled in the modifier block above, so we
     * never reach here with make == 0x5B/0x5C. */
    if (ext) {
        /* Win + Del = global Task Manager shortcut.  STRICT: Win must
         * actually be held down at this moment — Del alone must NOT
         * trigger this (regression-tested in QA hitbox suite). */
        if (g_meta && make == 0x53) {
            g_taskmgr_request = true;
            return;
        }
        uint8_t k = map_extended(make);
        if (k) buf_push(k);
        return;
    }

    /* ---- F-keys -------------------------------------------------------- */
    {
        uint8_t fk = map_function_key(make);
        if (fk) { buf_push(fk); return; }
    }

    /* ---- ASCII translation -------------------------------------------- */
    if (make >= 128) return;
    const uint8_t *tbl = current_table(shift_active());
    uint8_t c = tbl[make];
    if (!c) return;

    /* Caps Lock affects alphabetic keys.  For the Hungarian layout we keep
     * caps semantics consistent with their accented counterparts as well. */
    if (g_caps) {
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 'A' && c <= 'Z') {
            if (shift_active()) c += 32;
        }
        /* ISO-8859-2 accented pairs: low and high differ by 0x20 only for
         * 0xE1/0xC1, 0xE9/0xC9, 0xED/0xCD, 0xF3/0xD3, 0xF6/0xD6 - matches
         * the +/-32 ASCII rule. 0xF5/0xD5, 0xFA/0xDA, 0xFB/0xDB, 0xFC/0xDC
         * also follow the same pattern, so the rule below covers them. */
        else if (c >= 0xE0 && c <= 0xFE) {
            c -= 0x20;
        } else if (c >= 0xC0 && c <= 0xDE) {
            if (shift_active()) c += 0x20;
        }
    }

    /* Ctrl + letter -> control character (Ctrl+C = 0x03, Ctrl+H = 0x08). */
    if (ctrl_active()) {
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 1);
    }

    buf_push(c);
}

/* ---------- Unified i8042 output-buffer drain + router ------------------- *
 * The PS/2 keyboard and mouse share ONE controller and ONE output buffer
 * (port 0x60).  A byte's origin is told by the AUX bit (0x20) in the
 * status register (0x64): set => the byte came from the mouse, clear =>
 * keyboard.  We drain EVERY pending byte and route each one accordingly.
 *
 * This is called from THREE places:
 *   - keyboard_isr (IRQ1),
 *   - mouse_isr    (IRQ12),
 *   - pit_isr      (IRQ0, 100 Hz) as a safety drain.
 *
 * The IRQ0 path is the critical robustness fix: on some hosts (VirtualBox,
 * and stricter bare-metal 8042s) the mouse interrupt (IRQ12) is NOT
 * reliably delivered even though the device streams packets into the
 * output buffer.  With only IRQ1/IRQ12 draining, those undrained mouse
 * bytes fill the shared buffer, the controller then refuses to hand over
 * keyboard scan-codes, and the keyboard goes dead the moment the mouse
 * moves -- exactly the reported "keyboard works until I touch the mouse,
 * and the mouse never works at all" failure.  Draining from the always-
 * present 100 Hz timer guarantees the buffer can never wedge and lets the
 * cursor track even when IRQ12 never fires. */
/* While the mouse-init handshake is talking to the controller it polls the
 * data port itself for ACK/BAT/ID bytes.  Suppress the shared dispatch (and
 * especially the IRQ0 safety drain) during that window so we don't swallow
 * those handshake replies out from under it. */
static volatile bool g_ps2_inhibit = false;
void ps2_dispatch_inhibit(bool on) { g_ps2_inhibit = on; }

void ps2_dispatch(void) {
    if (g_ps2_inhibit) return;
    /* Bounded so a controller that wedges OBF-asserted can't spin us. */
    for (int guard = 0; guard < 32; guard++) {
        uint8_t st = inb(KBD_STATUS);
        if (!(st & 0x01)) return;          /* output buffer empty */
        uint8_t data = inb(KBD_DATA);
        if (st & 0x20) mouse_handle_byte(data);     /* AUX bit -> mouse  */
        else           keyboard_handle_byte(data);  /*         -> keyboard */
    }
}

/* ---------- IRQ1 ISR ----------------------------------------------------- */
static void keyboard_isr(registers_t *r) {
    (void)r;
    ps2_dispatch();
}

/* ---------- Public API --------------------------------------------------- */
void keyboard_init(void) {
    debug_step("kbd: initialising PS/2 keyboard (v3.0, bare-metal hardened)");
    g_head = g_tail = 0;
    g_shift_l = g_shift_r = g_caps = false;
    g_ctrl_l  = g_ctrl_r  = g_alt_l = g_alt_gr = false;
    g_extended = false;
    g_set2 = false;
    g_set2_release = false;
    g_layout   = KBD_LAYOUT_US;
    g_pending_notice = NULL;

    /* Bare-metal hardening: the previous "while output-buffer-full, read"
     * drain had no timeout.  On real hardware (Lenovo PCH + USB-legacy
     * emulating PS/2) the controller can sit in a state where the OBF
     * bit stays asserted forever, which froze the boot at the kbd init
     * step.  Bound the drain by both a byte-count cap AND a time cap. */
    uint32_t start_ms = pit_ms();
    int drained = 0;
    for (drained = 0; drained < 256; drained++) {
        if (!(inb(KBD_STATUS) & 0x01)) break;
        (void)inb(KBD_DATA);
        /* 50 ms total budget for the drain.  pit_ms() at 100 Hz has
         * 10 ms granularity which is plenty for a sanity check. */
        if (pit_ms() - start_ms > 50) {
            debug_printf("[kbd] drain timeout after %d bytes (status=0x%02x) "
                         "- continuing\n", drained, inb(KBD_STATUS));
            break;
        }
    }

    irq_install_handler(1, keyboard_isr);
    /* Explicit unmask: pic_remap() now hands us a fully-masked PIC, so
     * we must open IRQ1 ourselves.  Without this, no keyboard interrupt
     * ever reaches the CPU on bare metal - which is exactly the
     * "keyboard is dead at the login screen" failure mode. */
    pic_unmask(1);
    debug_ok("kbd: ready (IRQ1 unmasked, drained, AltGr+H/A layout switch)");
}

bool keyboard_has_data(void) { return g_head != g_tail; }

int keyboard_getc(void) {
    if (g_head == g_tail) return 0;
    uint8_t c = g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF_SIZE;
    return (int)c;
}

int keyboard_wait_getc(void) {
    while (g_head == g_tail) {
        __asm__ volatile ("sti; hlt");
    }
    uint8_t c = g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF_SIZE;
    return (int)c;
}

kbd_layout_t keyboard_get_layout(void) { return g_layout; }

void keyboard_set_layout(kbd_layout_t l) {
    if (l == g_layout) return;
    g_layout = l;
    notify_layout_changed();
}

const char *keyboard_pop_layout_change(void) {
    /* Snap-read the pending notice and clear it.  Cheap CLI/STI bracket
     * is enough to keep the IRQ writer and userspace reader honest. */
    cli();
    const char *p = (const char *)g_pending_notice;
    g_pending_notice = NULL;
    sti();
    return p;
}

bool keyboard_abort_requested(void) { return g_abort_requested; }
void keyboard_clear_abort     (void) { g_abort_requested = false; }

bool keyboard_shift_held(void) { return g_shift_l || g_shift_r; }
bool keyboard_ctrl_held (void) { return g_ctrl_l  || g_ctrl_r;  }

/* Empty the ring buffer.  Cheap CLI bracket so we don't race the IRQ
 * mid-drain (an interrupt fired between the empty-check and the
 * head/tail reset would leak its byte).  Use sparingly - drains are a
 * tool for "I really do want a fresh prompt", not an everyday call. */
void keyboard_drain(void) {
    cli();
    g_head = g_tail = 0;
    sti();
}
