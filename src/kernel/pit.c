/* ============================================================================
 * NexxoN OS - 8254 PIT driver
 * ----------------------------------------------------------------------------
 * Channel 0 is wired into IRQ0 and used as our system tick source.  The
 * timer ISR drives the idle-spinner animation in the terminal so the user
 * always sees that the OS is alive even while blocked on keyboard input.
 * ============================================================================ */
#include "pit.h"
#include "io.h"
#include "irq.h"
#include "isr.h"
#include "pic.h"
#include "terminal.h"
#include "debug.h"
#include "sched.h"
#include "keyboard.h"   /* ps2_dispatch(): IRQ0 safety-drain of the i8042 */

#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43
#define PIT_BASE_FREQ 1193182u

static volatile uint32_t g_ticks = 0;
static uint32_t          g_hz    = PIT_DEFAULT_HZ;

/* ---- Real CPU-usage gauge (idle vs busy ticks) ----------------------- *
 * pit_idle_hlt() parks the CPU in `hlt` with g_cpu_idle=1 around it; every
 * timer tick samples that flag, so busy/(busy+idle) over a window is the true
 * CPU utilisation (low when the idle loop mostly hlts, high under real load).
 * Replaces the old Task Manager heuristic that just counted redraw calls. */
static volatile uint32_t g_cpu_idle       = 0;
static volatile uint32_t g_cpu_busy_ticks = 0;
static volatile uint32_t g_cpu_idle_ticks = 0;

/* Spinner update prescaler.  At 100 Hz, every 8 ticks ~= 12.5 frames/sec. */
#define SPINNER_DIVIDER 8

static void pit_isr(registers_t *r) {
    /* Send the IRQ0 EOI *up front*, before sched_tick() may ctx_switch
     * away.  This guarantees EXACTLY ONE EOI per IRQ0 assertion:
     *   - irq_handler() deliberately skips its trailing EOI for IRQ0, so
     *     there is no longer a duplicate when the preempted task later
     *     unwinds back through here.
     *   - brand-new tasks entered via task_trampoline (which bypass
     *     irq_common_stub) are already covered because the controller is
     *     released here before the switch.
     * The previous design EOI'd inside sched_tick only when it switched,
     * and then irq_handler EOI'd again -- a double EOI that wedged the
     * 8259 on bare metal once mouse (IRQ12) traffic started. */
    pic_send_eoi(0);

    g_ticks++;
    if (g_cpu_idle) g_cpu_idle_ticks++; else g_cpu_busy_ticks++;
    if ((g_ticks % SPINNER_DIVIDER) == 0) {
        term_idle_tick();
    }

    /* Safety-drain the shared PS/2 controller from the always-present
     * 100 Hz timer.  If IRQ1/IRQ12 delivery is unreliable (VirtualBox and
     * some bare-metal 8042s drop the mouse IRQ entirely), undrained mouse
     * bytes would otherwise fill the output buffer, wedge the keyboard, and
     * freeze the cursor.  Routed by the AUX status bit in ps2_dispatch().
     * Runs BEFORE sched_tick() because the latter may ctx_switch away and
     * not return here for a full quantum. */
    ps2_dispatch();

    sched_tick(r);
}

void pit_init(uint32_t hz) {
    if (hz == 0) hz = PIT_DEFAULT_HZ;
    g_hz = hz;
    uint32_t divisor = PIT_BASE_FREQ / hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1)     divisor = 1;

    debug_step("pit: programming channel 0");
    /* Mode 3: square-wave generator on channel 0, lobyte/hibyte access. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, pit_isr);
    /* Bare-metal IRQ-routing fix: pic_remap() now masks every line
     * except the slave cascade.  Drivers MUST unmask their own IRQ
     * once the handler is installed.  Without this the PIT never
     * fires and pit_ms() never advances - which would deadlock every
     * PS/2 / e1000 / sleep loop downstream. */
    pic_unmask(0);
    debug_printf("[ OK ] pit: %u Hz (divisor=%u, IRQ0 unmasked)\n", hz, divisor);
}

uint32_t pit_ticks(void) { return g_ticks; }
uint32_t pit_ms   (void) { return (g_ticks * 1000u) / g_hz; }

/* Park the CPU in the idle loop, flagged so the timer ISR counts these ticks
 * as idle (not busy).  Use this instead of a bare `sti; hlt` in idle loops. */
void pit_idle_hlt(void) {
    g_cpu_idle = 1;
    __asm__ volatile ("sti; hlt");
    g_cpu_idle = 0;
}

/* Real CPU utilisation since the last call (busy ticks / total ticks),
 * 0..100; resets the accumulators so the caller samples a fresh window. */
uint32_t pit_cpu_usage_pct(void) {
    uint32_t busy = g_cpu_busy_ticks, idle = g_cpu_idle_ticks;
    g_cpu_busy_ticks = 0;
    g_cpu_idle_ticks = 0;
    uint32_t tot = busy + idle;
    return tot ? (busy * 100u) / tot : 0;
}

/* Re-assert the channel-0 rate after a firmware call.  The VBE trampoline
 * hands the CPU to the video BIOS (INT 10h) in real mode; some firmware
 * reprograms PIT channel 0 for its own delay loops and leaves it that way.
 * From then on every pit_ms()-derived timeout in the OS runs at the wrong
 * speed — on the user's board the 12 s keep/revert grace expired visibly
 * early.  Reprogramming is idempotent, so callers run it after EVERY
 * trampoline round-trip.  The status read-back (latch command 0xE2) is
 * logged FIRST so COM1 shows what the BIOS actually left in the channel:
 * a healthy channel reads mode 3 / lobyte+hibyte (status 0x36 family). */
void pit_reprogram(void) {
    outb(PIT_CMD, 0xE2);                    /* read-back: status of ch 0   */
    uint8_t status = inb(PIT_CH0_DATA);
    uint32_t divisor = PIT_BASE_FREQ / g_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1)     divisor = 1;
    uintptr_t eflags;
#if defined(__x86_64__)
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(eflags));
#else
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(eflags));
#endif
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
    if (eflags & 0x200) __asm__ volatile ("sti");
    debug_printf("[pit] reprogram after BIOS call: ch0 status=0x%02x "
                 "(mode %u, access %u) -> mode 3 @ %u Hz (divisor=%u)\n",
                 status, (status >> 1) & 7u, (status >> 4) & 3u, g_hz, divisor);
}

void pit_sleep(uint32_t ms) {
    uint32_t end = pit_ms() + ms;
    while (pit_ms() < end) {
        __asm__ volatile ("hlt");
    }
}

/* Microsecond-resolution clock for sub-tick pacing (pointer smoothing).
 * The PIT IRQ ticks at 100 Hz — the SAME cadence as the input reports we
 * want to smooth — so tick granularity is useless for spreading motion
 * between compositor frames.  Instead read the LIVE channel-0 down-counter
 * between IRQs.  Returns microseconds since boot; WRAPS every ~71 min, so
 * callers must only use DIFFERENCES of two reads (unsigned subtraction
 * stays correct across the wrap).
 *
 * Mode 3 detail: the counter is loaded with the divisor N and decrements
 * by TWO each 1.193182 MHz clock, once per OUTPUT HALF-period; OUT
 * (status bit 7) is high in the first half, low in the second.  Read-back
 * command 0xC2 latches status+count of channel 0 atomically, and the
 * 8254 holds the latched bytes until they are read, so an IRQ0 firing
 * mid-sequence cannot corrupt them (pit_isr never touches 0x40/0x43).
 * The g_ticks before/after retry pins the fraction to the right tick. */
uint32_t pit_now_us(void) {
    uint32_t divisor = PIT_BASE_FREQ / g_hz;
    if (divisor < 2) divisor = 2;
    uint32_t period_us = 1000000u / g_hz;
    uint32_t t0, t1, count;
    uint8_t  status;
    int guard = 4;
    do {
        t0 = g_ticks;
        outb(PIT_CMD, 0xC2);                /* read-back ch0: status+count */
        status = inb(PIT_CH0_DATA);
        uint8_t lo = inb(PIT_CH0_DATA);
        uint8_t hi = inb(PIT_CH0_DATA);
        count = ((uint32_t)hi << 8) | lo;
        t1 = g_ticks;
    } while (t0 != t1 && --guard > 0);
    if (count == 0 || count > divisor) count = divisor;  /* mid-reload glitch */
    uint32_t in_half = (divisor - count) / 2;            /* clocks into half  */
    uint32_t clocks  = (status & 0x80) ? in_half
                                       : divisor / 2 + in_half;
    return t1 * period_us + clocks * period_us / divisor;
}
