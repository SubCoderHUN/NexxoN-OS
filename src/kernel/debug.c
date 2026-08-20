/* ============================================================================
 * NexxoN OS - COM1 serial debugger implementation
 * ----------------------------------------------------------------------------
 * Funnels every formatted-output request through kvsnprintf() so that fmt
 * behaviour - width padding, left-justify, zero-pad - is identical to the
 * framebuffer terminal.
 * ============================================================================ */
#include "debug.h"
#include "io.h"
#include "string.h"

#define COM1            0x3F8
#define UART_DATA       (COM1 + 0)
#define UART_INT_EN     (COM1 + 1)
#define UART_DIV_LO     (COM1 + 0)
#define UART_DIV_HI     (COM1 + 1)
#define UART_FIFO_CTRL  (COM1 + 2)
#define UART_LINE_CTRL  (COM1 + 3)
#define UART_MODEM_CTRL (COM1 + 4)
#define UART_LINE_STATUS (COM1 + 5)

#define LSR_THR_EMPTY   0x20

static bool g_serial_up = false;

void debug_init(void) {
    outb(UART_INT_EN,     0x00);
    outb(UART_LINE_CTRL,  0x80);
    outb(UART_DIV_LO,     0x03);
    outb(UART_DIV_HI,     0x00);
    outb(UART_LINE_CTRL,  0x03);
    outb(UART_FIFO_CTRL,  0xC7);
    outb(UART_MODEM_CTRL, 0x0B);
    g_serial_up = true;
}

static inline int serial_tx_ready(void) {
    return inb(UART_LINE_STATUS) & LSR_THR_EMPTY;
}

void debug_putc(char c) {
    if (!g_serial_up) return;
    if (c == '\n') {
        while (!serial_tx_ready()) {}
        outb(UART_DATA, '\r');
    }
    while (!serial_tx_ready()) {}
    outb(UART_DATA, (uint8_t)c);
}

void debug_puts(const char *s) {
    while (*s) debug_putc(*s++);
}

void debug_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    debug_puts(buf);
}

void debug_step(const char *what) { debug_printf("[ .. ] %s\n", what); }
void debug_ok  (const char *what) { debug_printf("[ OK ] %s\n", what); }
void debug_fail(const char *what, const char *why) {
    debug_printf("[FAIL] %s :: %s\n", what, why ? why : "unknown");
}

void debug_banner(void) {
    debug_puts(
        "\n"
        "================================================================\n"
        "  NexxoN OS  -  x86_64 (64-bit) micro-kernel  -  build "
            __DATE__ " " __TIME__ "\n"
        "================================================================\n"
        "\n");
}
