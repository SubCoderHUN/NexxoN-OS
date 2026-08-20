/* ============================================================================
 * NexxoN OS - Kernel entry point
 * ----------------------------------------------------------------------------
 * Called by boot.asm with:
 *      kernel_main(uint32_t magic, multiboot_info_t *mbi)
 *
 * Responsibilities (in strict order, every step logged to COM1):
 *      1. Init COM1 serial so subsequent failures are visible.
 *      2. Validate Multiboot magic.
 *      3. Init GDT/IDT/PIC/PIT/keyboard.
 *      4. Init VGA framebuffer + terminal.
 *      5. Init ATA + NXFS filesystem.
 *      6. Enable interrupts and hand control to the shell.
 *
 * Any failure escalates to panic() which renders a kernel-debugger screen.
 * ============================================================================ */
#include "sysdisk.h"
#include "types.h"
#include "io.h"
#include "string.h"
#include "debug.h"
#include "multiboot.h"
#include "vga.h"
#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "ahci.h"
#include "nxfs.h"
#include "shell.h"
#include "panic.h"
#include "font.h"
#include "window.h"
#include "nxscript.h"
#include "acpi.h"
#include "rtc.h"
#include "tasktimer.h"
#include "boot_info.h"
#include "desktop.h"
#include "speaker.h"
#include "mtrr.h"
#include "syscall.h"
#include "e1000.h"
#include "netif.h"
#include "net.h"
#include "usb.h"
#include "usbhid.h"
#include "vfs.h"
#include "pnp.h"
#include "i18n.h"
#include "auth.h"
#include "apps.h"
#include "theme.h"

extern const netif_driver_t e1000_driver;
extern const netif_driver_t pcnet_driver;
extern const netif_driver_t rtl_driver;

/* Adapter from WM resize callback signature -> terminal's
 * retarget-preserve API.  Keeps console scrollback alive during a drag
 * and re-rasterises every cell into the resized framebuffer in a single
 * pass — no black flash, no lost lines. */
static void shell_window_resize_cb(window_t *w) {
    if (!w) return;
    term_retarget_preserve(&w->content);
}

static void draw_splash(void) {
    /* A simple coloured banner drawn while the user waits for bring-up. */
    term_clear();
    term_set_color(VGA_GREEN, VGA_BLACK);
    term_printf("================================================================\n");
    term_printf("                                                                \n");
    term_printf("                  N  E  X  X  O  N        O  S                 \n");
    term_printf("                                                                \n");
    term_printf("        x86_64 (64-bit) micro-kernel    -    built %s            \n", __DATE__);
    term_printf("                                                                \n");
    term_printf("================================================================\n\n");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
}

static void boot_log(const char *what) {
    term_set_color(VGA_YELLOW, VGA_BLACK);
    term_printf("[ .. ] ");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    term_printf("%s\n", what);
}
static void boot_ok(const char *what) {
    term_set_color(VGA_GREEN, VGA_BLACK);
    term_printf("[ OK ] ");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    term_printf("%s\n", what);
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    /* --------------------------------------------------------------------
     * Stage 0: serial debugger.  This MUST be first so that everything
     * else has a fallback channel even on a totally black screen.
     * -------------------------------------------------------------------- */
    debug_init();
    debug_banner();
    debug_printf("kernel_main(magic=0x%x, mbi=%p)\n", magic, (uint32_t)(uintptr_t)mbi);

    /* --------------------------------------------------------------------
     * Stage 1: validate that GRUB really gave us what we asked for.  The
     * Multiboot 1 spec mandates 0x2BADB002 in EAX.  Anything else means
     * something dropped us off in a broken state.
     * -------------------------------------------------------------------- */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        debug_printf("FATAL: bad multiboot magic 0x%x\n", magic);
        /* We have no framebuffer, no IDT - just hang. */
        for (;;) __asm__ volatile ("cli; hlt");
    }
    debug_ok("multiboot magic verified");

    /* Capture the BIOS drive + the kernel-payload module right away so
     * installsys later in this boot can find them.  No allocations, no
     * dereferences past mbi - safe to call this early. */
    boot_info_init(magic, mbi);

    /* --------------------------------------------------------------------
     * Stage 2: descriptor tables - replace GRUB's GDT with our own, then
     * install the IDT so any subsequent fault produces a panic dump.
     * -------------------------------------------------------------------- */
    gdt_init();
    idt_init();
    syscall_init();
    { extern void linux_syscall_init(void); linux_syscall_init(); }

    /* --------------------------------------------------------------------
     * Stage 2b: x87 FPU bring-up.  GCC, with -mno-sse, lowers every
     * `float` / `double` operation in our C code to x87 instructions
     * (FLD / FSTP / FMUL / FILDL / ...).  GRUB leaves CR0 in whatever
     * state BIOS handed it, which on many machines has CR0.EM=1 ("x87
     * emulation"), so the very first x87 op faults with #UD.  The
     * spreadsheet evaluator (sheet.c) and the HLS playlist parser
     * (net/video.c) both hit doubles in their parse paths, and any
     * future driver that uses `double` would crash the same way.
     *
     * Enable the real FPU:
     *   CR0.MP = 1  (Monitor coProcessor)
     *   CR0.EM = 0  (no emulation -- use the on-die x87 directly)
     *   CR0.TS = 0  (no task-switch trap)
     *   CR0.NE = 1  (native FPU exceptions through #MF, not IRQ13)
     * then issue FNINIT to put the FPU in a known state. */
    {
        /* uintptr_t so the CR0 mov picks the native register width (EAX in
         * 32-bit PM, RAX in long mode) - a uint32_t operand mismatches the
         * 64-bit control register.  UL masks keep the upper bits intact. */
        uintptr_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((uintptr_t)1u << 2);       /* clear EM */
        cr0 &= ~((uintptr_t)1u << 3);       /* clear TS */
        cr0 |=  ((uintptr_t)1u << 1);       /* set   MP */
        cr0 |=  ((uintptr_t)1u << 5);       /* set   NE */
        __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
        __asm__ volatile ("fninit");
        debug_ok("fpu: x87 enabled (CR0.MP=1 EM=0 TS=0 NE=1, FNINIT issued)");
    }

    /* TASK 21: IPC bring-up before any subsystem can publish to it. */
    {
        extern void ipc_init(void);
        ipc_init();
    }
    /* TASK 34: generic DMA buffer pool. */
    {
        extern void dma_init(void);
        dma_init();
    }
    /* TASK 35: NX bit + ASLR offset.  Must run before the ELF loader
     * relocates any user-space image. */
    {
        extern bool aslr_init(void);
        aslr_init();
    }
    /* TASK 23: .nxl dynamic library loader exports kernel libc helpers. */
    {
        extern bool nxl_loader_init(void);
        nxl_loader_init();
    }
    /* TASK 24: virtual-memory swap daemon. */
    {
        extern bool swap_init(void);
        swap_init();
    }
    /* TASK 28: NXFS v2 journaling. */
    {
        extern bool journal_init(void);
        journal_init();
    }

    /* --------------------------------------------------------------------
     * Stage 3: PIC remap.  Hardware IRQs must be moved off vectors 0..15
     * before we enable interrupts, otherwise a spurious IRQ would deliver
     * to an exception vector and produce nonsense.
     * -------------------------------------------------------------------- */
    pic_remap();

    /* --------------------------------------------------------------------
     * Stage 4: graphics + terminal.  Once this is up we mirror every
     * subsequent boot-log line on screen as well as on COM1.
     * -------------------------------------------------------------------- */
    debug_step("vga: bringing up linear framebuffer");
    if (!vga_init(mbi)) {
        panic("VGA framebuffer initialisation failed - check multiboot info struct");
    }
    debug_ok("vga: framebuffer up");

    /* TASK 14: stamp the VBE LFB physical range as Write-Combining via
     * MTRRs so post-compositor blits coalesce into burst PCIe writes.
     * On a CPU without MTRRs the call is a logged no-op and the
     * compositor falls back to uncached writes. */
    {
        uint32_t fb_phys = (uint32_t)(uintptr_t)vga_framebuffer();
        /* Cover the LARGEST framebuffer the OS can ever switch to
         * (1920x1080x4) rather than just the boot resolution.  The old
         * code sized the WC region from VGA_W_DEFAULT, so at any higher
         * resolution most of the framebuffer fell OUTSIDE the
         * write-combining range and every composite crawled over uncached
         * PCIe writes — the root cause of "higher resolution = lower FPS"
         * and the slow first desktop frame.  mtrr_set_write_combining
         * rounds this up to a power of two, which a later runtime resolution
         * change stays comfortably inside.
         *
         * Use PADDED max dimensions (2048x1536x4 = 12 MiB -> rounds to 16 MiB):
         * 1920x1080x4 alone is 7.91 MiB and rounds to only 8 MiB, but real GPUs
         * pad the scanline pitch (LinBytesPerScanLine > width*4), so the FB tail
         * at high resolution fell outside an 8 MiB range and stayed uncached
         * (= "higher resolution is slower at the bottom of the screen").  The
         * 16 MiB envelope is still FB-base-aligned (0x..000000) so it never
         * pulls in pre-FB MMIO. */
        uint32_t fb_size = 2048u * 1536u * 4u;
        if (mtrr_supported() &&
            mtrr_set_write_combining(fb_phys, fb_size)) {
            debug_ok("mtrr: LFB range configured as Write-Combining");
        } else {
            debug_printf("[mtrr] write-combining unavailable - blits will "
                         "use the default cache policy\n");
        }
        /* NOTE: PAT write-combining (which overrides a BIOS UC MTRR) is applied
         * later, right after paging is enabled — see the VMM stage below. */
    }

    term_init();
    draw_splash();
    boot_ok ("Multiboot v1 magic verified");
    boot_ok ("GDT loaded (kernel CS=0x08, DS=0x10)");
    boot_ok ("IDT loaded (ISRs 0..31 + IRQs 32..47)");
    boot_ok ("8259A PIC remapped (master=0x20..0x27, slave=0x28..0x2F)");
    boot_ok ("VGA linear framebuffer initialised");
    boot_log("PIT timer: programming channel 0 to 100 Hz ...");

    /* --------------------------------------------------------------------
     * Stage 5: timer + keyboard (requires IRQ infrastructure).
     * -------------------------------------------------------------------- */
    pit_init(PIT_DEFAULT_HZ);
    boot_ok ("PIT timer running @ 100 Hz");
    boot_log("Keyboard: enabling PS/2 controller ...");
    keyboard_init();
    boot_ok ("PS/2 keyboard ready (USB-Legacy compatible)");

    /* Now safe to allow IRQs through. */
    __asm__ volatile ("sti");
    debug_ok("interrupts enabled");

    /* --------------------------------------------------------------------
     * Stage 6: storage stack.  Modern flow:
     *   - PCI scan to find the AHCI 1.0 controller (class 01h/06h/01h).
     *   - AHCI driver brings up the first SATA port and IDENTIFYs the disk.
     *   - NXFS tries to mount from SATA (offset LBA 2048); if no valid
     *     superblock is found, it falls back to a RAMFS Live image so the
     *     OS is usable even on a brand-new machine.
     * -------------------------------------------------------------------- */
    boot_log("PCI: scanning for a SATA/IDE disk (AHCI, then legacy IDE) ...");
    if (sysdisk_init()) {
        char dmsg[80];
        ksnprintf(dmsg, sizeof(dmsg),
                  "Storage: %s disk online (%u MiB)",
                  sysdisk_kind(),
                  (uint32_t)(((uint64_t)sysdisk_sector_count() * 512u) >> 20));
        boot_ok(dmsg);
    } else {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("[NOTE] no SATA/IDE disk found - the OS will boot in Live Mode.\n");
        term_printf("       File changes will be kept in RAM only and lost on reboot.\n");
        /* Surface the AHCI probe breadcrumbs on-screen (serial is invisible on
         * a real board) so a single boot photo shows WHY no disk bound. */
        term_printf("       [ahci] %s\n", ahci_diag());
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    }

    boot_log("NXFS: choosing backend (SATA -> RAMFS fallback) ...");
    int r = nxfs_mount();
    if (r != NXFS_OK) {
        panic("NXFS mount catastrophic failure (code %d)", r);
    }
    /* Boot-source classification.  Multiboot v1's boot_device field is
     * a 32-bit packed BIOS drive number + sub-partition chain.  The
     * top byte (boot_device >> 24) is the BIOS drive ID:
     *      0x00..0x7F  =  floppy / removable
     *      0x80..0xFF  =  fixed disk (HDD / SSD / SATA)
     *      0xE0..0xEF  =  CD-ROM (BIOS extensions / El Torito)
     * We combine that with the NXFS install signature to decide what
     * the user sees, so the message lines up with their experience
     * (booting from HDD always says "Booting from NexxoN HDD"). */
    uint8_t bios_drive = 0;
    if (mbi->flags & MB_FLAG_BOOTDEV) {
        bios_drive = (uint8_t)((mbi->boot_device >> 24) & 0xFF);
    }
    const char *source =
        (bios_drive >= 0xE0 && bios_drive <= 0xEF) ? "CD/USB"   :
        (bios_drive >= 0x80)                       ? "HDD"      :
        (bios_drive != 0)                          ? "floppy"   :
                                                     "(unknown)";
    if (nxfs_mode() == NXFS_MODE_LIVE) {
        boot_ok ("NXFS mounted in LIVE MODE (RAMFS, changes lost on reboot)");
        term_set_color(VGA_CYAN, VGA_BLACK);
        term_printf("       boot source: %s (BIOS drive 0x%02x)\n",
                    source, bios_drive);
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    } else if (nxfs_is_installed()) {
        boot_ok ("Booting from NexxoN HDD (installed image, persistent)");
        term_set_color(VGA_GREEN, VGA_BLACK);
        term_printf("       boot source: %s (BIOS drive 0x%02x) - INSTALLED\n",
                    source, bios_drive);
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    } else {
        boot_ok ("NXFS mounted from SATA (no install signature)");
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("       boot source: %s (BIOS drive 0x%02x) - "
                    "disk found but not stamped by installsys\n",
                    source, bios_drive);
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    }

    /* --------------------------------------------------------------------
     * Stage 7: GUI bring-up - mouse driver, window manager, console window.
     * Once the WM is up we redirect the terminal to the console window's
     * private framebuffer so the shell renders inside a draggable* window
     * instead of straight onto the screen.  (* drag not wired in v4.0)
     * -------------------------------------------------------------------- */
    acpi_init();
    /* TASK 27: SMP bring-up.  Best-effort; fall back to UP. */
    {
        extern bool smp_init(void);
        extern int  smp_cpu_count(void);
        if (smp_init()) {
            char buf[64];
            ksnprintf(buf, sizeof(buf),
                      "SMP: %d processors online (APIC trampoline @ 0x8000)",
                      smp_cpu_count());
            term_set_color(VGA_GREEN, VGA_BLACK);
            term_printf("[ OK ] ");
            term_set_color(VGA_LTGRAY, VGA_BLACK);
            term_printf("%s\n", buf);
        }
    }

    /* Wall-clock + scheduler.  Both are pure software on top of CMOS
     * and the PIT - no extra IRQs are wired, so this is safe to run
     * after `sti` but before the WM brings up its render loop. */
    rtc_init();
    boot_ok ("RTC: CMOS wall clock attached");
    tasktimer_init();
    boot_ok ("Tasktimer: real-time scheduler armed");

    boot_log("Mouse: initialising PS/2 auxiliary device (IRQ12) ...");
    mouse_init();
    boot_ok ("Mouse: 3-byte packets, IRQ12 attached");

    boot_log("WM: bringing up the window manager + 6 MiB framebuffer pool ...");
    wm_init();

    /* Desktop environment: wallpaper, panel (taskbar), start menu, icons.
     * Must be initialised BEFORE the first wm_present() because the WM
     * delegates wallpaper drawing to desktop_draw_background().  No icons
     * are registered here - the shell wires up `addicon` for users.    */
    desktop_init();
    boot_ok ("Desktop: panel + icon table ready");

    /* Console window: 600x400 (Task 2 spec, down from the original
     * 900x600).  Content area = 596 x 380 pixels = 74 cols x 47 rows
     * at 8x8 font.  Plenty for an interactive shell while leaving the
     * majority of the desktop free for user icons + secondary windows. */
    window_t *console = wm_create_window(60, 80, 600, 400, "NexxoN Shell");
    if (!console) panic("WM: failed to create the console window");
    wm_set_focus(console);
    wm_set_shell_window(console);   /* enables focus-gated Ctrl+C abort */

    /* Keep the boot log on the physical splash screen until the late GUI
     * hand-off below.  Earlier builds retargeted the terminal to this
     * off-screen console immediately; if a later real-hardware driver probe
     * (NIC/USB/audio/etc.) stalled, the user only saw the previous splash
     * line and it looked like the desktop/window code had frozen.  Deferring
     * the retarget keeps every remaining boot milestone visible on bare
     * metal and makes the actual blocking subsystem obvious. */
    boot_ok ("WM: console window 0 ready - terminal hand-off deferred");

    /* Restore desktop icons saved on a previous boot (BUG 3).  NXFS is
     * up by now so /sys/desktop.cfg, if present, can be re-read and the
     * icon table re-populated before the user ever sees the desktop. */
    {
        int restored = desktop_load_icons();
        if (restored > 0) {
            boot_ok ("Desktop: restored saved icons from /sys/desktop.cfg");
            term_set_color(VGA_CYAN, VGA_BLACK);
            term_printf("       %d icon%s restored.\n",
                        restored, restored == 1 ? "" : "s");
            term_set_color(VGA_LTGRAY, VGA_BLACK);
        } else {
            /* Fresh / empty desktop: seed a few default shortcuts. */
            desktop_seed_defaults();
        }
    }

    /* TASK 8: wallpaper.bmp -> desktop background.  Optional; absence is
     * just logged and the default dotted pattern stays. */
    if (desktop_load_wallpaper()) {
        boot_ok ("Desktop: /wallpaper.bmp loaded as background");
    }

    /* TASK 28: universal NIC dispatcher.  Register every known driver
     * and let netif_autodetect() pick the first one that probes.  This
     * way QEMU's E1000 and VirtualBox's PCnet-FAST III both light up
     * the same IPv4/ICMP/TCP stack without code changes. */
    netif_register(&e1000_driver);
    netif_register(&pcnet_driver);
    /* Realtek RTL8169 / RTL8111 / RTL8168 family - common on consumer
     * desktops and mini-PCs.  Registered last so e1000 still wins on
     * QEMU's emulated NIC; on real hardware netif_autodetect probes
     * each driver in registration order and bind to the first
     * present + init-OK match. */
    netif_register(&rtl_driver);
    if (netif_autodetect()) {
        net_init();
        boot_ok ("Network: NIC dispatcher bound, IPv4/ICMP/TCP stack ready");
        term_set_color(VGA_CYAN, VGA_BLACK);
        term_printf("       interface = %s\n", netif_name());
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    } else {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("[NOTE] no supported NIC found - network disabled.\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    }

    /* TASK 3/4/26: full USB host stack bring-up.  Discovery + UHCI/EHCI
     * QH/qTD scheduling + enumeration + class drivers (Mass Storage).
     * The PnP daemon installs every detected USB partition under /usbN
     * via the VFS mount table, ready for the file manager to browse. */
    vfs_init();
    boot_log("USB: probing host controllers (firmware-legacy aware) ...");
    int nusb = usb_init();
    bool usb_auto_takeover = false;
    if (nusb > 0) {
        /* usb_init() decides the bare-metal-safe policy itself: if firmware
         * SMM still owns a controller for USB-legacy PS/2 emulation it leaves
         * the controllers untouched (keyboard/mouse keep working via the PS/2
         * driver); otherwise it does a clean ownership hand-off + native
         * bring-up.  Either way we never half-reset input into a dead state. */
        if (usb_legacy_preserved()) {
            boot_ok ("USB: firmware legacy input preserved (controllers not reset)");
            /* Issue #1: automatically do what the `usbnative` command does, so
             * the USB mouse (and keyboard) work natively without the user
             * typing it every boot — but FAIL-SAFE.  A non-destructive
             * pre-flight must first confirm the safe profile (an add-in xHCI
             * carrying an FS/LS HID); only then do we hand that one controller
             * off from SMM, leaving the chipset USB — boot pendrive + the SMM
             * keyboard it emulates — completely untouched.  `nousbnative` on
             * the kernel cmdline (GRUB "safe mode") skips it entirely. */
            if (boot_info_cmdline_has("nousbnative")) {
                term_set_color(VGA_YELLOW, VGA_BLACK);
                term_printf("       USB: native takeover disabled by `nousbnative` (safe mode).\n");
                term_set_color(VGA_LTGRAY, VGA_BLACK);
            } else {
                char why[140];
                int adv = usb_native_takeover_advisable(why, sizeof(why));
                term_set_color(VGA_CYAN, VGA_BLACK);
                term_printf("       USB: %s\n", why);
                term_set_color(VGA_LTGRAY, VGA_BLACK);
                if (adv > 0) {
                    term_printf("       USB: taking the add-in controller over from "
                                "BIOS SMM (native mouse + keyboard) ...\n");
                    (void)usb_force_native();
                    usb_auto_takeover = true;
                } else {
                    term_printf("       USB: auto-takeover skipped; `usbnative` "
                                "forces it from the shell if needed.\n");
                }
            }
        } else {
            boot_ok ("USB: host controllers reset, native stack online");
        }
    } else {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("[NOTE] no UHCI/EHCI USB host controller initialised; BIOS legacy input remains active.\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
    }
    pnp_init();

    /* Verify the boot-time native takeover actually produced working input.
     * HID drivers bind in pnp_init() (above), so the counts are meaningful only
     * now.  The chipset USB was never touched, so the boot device is safe
     * regardless; this only surfaces whether native keyboard/mouse came up so a
     * bad case is visible on screen instead of a silently dead pointer. */
    if (usb_auto_takeover) {
        int kb = 0, ms = 0;
        usbhid_counts(&kb, &ms);
        if (kb + ms > 0) {
            boot_ok ("USB: native takeover complete — HID online");
            term_set_color(VGA_CYAN, VGA_BLACK);
            term_printf("       USB: %d keyboard + %d mouse native; ports %s\n",
                        kb, ms, usb_takeover_diag());
            term_set_color(VGA_LTGRAY, VGA_BLACK);
        } else {
            term_set_color(VGA_YELLOW, VGA_BLACK);
            term_printf("[WARN] USB: native takeover enumerated no working HID.\n");
            term_printf("       If input is dead, reboot and choose GRUB \"safe mode\"\n");
            term_printf("       (no USB takeover).  ports %s\n", usb_takeover_diag());
            term_set_color(VGA_LTGRAY, VGA_BLACK);
        }
    }

    /* Unified driver framework: enumerate every bus (PCI + USB + platform),
     * match the driver registry, and record bindings.  Runs AFTER the tuned
     * boot-order init above, so it inventories what is already up and flags any
     * unknown hardware.  See docs/DRIVER_FRAMEWORK.md. */
    {
        extern void drvmgr_init(void);
        drvmgr_init();
    }
    /* TASK Cookies: session store before any HTTP/HTTPS fetch. */
    {
        extern void cookie_init(void);
        cookie_init();
    }
    /* TASK Notify: toast center. */
    {
        extern void notify_init(void);
        notify_init();
    }
    /* TASK Vault: credential store (locked until login). */
    {
        extern void vault_init(void);
        vault_init();
    }
    /* TASK VMM: per-process page-table foundation + hardware paging. */
    {
        extern bool vmm_init(void);
        extern void vmm_enable_paging(void);
        vmm_init();
        vmm_enable_paging();
        boot_ok ("VMM: per-process page directories ready, paging enabled (PSE 4 GiB)");
        /* PAT write-combining for the LFB MUST run after paging is live (it
         * rewrites the 4 MiB framebuffer PDEs + flushes the TLB).  The earlier
         * MTRR setup loses to a BIOS UC range that overlaps the LFB; PAT WC
         * over a UC MTRR resolves to WC (SDM Vol.3 Table 11-7), which is what
         * actually makes blits fast on boards like the ASUS P8Z77. */
        {
            extern bool vmm_set_pat_write_combining(uint32_t, uint32_t);
            uint32_t fb_phys = (uint32_t)(uintptr_t)vga_framebuffer();
            uint32_t fb_size = 1920u * 1080u * 4u;
            if (vmm_set_pat_write_combining(fb_phys, fb_size))
                boot_ok("PAT: LFB forced to Write-Combining (overrides MTRR)");
        }
    }
    /* TASK SCHED: preemptive round-robin scheduler (IRQ0, 100 Hz PIT). */
    {
        extern bool sched_init(void);
        if (sched_init()) {
            boot_ok ("Scheduler: preemptive round-robin armed (40 ms quantum)");
        }
        /* Spawn a heartbeat background task to verify ctx_switch works. */
        {
            extern void *sched_spawn(const char *, void (*)(uint32_t), uint32_t, void *);
            extern void heartbeat_task(uint32_t);
            sched_spawn("heartbeat", heartbeat_task, 0, NULL);
        }
    }
    if (vfs_mount_count() > 0) {
        char line[80];
        for (int i = 0; i < vfs_mount_count(); i++) {
            if (pnp_mount_describe(i, line, sizeof(line)) > 0) {
                term_set_color(VGA_CYAN, VGA_BLACK);
                term_printf("       %s\n", line);
                term_set_color(VGA_LTGRAY, VGA_BLACK);
            }
        }
        boot_ok ("USB: plug-and-play daemon mounted detected USB volumes");
    }

    /* TASK 22: PCI audio bring-up (AC'97 / HDA / PC speaker fallback). */
    {
        extern bool audio_init(void);
        if (audio_init()) {
            boot_ok ("Audio: PCI codec detected and brought online");
        }
    }

    /* TASK 30: localisation engine. */
    i18n_init();

    /* TASK 31: global theming engine. */
    theme_init();

    /* --------------------------------------------------------------------
     * Boot-log visibility hold.  On a fast machine the splash + boot log can
     * flash past before the monitor has even re-synced after GRUB's video
     * mode switch, so the user never sees what happened during startup.  Hold
     * the completed boot log on screen here (the whole log is still visible —
     * login/desktop have not painted yet), skippable by any key.  The default
     * is brief; the GRUB "verbose boot" entry passes `bootpause` for a long
     * hold when something needs studying.  Diagnostic text, English to match
     * the rest of the boot log. */
    {
        bool     verbose = boot_info_cmdline_has("bootpause");
        uint32_t secs    = verbose ? 30u : 5u;
        term_set_color(VGA_CYAN, VGA_BLACK);
        term_printf("\n[ boot complete ] holding the boot log for %u s — "
                    "press any key to continue", secs);
        if (!verbose)
            term_printf(" (GRUB \"verbose boot\" holds it longer)");
        term_printf(" ...\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        keyboard_drain();                      /* ignore keys buffered so far */
        uint32_t t0 = pit_ms();
        while ((pit_ms() - t0) < secs * 1000u) {
            if (keyboard_has_data()) { (void)keyboard_getc(); break; }
            pit_sleep(20);
        }
    }

    /* TASK 31: authentication store + graphical login.
     *
     * GUI overhaul: historically wm_present() ran BEFORE login_show(),
     * which let the desktop + taskbar + shell window flash on screen
     * for a brief second before the modal login overlaid them.  We now
     *   1. load persisted settings from /sys/gephaz.cfg so the
     *      sign-in panel renders in the user's saved language,
     *   2. call login_show() which paints its own modal screen
     *      directly via gfx_screen() — the WM never composites a
     *      pre-auth frame,
     *   3. only after successful auth do we wm_present() to commit
     *      the desktop, and play the boot jingle. */
    auth_init();
    gephaz_load_settings();
    /* Live CD (#7): a freshly-booted live image has no real owner, so we
     * don't gate it behind a credentials prompt -- sign in automatically as
     * the default admin account and drop straight to the desktop.  An
     * INSTALLED system still shows the graphical login, using the account
     * that was created during installation (#8). */
    if (nxfs_mode() == NXFS_MODE_LIVE) {
        auth_set_current_user("admin");
        {
            extern int vault_unlock(const char *p);
            vault_unlock("admin");
        }
        debug_ok("login: live mode - auto signed in as 'admin' (no prompt)");
    } else {
        login_show();
    }

    /* Late terminal hand-off: all boot diagnostics above stayed visible on
     * the splash screen; from this point on the interactive shell owns the
     * console window.  Install the resize hook at the same time so any future
     * content-buffer rebuild preserves the terminal scrollback. */
    term_set_target(&console->content);
    term_set_dirty_cb(wm_mark_dirty);
    /* Finer-grained: the 12.5 Hz idle spinner damages only its glyph cell,
     * so it no longer forces a full-screen recompose every 80 ms. */
    term_set_region_dirty_cb(wm_shell_content_damage);
    wm_set_resize_cb(console, shell_window_resize_cb);
    boot_ok ("WM: shell terminal attached to console window");

    /* Initial compose now that the user is signed in.  This happens BEFORE
     * the boot jingle so the desktop appears the instant login completes
     * rather than after ~360 ms of blocking tones — the jingle then plays
     * over the already-visible desktop instead of widening the gap between
     * the last boot-log line and the desktop. */
    wm_present();

    /* Boot jingle plays just after the desktop is committed so the user
     * associates it with the desktop appearing. */
    speaker_boot_jingle();

    /* NXScript engine: initialise the global symbol table. */
    nxscript_init();
    debug_ok("nxscript: interpreter ready");

    debug_ok("kernel ready, entering shell");

#if defined(__x86_64__)
    /* Diagnostic forced path (64-bit only): with `tramptest` on the cmdline,
     * exercise the long-mode VBE trampoline before the shell - switch to
     * 800x600 then back to 1024x768 (two real-mode INT 10h round-trips out of
     * and back into long mode).  Pair with `forcetramp` to force the trampoline
     * over the BGA path (on real bare metal there is no BGA, so the trampoline
     * runs regardless).  COM1 reports both legs; the desktop survives either
     * way (a bad trampoline can only fail the switch, not the boot).  Handy for
     * validating runtime resolution switching on a real machine. */
    if (boot_info_cmdline_has("tramptest")) {
        extern bool vga_switch_mode(uint16_t, uint16_t);
        debug_printf("[tramptest] VBE trampoline round-trip 800x600 -> 1024x768 ...\n");
        bool a = vga_switch_mode(800, 600);
        bool b = vga_switch_mode(1024, 768);
        debug_printf("[tramptest] 800x600=%d 1024x768=%d ; back in long mode, alive\n",
                     a, b);
    }
#endif

    /* --------------------------------------------------------------------
     * Stage 8: shell.  Never returns.
     * -------------------------------------------------------------------- */
    shell_run();

    panic("shell_run() returned - this should be unreachable");
}
