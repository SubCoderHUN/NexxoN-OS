/* ============================================================================
 * NexxoN OS - Kernel panic / global debugger
 * ----------------------------------------------------------------------------
 * Two failure tiers:
 *
 *  (1) recoverable_panic / recoverable_panic_from_exception
 *      Paints a small red modal window on top of every other window with
 *      the error message + OK button.  Once the user dismisses it (mouse
 *      click on OK, or Enter / Esc / Space), it tears down any non-
 *      protected windows, hands focus back to the shell, and longjmps to
 *      the recovery point armed by PANIC_ARM_RECOVERY() at the top of
 *      shell_run.  The shell prints a one-line "recovered" notice and
 *      keeps running.
 *
 *  (2) panic / panic_from_exception
 *      Old-style full-screen red BSOD with a register dump.  Freezes the
 *      machine in a cli/hlt loop.  Used when:
 *         - recovery is not armed (we're too early in boot, or the shell
 *           has handed off the stack without re-arming);
 *         - the exception is structurally unrecoverable (double fault,
 *           machine check) and continuing would corrupt more state.
 *
 *  Recovery is global (single jmp_buf) on purpose - this is a single-
 *  threaded kernel and re-entrant panics would be a bug in the panic
 *  path itself.  recoverable_panic() therefore disarms recovery before
 *  rendering, so a second exception fired by the modal-render path falls
 *  through to the hard panic.
 * ============================================================================ */
#include "panic.h"
#include "isr.h"
#include "vga.h"
#include "terminal.h"
#include "debug.h"
#include "io.h"
#include "string.h"
#include "setjmp.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"
#include "i18n.h"
#include "pit.h"
#include "usermode.h"
#include "nxfs.h"
#include "rtc.h"
#include <stdarg.h>

/* From boot/boot.asm - reads cr2 (page fault linear address).  Returns the
 * full register width (64-bit in long mode), so callers print it with %016lx. */
extern uintptr_t read_cr2(void);

/* ---- Recovery state ----------------------------------------------------- */
jmp_buf       g_panic_recovery;
volatile bool g_panic_recovery_armed = false;

void panic_disarm_recovery(void) { g_panic_recovery_armed = false; }

/* Last-resort halt loop.  We deliberately do NOT execute `cli; hlt`
 * here — masking interrupts forever is what made the old code
 * literally freeze the box with no way out short of yanking power.
 * Instead, enable interrupts (so the keyboard ISR keeps running),
 * hlt to save power, and on every wake check the keyboard scancode
 * queue for Ctrl+R.  Ctrl+R pulses the 8042 reset line and the
 * machine reboots cleanly without any external action.  All other
 * keys are ignored — the user can still see the panic banner. */
static NORETURN void freeze(void) {
    /* Direct hardware reset via the keyboard controller.  Some BIOSes
     * mask the 0x64-reset; loop a few times so we win the race. */
    term_set_color(VGA_YELLOW, 0xFFAA0000);
    term_printf("\n*** Press CTRL+R to reboot ***\n");
    term_set_color(VGA_WHITE, 0xFFAA0000);
    /* Drain any keys buffered before we got here. */
    while (keyboard_has_data()) (void)keyboard_getc();
    for (;;) {
        __asm__ volatile ("sti; hlt");
        while (keyboard_has_data()) {
            int k = keyboard_getc();
            if (k == 0x12 /* Ctrl+R */) {
                for (int i = 0; i < 100; i++) {
                    outb(0x64, 0xFE);
                    for (volatile int j = 0; j < 100000; j++) {}
                }
                /* If the 8042 reset is wired to a non-existent pin
                 * (some virt platforms), fall through and triple-
                 * fault by loading an invalid IDT. */
                struct PACKED { uint16_t limit; uint32_t base; } zero_idt = {0, 0};
                __asm__ volatile ("lidt %0; int $0x03" :: "m"(zero_idt));
            }
        }
    }
    __builtin_unreachable();
}

/* Full-screen LFB draw target the RSOD paints onto.  At fault time the
 * terminal is usually retargeted at the shell window's OFF-screen content
 * buffer, so vga_clear() would paint the screen red while term_printf() wrote
 * the dump into a buffer nothing composites - a blank red screen.  Point the
 * terminal straight at the linear framebuffer first so the dump is always
 * visible, whatever owned the terminal when the CPU faulted. */
static draw_target_t g_panic_screen;

static void render_header(const char *title) {
    /* Red Screen of Death (RSOD).  Saturated dark red so it's unmistakable -
     * the shade matches historical BSOD imagery for instant recognition. */
    void *fb = vga_framebuffer();
    if (fb) {
        g_panic_screen.fb     = (uint8_t *)fb;
        g_panic_screen.width  = vga_width();
        g_panic_screen.height = vga_height();
        g_panic_screen.pitch  = vga_width() * 4;
        term_set_target(&g_panic_screen);
    }
    vga_clear(0xFFAA0000);
    term_clear();
    term_set_color(VGA_WHITE, 0xFFAA0000);
    term_printf("================================================================\n");
    term_printf("           %s                                                   \n",
                L(STR_PANIC_TITLE));
    term_printf("================================================================\n");
    term_printf("%s\n\n", title);
}

/* Walk the stack from the trap frame downward and print the raw machine
 * words.  Helps developers identify which call chain led to the fault when
 * symbols are unavailable (feed an address to `addr2line -e build/nexxon.elf`).
 * We bound the walk tightly to avoid reading garbage past the saved frame.
 * Word width follows the architecture (8 bytes in long mode). */
static void dump_stack_trace(uintptr_t *sp, int n_words) {
    if (!sp) return;
    term_set_color(VGA_LTGRAY, 0xFFAA0000);
    term_printf(" Raw stack trace (top down, %d words):\n", n_words);
#if defined(__x86_64__)
    for (int row = 0; row < (n_words + 1) / 2; row++) {
        term_printf("   +%03x:", row * 16);
        for (int col = 0; col < 2 && row * 2 + col < n_words; col++)
            term_printf("  0x%016lx", (unsigned long)sp[row * 2 + col]);
        term_printf("\n");
    }
#else
    for (int row = 0; row < (n_words + 3) / 4; row++) {
        term_printf("   +%02x:", row * 16);
        for (int col = 0; col < 4 && row * 4 + col < n_words; col++)
            term_printf("  %08x", (uint32_t)sp[row * 4 + col]);
        term_printf("\n");
    }
#endif
    term_set_color(VGA_WHITE, 0xFFAA0000);
}

/* ====================================================================== */
/*           Recoverable-panic GUI modal                                  */
/* ====================================================================== */
/* Render a small centred red window with the error message and an OK
 * button.  Runs its own input loop driven by wm_tick() so the rest of
 * the compositor (mouse cursor, idle ticks) keeps working.
 *
 * Returns once the user has dismissed the modal.  The caller is expected
 * to longjmp() out afterwards - this function deliberately does NOT
 * longjmp itself so the unit-testable rendering stays separated from the
 * non-local control flow. */

#define MODAL_W           480
#define MODAL_H           160
#define MODAL_BG          0xFFA01010
#define MODAL_BORDER_HI   0xFFFFE0E0
#define MODAL_BORDER_LO   0xFF500000
#define MODAL_FG          0xFFFFFFFF
#define MODAL_BTN_BG      0xFFC0C0C0
#define MODAL_BTN_BG_HOT  0xFFFFFFA0
#define MODAL_BTN_FG      0xFF000000
#define MODAL_BTN_W       60
#define MODAL_BTN_H       22

/* Render the modal window's content using the public WM/GFX API so the
 * compositor handles z-order + dirty tracking for us. */
static void modal_paint(window_t *w, const char *title, const char *msg,
                        bool button_hot) {
    if (!w || !w->in_use) return;
    draw_target_t *t = &w->content;

    gfx_clear(t, MODAL_BG);

    /* Top accent stripe with the title. */
    gfx_fill_rect(t, 0, 0, (int)t->width, 18, MODAL_BORDER_LO);
    gfx_draw_string(t, 8, 5, title, MODAL_FG, MODAL_BORDER_LO);

    /* Word-wrapped body text - hand-rolled because we don't have a real
     * text layout engine and the messages are short (<= ~80 chars). */
    int y = 30;
    int line_w = (int)t->width - 16;
    int chars_per_line = line_w / FONT_GLYPH_W;
    if (chars_per_line < 1) chars_per_line = 1;

    const char *p = msg;
    while (*p && y < (int)t->height - MODAL_BTN_H - 12) {
        int n = 0;
        /* Take up to chars_per_line characters or until newline. */
        while (p[n] && p[n] != '\n' && n < chars_per_line) n++;
        /* If we ran out of width mid-word, back up to the last space so
         * we don't split a word in half.  Skip the back-up when there is
         * no space at all on the line. */
        if (p[n] && p[n] != '\n' && n == chars_per_line) {
            int back = n;
            while (back > 0 && p[back] != ' ') back--;
            if (back > 0) n = back;
        }
        /* Draw the slice character by character (no draw_string_n yet). */
        for (int i = 0; i < n; i++) {
            gfx_draw_char(t, 8 + i * FONT_GLYPH_W, y, p[i],
                          MODAL_FG, MODAL_BG);
        }
        y += FONT_GLYPH_H + 2;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }

    /* OK button: centred near the bottom edge. */
    int bx = ((int)t->width - MODAL_BTN_W) / 2;
    int by = (int)t->height - MODAL_BTN_H - 8;
    uint32_t bbg = button_hot ? MODAL_BTN_BG_HOT : MODAL_BTN_BG;
    gfx_fill_rect(t, bx, by, MODAL_BTN_W, MODAL_BTN_H, bbg);
    gfx_draw_hline(t, bx,                   by,                   MODAL_BTN_W, MODAL_BORDER_HI);
    gfx_draw_vline(t, bx,                   by,                   MODAL_BTN_H, MODAL_BORDER_HI);
    gfx_draw_hline(t, bx,                   by + MODAL_BTN_H - 1, MODAL_BTN_W, MODAL_BORDER_LO);
    gfx_draw_vline(t, bx + MODAL_BTN_W - 1, by,                   MODAL_BTN_H, MODAL_BORDER_LO);
    int tx = bx + (MODAL_BTN_W - 2 * FONT_GLYPH_W) / 2;
    int ty = by + (MODAL_BTN_H - FONT_GLYPH_H) / 2;
    gfx_draw_string(t, tx, ty, L(STR_BTN_OK), MODAL_BTN_FG, bbg);
}

/* Returns true when the screen point (gx, gy) lies inside the OK button
 * of the given modal window.  The button is positioned in
 * content-local coords inside modal_paint() so we re-derive the same
 * geometry here. */
static bool hit_ok_button(window_t *w, int gx, int gy) {
    if (!w || !w->in_use) return false;
    int cx = w->x + WM_BORDER;
    int cy = w->y + WM_BORDER + WM_TITLE_H + 1;
    int bx = cx + ((int)w->content.width - MODAL_BTN_W) / 2;
    int by = cy + (int)w->content.height - MODAL_BTN_H - 8;
    return gx >= bx && gx < bx + MODAL_BTN_W &&
           gy >= by && gy < by + MODAL_BTN_H;
}

/* Tear down every non-protected window before we even try to render the
 * modal.  Rationale: the subsystem that just faulted may have left its
 * window in a half-initialised state (NULL framebuffer, mid-resize, a
 * destroy_cb that the kernel was halfway through calling).  Letting
 * wm_tick() composite that wreckage in the background of the modal is
 * how we end up in the "modal shows but the system is frozen" failure
 * mode the user reported.  Sweep first, paint after. */
static void sweep_crashed_windows(void) {
    window_t *wins[WM_MAX_WINDOWS];
    int n = wm_get_windows(wins, WM_MAX_WINDOWS);
    for (int i = 0; i < n; i++) {
        if (!wins[i] || !wins[i]->in_use) continue;
        if (wins[i]->protected) continue;
        wm_pop_modal(wins[i]);
        wm_destroy_window(wins[i]);
    }
}

/* Direct-to-framebuffer fallback dialog used when the WM is too broken
 * to host another window.  Paints a centred red panel straight onto the
 * VBE LFB and spins on keyboard input.  Does NOT call wm_tick(), does
 * NOT depend on the mouse driver, does NOT need a working pool
 * allocator — exactly the safety net the user asked for when the
 * "Recoverable Error" modal stops responding. */
static void show_safe_dialog(const char *title, const char *msg) {
    extern void *vga_framebuffer(void);
    uint8_t *fb = (uint8_t *)vga_framebuffer();
    if (!fb) {
        /* No framebuffer either — fall back to the serial console. */
        debug_printf("\n[safe-dialog] %s: %s\n", title, msg);
        for (;;) {
            __asm__ volatile ("sti; hlt");
            if (keyboard_has_data()) {
                int k = keyboard_getc();
                if (k == '\n' || k == ' ' || k == KEY_ESCAPE) return;
            }
        }
    }
    int sw = (int)vga_width();
    int sh = (int)vga_height();
    int pw = 520, ph = 200;
    int px = (sw - pw) / 2; if (px < 0) px = 0;
    int py = (sh - ph) / 2; if (py < 0) py = 0;

    /* Borderless fill straight onto the LFB. */
    for (int y = 0; y < ph; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint32_t)(py + y) * sw * 4 + (uint32_t)px * 4);
        for (int x = 0; x < pw; x++) row[x] = MODAL_BG;
    }
    /* Header strip with the title. */
    for (int y = 0; y < 22; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint32_t)(py + y) * sw * 4 + (uint32_t)px * 4);
        for (int x = 0; x < pw; x++) row[x] = MODAL_BORDER_LO;
    }
    /* Build a synthetic draw target that points straight at the LFB
     * so gfx_draw_string + glyph blits work without the WM. */
    draw_target_t safe_dt;
    safe_dt.fb      = fb;
    safe_dt.pitch   = (uint32_t)sw * 4;
    safe_dt.width   = (uint32_t)sw;
    safe_dt.height  = (uint32_t)sh;
    safe_dt.clip_on = false;          /* no scissor on the panic surface */
    gfx_draw_string(&safe_dt, px + 8, py + 6, title, MODAL_FG, MODAL_BORDER_LO);

    /* Word-wrapped body. */
    int y = py + 36;
    int chars_per_line = (pw - 16) / FONT_GLYPH_W;
    if (chars_per_line < 1) chars_per_line = 1;
    const char *p = msg;
    while (*p && y < py + ph - 40) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < chars_per_line) n++;
        if (p[n] && p[n] != '\n' && n == chars_per_line) {
            int back = n;
            while (back > 0 && p[back] != ' ') back--;
            if (back > 0) n = back;
        }
        for (int i = 0; i < n; i++) {
            gfx_draw_char(&safe_dt, px + 8 + i * FONT_GLYPH_W, y,
                          p[i], MODAL_FG, MODAL_BG);
        }
        y += FONT_GLYPH_H + 2;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }
    const char *foot = "[Enter/Esc/Space] dismiss   [Ctrl+R] reboot";
    int fy = py + ph - 24;
    for (int i = 0; foot[i] && (px + 8 + i * FONT_GLYPH_W) < (px + pw - 8); i++) {
        gfx_draw_char(&safe_dt, px + 8 + i * FONT_GLYPH_W, fy,
                      foot[i], MODAL_FG, MODAL_BG);
    }

    /* Drain leftover keys, then accept any dismiss key. */
    while (keyboard_has_data()) (void)keyboard_getc();
    for (;;) {
        __asm__ volatile ("sti; hlt");
        if (!keyboard_has_data()) continue;
        int k = keyboard_getc();
        if (k == '\n' || k == '\r' || k == ' ' || k == KEY_ESCAPE) return;
        /* Ctrl+R = ASCII DC2 (0x12) on most layouts; reboot via 8042. */
        if (k == 0x12) {
            for (int i = 0; i < 100; i++) {
                outb(0x64, 0xFE);
                for (volatile int j = 0; j < 100000; j++) {}
            }
            return; /* unreachable */
        }
    }
}

static void show_modal_and_wait(const char *title, const char *msg) {
    /* SAFETY FIRST: drop the crashed subsystem's window(s) before we
     * touch the WM compositor again.  Without this, wm_tick() inside
     * the modal loop walks a freed-but-still-listed framebuffer and
     * locks up the kernel — the visible "frozen with the modal up"
     * symptom. */
    sweep_crashed_windows();

    /* Centre the modal on the screen. */
    int sx = ((int)vga_width()  - MODAL_W) / 2; if (sx < 0) sx = 0;
    int sy = ((int)vga_height() - MODAL_H) / 2; if (sy < 0) sy = 0;

    window_t *w = wm_create_window(sx, sy, MODAL_W, MODAL_H, title);
    if (!w) {
        /* Window slot exhausted (a child window leaked its slot) — drop
         * to the WM-independent safety dialog so the user is never left
         * staring at a frozen screen with no way out. */
        debug_printf("[recoverable_panic] WM unavailable, using safe dialog\n");
        show_safe_dialog(title, msg);
        return;
    }
    wm_set_focus(w);
    /* The panic dialog is itself a modal - keep it on top of any other
     * modal owned by the subsystem that just crashed (e.g. the editor)
     * so clicks land on us, not on the dying window underneath. */
    wm_push_modal(w);

    /* Drain any queued keystrokes so a fast typist's input cannot
     * dismiss the modal before they see it. */
    while (keyboard_has_data()) (void)keyboard_getc();

    bool was_hot = false;
    modal_paint(w, title, msg, false);
    wm_mark_dirty();
    wm_present();

    /* Watchdog: if more than this many idle ticks pass with no input,
     * we assume the WM has wedged (an IRQ stopped firing, a callback
     * looped, etc.) and fall through to the LFB-direct safe dialog.
     * 30 s @ 100 Hz PIT = 3000 ticks is a generous upper bound for a
     * human to find the OK button.  This is the second half of "the
     * system never freezes" — guaranteeing forward progress. */
    uint32_t start_ms = pit_ms();
    bool dismissed = false;

    for (;;) {
        if (keyboard_has_data()) {
            int k = keyboard_getc();
            /* Any "common" dismissal key works, plus Ctrl+R for reboot. */
            if (k == '\n' || k == '\r' || k == ' ' || k == KEY_ESCAPE) {
                dismissed = true;
                break;
            }
            if (k == 0x12) {  /* Ctrl+R */
                for (int i = 0; i < 100; i++) {
                    outb(0x64, 0xFE);
                    for (volatile int j = 0; j < 100000; j++) {}
                }
            }
            continue;
        }
        int mx = mouse_x(), my = mouse_y();
        uint8_t btn = mouse_btn();
        bool now_hot = hit_ok_button(w, mx, my);
        if (now_hot != was_hot) {
            was_hot = now_hot;
            modal_paint(w, title, msg, now_hot);
            wm_mark_dirty();
        }
        if (now_hot && (btn & MOUSE_BTN_LEFT)) {
            dismissed = true;
            break;
        }
        wm_tick();
        __asm__ volatile ("sti; hlt");

        if ((pit_ms() - start_ms) > 30000u) {
            /* WM looks wedged — fall through to the LFB-direct dialog
             * so the user always has a working escape hatch. */
            debug_printf("[recoverable_panic] WM watchdog tripped, escalating to safe dialog\n");
            break;
        }
    }

    wm_pop_modal(w);
    wm_destroy_window(w);

    if (!dismissed) {
        show_safe_dialog(title, msg);
    }

    /* Final sweep — the subsystem that crashed may have spawned more
     * windows after we ran the first sweep (rare, but harmless to
     * repeat). */
    sweep_crashed_windows();

    wm_refocus_shell();
    wm_mark_dirty();
    wm_present();
}

/* Format the printf-style message into a heap-free static buffer so we
 * never touch a malloc from inside a panic.  The buffer is large enough
 * for the longest realistic driver error. */
static void format_recovery_msg(char *out, uint32_t cap,
                                const char *fmt, va_list ap) {
    kvsnprintf(out, cap, fmt, ap);
}

NORETURN void recoverable_panic(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    format_recovery_msg(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    debug_puts("\n[recoverable_panic] ");
    debug_puts(buf);
    debug_putc('\n');

    /* If we got here from before the shell armed recovery, we have nowhere
     * to longjmp to - escalate to a hard halt with the same message.  This
     * happens during boot bringup (no shell yet) and is the right
     * behaviour: a watchdog firing pre-shell means the OS cannot run. */
    if (!g_panic_recovery_armed) {
        panic("%s", buf);
    }

    /* Disarm BEFORE we touch the WM so a second exception triggered by
     * the modal render path can't re-enter this function in a loop. */
    g_panic_recovery_armed = false;

    /* CPU exceptions arrive with IF=0 (interrupt-gate semantics).  The
     * WM tick loop needs interrupts on to make progress (keyboard and
     * mouse IRQs).  Enable them now - we're past the unsafe instant. */
    __asm__ volatile ("sti");

    show_modal_and_wait(L(STR_PANIC_RECOVERABLE), buf);

    /* If the crash happened inside a ring-3 launch the kernel side of
     * usermode.c still thinks the task is running.  Clear it before we
     * longjmp away — otherwise the NEXT apps_launch sees the stale
     * g_user_running=true and silently skips the CPL=3 transition. */
    usermode_reset_after_panic();

    /* Hand control back to the shell's main loop. */
    longjmp(g_panic_recovery, 1);
}

/* ====================================================================== */
/*  Register / stack dump - arch-aware, shared by every panic sink         */
/* ====================================================================== */
/* A panic must spill the trap frame to three very different sinks: the
 * framebuffer terminal (term_printf), the COM1 serial log (debug_puts) and
 * the on-disk crash log (a growing char buffer).  Rather than triplicate the
 * arch-specific register layout, each line is formatted once with ksnprintf
 * and handed to a sink callback.  On x86_64 this is a TRUE 64-bit RSOD: all
 * 16 GP registers plus RIP/RSP/RFLAGS/CR2 print at full width (kvsnprintf
 * gained real %016lx in Phase 16). */
typedef void (*panic_sink_fn)(void *ctx, const char *line);

static void sink_term(void *ctx, const char *s)   { (void)ctx; term_printf("%s", s); }
static void sink_serial(void *ctx, const char *s) { (void)ctx; debug_puts(s); }

struct crashbuf { char *buf; uint32_t cap; uint32_t off; };
static void sink_crashbuf(void *ctx, const char *s) {
    struct crashbuf *cb = (struct crashbuf *)ctx;
    if (cb->off < cb->cap)
        cb->off += (uint32_t)ksnprintf(cb->buf + cb->off, cb->cap - cb->off, "%s", s);
}

static void dump_regs(registers_t *r, panic_sink_fn sink, void *ctx) {
    char b[160];
    if (!r) return;
#if defined(__x86_64__)
    ksnprintf(b, sizeof b, " RIP=0x%016lx  CS =0x%04lx  RFLAGS=0x%016lx\n",
              (unsigned long)r->rip, (unsigned long)(r->cs & 0xFFFF),
              (unsigned long)r->rflags); sink(ctx, b);
    ksnprintf(b, sizeof b, " RSP=0x%016lx  SS =0x%04lx\n",
              (unsigned long)r->rsp, (unsigned long)(r->ss & 0xFFFF)); sink(ctx, b);
    ksnprintf(b, sizeof b, " RAX=0x%016lx  RBX=0x%016lx\n",
              (unsigned long)r->rax, (unsigned long)r->rbx); sink(ctx, b);
    ksnprintf(b, sizeof b, " RCX=0x%016lx  RDX=0x%016lx\n",
              (unsigned long)r->rcx, (unsigned long)r->rdx); sink(ctx, b);
    ksnprintf(b, sizeof b, " RSI=0x%016lx  RDI=0x%016lx\n",
              (unsigned long)r->rsi, (unsigned long)r->rdi); sink(ctx, b);
    ksnprintf(b, sizeof b, " RBP=0x%016lx  R8 =0x%016lx\n",
              (unsigned long)r->rbp, (unsigned long)r->r8); sink(ctx, b);
    ksnprintf(b, sizeof b, " R9 =0x%016lx  R10=0x%016lx\n",
              (unsigned long)r->r9, (unsigned long)r->r10); sink(ctx, b);
    ksnprintf(b, sizeof b, " R11=0x%016lx  R12=0x%016lx\n",
              (unsigned long)r->r11, (unsigned long)r->r12); sink(ctx, b);
    ksnprintf(b, sizeof b, " R13=0x%016lx  R14=0x%016lx\n",
              (unsigned long)r->r13, (unsigned long)r->r14); sink(ctx, b);
    ksnprintf(b, sizeof b, " R15=0x%016lx\n", (unsigned long)r->r15); sink(ctx, b);
    if (r->int_no == 14) {
        ksnprintf(b, sizeof b, " CR2=0x%016lx   <-- faulting linear address\n",
                  (unsigned long)read_cr2()); sink(ctx, b);
    }
#else
    ksnprintf(b, sizeof b, " EIP=0x%08x  CS =0x%04x  EFLAGS=0x%08x\n",
              r->eip, r->cs & 0xFFFF, r->eflags); sink(ctx, b);
    ksnprintf(b, sizeof b, " EAX=0x%08x  EBX=0x%08x  ECX=0x%08x  EDX=0x%08x\n",
              r->eax, r->ebx, r->ecx, r->edx); sink(ctx, b);
    ksnprintf(b, sizeof b, " ESI=0x%08x  EDI=0x%08x  EBP=0x%08x  ESP=0x%08x\n",
              r->esi, r->edi, r->ebp, (uint32_t)(uintptr_t)r); sink(ctx, b);
    ksnprintf(b, sizeof b, " DS =0x%04x  CR2=0x%08x\n",
              r->ds & 0xFFFF, (uint32_t)read_cr2()); sink(ctx, b);
#endif
}

NORETURN void recoverable_panic_from_exception(registers_t *r) {
    char buf[256];
#if defined(__x86_64__)
    ksnprintf(buf, sizeof(buf),
              "CPU %s (vec %lu, err 0x%lx) at RIP=0x%016lx. "
              "The subsystem that caused this has been terminated; "
              "shell is recovering.",
              exception_name((uint8_t)r->int_no),
              (unsigned long)r->int_no, (unsigned long)r->err_code,
              (unsigned long)r->rip);
#else
    ksnprintf(buf, sizeof(buf),
              "CPU %s (vec %u, err 0x%x) at EIP=0x%08x. "
              "The subsystem that caused this has been terminated; "
              "shell is recovering.",
              exception_name((uint8_t)r->int_no),
              r->int_no, r->err_code, r->eip);
#endif
    recoverable_panic("%s", buf);
}

/* Write a crash dump to /sys/crash.log on NXFS.
 * Called with interrupts disabled just before the RSOD freeze.
 * Best-effort: any NXFS error is silently ignored. */
static void write_crash_dump(const char *kind, registers_t *r,
                              const char *msg) {
    /* Resolve /sys directory then crash.log inside it. */
    uint32_t root_inode = 1;
    uint32_t sys_inode  = 0;
    if (nxfs_resolve(root_inode, "sys", &sys_inode) != 0) return;

    /* Try to create or re-use crash.log */
    uint32_t log_inode = 0;
    if (nxfs_resolve(sys_inode, "crash.log", &log_inode) != 0) {
        if (nxfs_create_file(sys_inode, "crash.log", &log_inode) != 0)
            return;
    }

    char buf[1024];
    int  off = 0;

    /* Timestamp */
    char ts[32] = "";
    rtc_time_t now;
    rtc_now(&now);
    rtc_format_datetime(&now, ts, sizeof(ts));
    off += ksnprintf(buf + off, sizeof(buf) - (uint32_t)off,
                     "=== CRASH DUMP [%s, uptime %u ms] ===\n",
                     ts, pit_ms());
    off += ksnprintf(buf + off, sizeof(buf) - (uint32_t)off,
                     "Type: %s\n", kind);
    if (msg && *msg)
        off += ksnprintf(buf + off, sizeof(buf) - (uint32_t)off,
                         "Msg : %s\n", msg);

    if (r) {
        off += ksnprintf(buf + off, sizeof(buf) - (uint32_t)off,
                         "Vec : %u (%s)  Err=0x%x\n",
                         (unsigned)r->int_no, exception_name((uint8_t)r->int_no),
                         (unsigned)r->err_code);
        struct crashbuf cb = { buf, sizeof(buf), (uint32_t)off };
        dump_regs(r, sink_crashbuf, &cb);
        off = (int)cb.off;
    }
    off += ksnprintf(buf + off, sizeof(buf) - (uint32_t)off,
                     "=== END ===\n");

    nxfs_write_file(log_inode, buf, (uint32_t)off);
}

NORETURN void panic_from_exception(registers_t *r) {
    cli();

    /* Write crash dump to /sys/crash.log before touching the framebuffer. */
    write_crash_dump("CPU Exception", r, NULL);

    /* serial first - so we get the dump even if the framebuffer is dead */
    debug_puts("\n\n!!! KERNEL PANIC -- CPU EXCEPTION !!!\n");
    {
        char hdr[96];
        ksnprintf(hdr, sizeof hdr, "vec=%u (%s)  err=0x%x\n",
                  (unsigned)r->int_no, exception_name((uint8_t)r->int_no),
                  (unsigned)r->err_code);
        debug_puts(hdr);
    }
    dump_regs(r, sink_serial, NULL);

    /* If the shell has armed recovery AND the exception is one we know
     * how to step over (page fault, divide-by-zero, invalid opcode,
     * general-protection, x87, SIMD), redirect to the recoverable
     * path instead of halting the box.  Double-fault / machine-check
     * stay on the hard-halt path because they imply the kernel state
     * itself is corrupt. */
    bool can_recover = g_panic_recovery_armed && (
        r->int_no == 0  || r->int_no == 6  || r->int_no == 13 ||
        r->int_no == 14 || r->int_no == 16 || r->int_no == 19);
    if (can_recover) {
        debug_printf("[panic] recovery armed - routing to recoverable_panic\n");
        recoverable_panic_from_exception(r);
        /* recoverable_panic does not return */
    }

    /* framebuffer dump */
    render_header(L(STR_PANIC_CAUSE_CPU));
    term_printf(" Vector  : %u  (%s)\n", (unsigned)r->int_no,
                exception_name((uint8_t)r->int_no));
    term_printf(" Err Code: 0x%x\n\n", (unsigned)r->err_code);
    dump_regs(r, sink_term, NULL);
    term_printf("\n");

    /* Error code decoding for the page fault, since it's the most common
     * crash an OS dev will hit during bring-up. */
    if (r->int_no == 14) {
        uint32_t e = (uint32_t)r->err_code;
        term_printf(" #PF flags: %s, %s, %s%s%s\n",
                    (e & 1) ? "page-protection" : "non-present",
                    (e & 2) ? "write"            : "read",
                    (e & 4) ? "user"             : "kernel",
                    (e & 8) ? ", reserved-bit"   : "",
                    (e & 16)? ", instr-fetch"    : "");
    } else if (r->int_no == 13) {
        uint32_t e = (uint32_t)r->err_code;
        term_printf(" #GP flags: %s segment selector idx 0x%x %s\n",
                    (e & 1) ? "external" : "internal",
                    (e >> 3) & 0x1FFF,
                    (e & 4) ? "(IDT)" : "(GDT/LDT)");
    }

    /* Raw hex stack trace.  `r` sits at the saved stack pointer the ISR stub
     * pushed onto the kernel stack, so walking from there spots the culprit
     * call chain by eye (or feed an address to addr2line). */
    term_printf("\n");
    dump_stack_trace((uintptr_t *)r, 32);

    term_printf("\n%s\n", L(STR_PANIC_HALTED));
    freeze();
}

NORETURN void panic(const char *fmt, ...) {
    cli();

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Write crash dump to /sys/crash.log before touching the framebuffer. */
    write_crash_dump("Kernel Panic", NULL, buf);

    debug_puts("\n\n!!! KERNEL PANIC !!!\n");
    debug_puts(buf);
    debug_putc('\n');

    render_header(L(STR_PANIC_CAUSE_KERNEL));
    term_printf("%s\n", buf);
    term_printf("\n%s\n", L(STR_PANIC_HALTED));
    freeze();
}
