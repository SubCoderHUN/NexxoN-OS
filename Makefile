# ============================================================================
# NexxoN OS - Build System  (x86_64 long mode)
# ============================================================================
# Targets:
#   make            -> Build the bootable .img (64-bit kernel ELF + GRUB image)
#   make kernel     -> Build only the kernel ELF
#   make iso        -> Build only the bootable image
#   make disk       -> (optional) Build an empty NXFS data disk for QEMU testing
#   make proxy      -> (optional) Package the Node.js/Puppeteer web-rendering proxy
#   make run        -> Run NexxoN OS in QEMU (persistent AHCI/SATA disk + AC97 + net)
#   make run-debug  -> Run in QEMU with serial-to-stdio for boot debug output
#   make clean      -> Remove all build artifacts
#
# The kernel is a single elf64 image: GRUB (Multiboot v1) loads it and enters
# 32-bit protected mode at _start (src/boot/boot.asm), which builds 4-level
# page tables, switches the CPU into IA-32e long mode and calls kernel_main.
# Persistent storage lives on the AHCI/SATA disk the installer writes to
# (in QEMU, `make run` attaches one automatically and creates it on demand).
#
# Tooling:
#   The kernel is built with the host's GCC in -m64 -ffreestanding mode, which
#   is enough for a v1-Multiboot elf64 kernel.  A full x86_64-elf-gcc cross-
#   toolchain is also supported - set CROSS=x86_64-elf- on the command line.
# ============================================================================

# ---------- Toolchain ------------------------------------------------------
CROSS    ?=
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
AS       := nasm
OBJCOPY  := $(CROSS)objcopy
READELF  := $(CROSS)readelf
MUSL_CC  ?= musl-gcc
MUSL_RT  ?= /lib/x86_64-linux-musl/libc.so

# ---------- Flags ----------------------------------------------------------
# Freestanding: no hosted libc / no standard startup files.
# -fno-pic / -fno-pie: the kernel is linked at a fixed address, never PIE.
# -fno-stack-protector: the stack-cookie machinery requires libc / TLS.
# -mno-red-zone: MANDATORY for kernel/interrupt code (the SysV red zone is
#   clobbered by interrupt frames).  -mcmodel=large: no 2 GiB symbol-range
#   assumptions.  SSE is left ENABLED (x86_64 baseline; the SysV ABI returns
#   float/double in XMM) - boot.asm turns on CR0.MP/CR4.OSFXSR before any C runs.
# -nostdinc + -isystem <gcc include>: use only GCC's freestanding headers
#   (stdarg.h, stddef.h, stdint.h), never the host's /usr/include.
GCC_INCDIR := $(shell $(CC) -m64 -print-file-name=include)

CFLAGS  := -m64 -std=gnu11 \
           -ffreestanding -nostdlib -nostdinc \
           -isystem $(GCC_INCDIR) \
           -fno-pic -fno-pie -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
           -mno-red-zone -mcmodel=large \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -Wno-address-of-packed-member \
           -O2 -g \
           -Isrc/include

LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -z noexecstack

ASFLAGS := -f elf64 -g -F dwarf

# ---------- Project layout ------------------------------------------------
SRC_DIR    := src
BUILD_DIR  := build
ISO_DIR    := iso
PROXY_DIR  := proxy
PROXY_OUT  := $(BUILD_DIR)/proxy
KERNEL_ELF := $(BUILD_DIR)/nexxon.elf
INSTALL_ELF := $(BUILD_DIR)/nexxon-install.elf
IMAGE      := nexxon-os.img
DATA_DISK  := nxfs-disk.img
DATA_SIZE_MB := 64

# Collect every C and ASM source under src/.  stage1 / stage2 are excluded
# here because they build as FLAT binaries with hard ORG addresses (0x7C00 /
# 0x8000) and must NOT be linked into the kernel ELF - they get their own
# recipe + objcopy wrapper below (installsys writes them to LBA 0 / LBA 1).
C_SRCS  := $(shell find $(SRC_DIR) -type f -name '*.c')
S_SRCS  := $(shell find $(SRC_DIR) -type f -name '*.asm' \
                       -not -name 'stage1.asm' -not -name 'stage2.asm')

# Map sources to object paths under build/
C_OBJS  := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
S_OBJS  := $(patsubst $(SRC_DIR)/%.asm,$(BUILD_DIR)/%.o,$(S_SRCS))

# ---------- Embedded boot binaries ----------------------------------------
# Stage-1 (MBR) and stage-2 are assembled to flat binaries (nasm -f bin), then
# wrapped into elf64 object files via objcopy --binary so the kernel can
# reference them as `extern uint8_t _binary_build_boot_stageX_bin_start[]`.
# This is how installsys gets the bytes it needs to write to LBA 0 and LBA 1.
# stage2 is assembled with -DELF64 so the INSTALLED system's stage-2 loader
# parses the elf64 kernel image off the SATA disk.
STAGE1_BIN := $(BUILD_DIR)/boot/stage1.bin
STAGE2_BIN := $(BUILD_DIR)/boot/stage2.bin
STAGE1_OBJ := $(BUILD_DIR)/boot/stage1.o
STAGE2_OBJ := $(BUILD_DIR)/boot/stage2.o
EMBED_OBJS := $(STAGE1_OBJ) $(STAGE2_OBJ)

# ---------- Embedded DOOM shareware IWAD -----------------------------------
# doom1.wad (freely distributable shareware episode) rides in .rodata via the
# same objcopy wrapping as the boot stages; the DOOM app reads it through an
# in-memory file-IO shim (_binary_assets_doom1_wad_start/_end).
DOOM_WAD     := assets/doom1.wad
DOOM_WAD_OBJ := $(BUILD_DIR)/assets/doom1wad.o

# ---------- Embedded demo Linux ELF ---------------------------------------
# A static-PIE x86_64 Linux binary linked against musl, embedded in .rodata so
# the compatibility layer has a deterministic libc-level integration test.
LINUXDEMO_SRC := userland/linuxdemo.c
LINUXDEMO_ELF := $(BUILD_DIR)/userland/linuxdemo.elf
LINUXDEMO_OBJ := $(BUILD_DIR)/userland/linuxdemo.o
LINUXEXEC_SRC := userland/linuxexec.c
LINUXEXEC_ELF := $(BUILD_DIR)/userland/linuxexec.elf
LINUXEXEC_OBJ := $(BUILD_DIR)/userland/linuxexec.o
LINUXDYN_SRC  := userland/linuxdyn.c
LINUXDYN_ELF  := $(BUILD_DIR)/userland/linuxdyn.elf
LINUXDYN_OBJ  := $(BUILD_DIR)/userland/linuxdyn.o
LINUXSH_SRC   := userland/linuxsh.c
LINUXSH_ELF   := $(BUILD_DIR)/userland/linuxsh.elf
LINUXSH_OBJ   := $(BUILD_DIR)/userland/linuxsh.o
LINUXNET_SRC  := userland/linuxnet.c
LINUXNET_ELF  := $(BUILD_DIR)/userland/linuxnet.elf
LINUXNET_OBJ  := $(BUILD_DIR)/userland/linuxnet.o
LINUXPTHREAD_SRC  := userland/linuxpthread.c
LINUXPTHREAD_ELF  := $(BUILD_DIR)/userland/linuxpthread.elf
LINUXPTHREAD_OBJ  := $(BUILD_DIR)/userland/linuxpthread.o
LINUXEPOLL_SRC    := userland/linuxepoll.c
LINUXEPOLL_ELF    := $(BUILD_DIR)/userland/linuxepoll.elf
LINUXEPOLL_OBJ    := $(BUILD_DIR)/userland/linuxepoll.o
LINUXGLIBC_SRC    := userland/linuxglibc.c
LINUXGLIBC_ELF    := $(BUILD_DIR)/userland/linuxglibc.elf
LINUXGLIBC_OBJ    := $(BUILD_DIR)/userland/linuxglibc.o
LINUXDRI_SRC      := userland/linuxdri.c
LINUXDRI_ELF      := $(BUILD_DIR)/userland/linuxdri.elf
LINUXDRI_OBJ      := $(BUILD_DIR)/userland/linuxdri.o
LINUXMMAP_SRC     := userland/linuxmmap.c
LINUXMMAP_ELF     := $(BUILD_DIR)/userland/linuxmmap.elf
LINUXMMAP_OBJ     := $(BUILD_DIR)/userland/linuxmmap.o
LINUXDRMFB_SRC    := userland/linuxdrmfb.c
LINUXDRMFB_ELF    := $(BUILD_DIR)/userland/linuxdrmfb.elf
LINUXDRMFB_OBJ    := $(BUILD_DIR)/userland/linuxdrmfb.o
LINUXALSA_SRC     := userland/linuxalsa.c
LINUXALSA_ELF     := $(BUILD_DIR)/userland/linuxalsa.elf
LINUXALSA_OBJ     := $(BUILD_DIR)/userland/linuxalsa.o
LINUXX11_SRC      := userland/linuxx11.c
LINUXX11_ELF      := $(BUILD_DIR)/userland/linuxx11.elf
LINUXX11_OBJ      := $(BUILD_DIR)/userland/linuxx11.o
LINUXXLIB_SRC     := userland/linuxxlib.c
LINUXXLIB_ELF     := $(BUILD_DIR)/userland/linuxxlib.elf
LINUXXLIB_OBJ     := $(BUILD_DIR)/userland/linuxxlib.o
LINUXXEVENT_SRC   := userland/linuxxevent.c
LINUXXEVENT_ELF   := $(BUILD_DIR)/userland/linuxxevent.elf
LINUXXEVENT_OBJ   := $(BUILD_DIR)/userland/linuxxevent.o
LINUXVULKAN_SRC   := userland/linuxvulkan.c
LINUXVULKAN_ELF   := $(BUILD_DIR)/userland/linuxvulkan.elf
LINUXVULKAN_OBJ   := $(BUILD_DIR)/userland/linuxvulkan.o
LINUXVULKANSO_SRC := userland/linuxvulkanso.c
LINUXVULKANSO_ELF := $(BUILD_DIR)/userland/linuxvulkanso.elf
LINUXVULKANSO_OBJ := $(BUILD_DIR)/userland/linuxvulkanso.o
LINUXVKLOADER_SRC := userland/linuxvkloader.c
LINUXVKLOADER_ELF := $(BUILD_DIR)/userland/linuxvkloader.elf
LINUXVKLOADER_OBJ := $(BUILD_DIR)/userland/linuxvkloader.o
LINUXVKINSTANCE_SRC := userland/linuxvkinstance.c
LINUXVKINSTANCE_ELF := $(BUILD_DIR)/userland/linuxvkinstance.elf
LINUXVKINSTANCE_OBJ := $(BUILD_DIR)/userland/linuxvkinstance.o
LINUXVKQUEUE_SRC := userland/linuxvkqueue.c
LINUXVKQUEUE_ELF := $(BUILD_DIR)/userland/linuxvkqueue.elf
LINUXVKQUEUE_OBJ := $(BUILD_DIR)/userland/linuxvkqueue.o
LINUXVKCOMMAND_SRC := userland/linuxvkcommand.c
LINUXVKCOMMAND_ELF := $(BUILD_DIR)/userland/linuxvkcommand.elf
LINUXVKCOMMAND_OBJ := $(BUILD_DIR)/userland/linuxvkcommand.o
LINUXVKSURFACE_SRC := userland/linuxvksurface.c
LINUXVKSURFACE_ELF := $(BUILD_DIR)/userland/linuxvksurface.elf
LINUXVKSURFACE_OBJ := $(BUILD_DIR)/userland/linuxvksurface.o
LINUXVKSWAPCHAIN_SRC := userland/linuxvkswapchain.c
LINUXVKSWAPCHAIN_ELF := $(BUILD_DIR)/userland/linuxvkswapchain.elf
LINUXVKSWAPCHAIN_OBJ := $(BUILD_DIR)/userland/linuxvkswapchain.o
VULKAN_STUB_SRC   := userland/vulkan_nexxon_stub.c
VULKAN_STUB_SO    := $(BUILD_DIR)/userland/libvulkan_nexxon.so
VULKAN_STUB_OBJ   := $(BUILD_DIR)/userland/libvulkan_nexxon_so.o
LINUXPULSE_SRC    := userland/linuxpulse.c
LINUXPULSE_ELF    := $(BUILD_DIR)/userland/linuxpulse.elf
LINUXPULSE_OBJ    := $(BUILD_DIR)/userland/linuxpulse.o
LINUX32_SRC       := userland/linux32.asm
LINUX32_ELF32_OBJ := $(BUILD_DIR)/userland/linux32-elf32.o
LINUX32_ELF       := $(BUILD_DIR)/userland/linux32.elf
LINUX32_OBJ       := $(BUILD_DIR)/userland/linux32.o
LINUX32GLIBC_SRC  := userland/linux32glibc.c
LINUX32GLIBC_ELF  := $(BUILD_DIR)/userland/linux32glibc.elf
LINUX32GLIBC_OBJ  := $(BUILD_DIR)/userland/linux32glibc.o
LINUX32PIE_SRC    := userland/linux32pie.c
LINUX32PIE_ELF    := $(BUILD_DIR)/userland/linux32pie.elf
LINUX32PIE_OBJ    := $(BUILD_DIR)/userland/linux32pie.o
LINUX32PTHREAD_SRC := userland/linux32pthread.c
LINUX32PTHREAD_ELF := $(BUILD_DIR)/userland/linux32pthread.elf
LINUX32PTHREAD_OBJ := $(BUILD_DIR)/userland/linux32pthread.o
STEAMWEBSTUB_SRC   := userland/steamwebstub.c
STEAMWEBSTUB_ELF   := $(BUILD_DIR)/userland/steamwebstub.elf
WINSTUB_SRC   := userland/winestub.c
WINSTUB_ELF   := $(BUILD_DIR)/userland/winestub.elf
WINSTUB_OBJ   := $(BUILD_DIR)/userland/winestub.o
MUSL_RT_COPY := $(BUILD_DIR)/userland/musl-libc.so
MUSL_RT_OBJ   := $(BUILD_DIR)/userland/musl-libc.o
HELLO_PE      := tools/hello_pe.bin
HELLO_PE_OBJ  := $(BUILD_DIR)/tools/hello_pe.o

OBJS    := $(S_OBJS) $(C_OBJS) $(EMBED_OBJS) $(DOOM_WAD_OBJ) \
           $(LINUXDEMO_OBJ) $(LINUXEXEC_OBJ) $(LINUXDYN_OBJ) $(LINUXSH_OBJ) \
           $(LINUXNET_OBJ) $(LINUXPTHREAD_OBJ) $(LINUXEPOLL_OBJ) \
           $(LINUXGLIBC_OBJ) $(LINUXDRI_OBJ) $(LINUXMMAP_OBJ) $(LINUXDRMFB_OBJ) $(LINUXALSA_OBJ) $(LINUXX11_OBJ) $(LINUXXLIB_OBJ) $(LINUXXEVENT_OBJ) $(LINUXVULKAN_OBJ) $(LINUXVULKANSO_OBJ) $(LINUXVKLOADER_OBJ) $(LINUXVKINSTANCE_OBJ) $(LINUXVKQUEUE_OBJ) $(LINUXVKCOMMAND_OBJ) $(LINUXVKSURFACE_OBJ) $(LINUXVKSWAPCHAIN_OBJ) $(VULKAN_STUB_OBJ) $(LINUXPULSE_OBJ) $(LINUX32_OBJ) $(LINUX32GLIBC_OBJ) $(LINUX32PIE_OBJ) $(LINUX32PTHREAD_OBJ) $(BUILD_DIR)/userland/steamwebstub.o $(WINSTUB_OBJ) $(MUSL_RT_OBJ) $(HELLO_PE_OBJ)

# ---------- Phony targets --------------------------------------------------
.PHONY: all kernel iso disk proxy run run-debug clean

all: $(IMAGE)

kernel: $(KERNEL_ELF)

iso: $(IMAGE)

disk: $(DATA_DISK)

# ---------- Object compilation --------------------------------------------
# Patterns reproduce the source tree under build/ so file paths stay readable.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(AS) $(ASFLAGS) $< -o $@

# The PureDOOM amalgamation is upstream id-Software code — silence warnings
# for just this translation unit instead of patching 40k foreign lines.
$(BUILD_DIR)/apps/doom/doomglue.o: CFLAGS += -w

$(DOOM_WAD_OBJ): $(DOOM_WAD)
	@mkdir -p $(dir $@)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(HELLO_PE): tools/build_hello_pe.py
	@echo "  PE    $@"
	@python3 tools/build_hello_pe.py

$(HELLO_PE_OBJ): $(HELLO_PE)
	@mkdir -p $(dir $@)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

# Build the demo Linux ELF with musl libc as a static PIE.  This is intentionally
# part of the normal image build: a successful image always contains the exact
# class of Linux program this compatibility milestone claims to support.
$(LINUXDEMO_ELF): $(LINUXDEMO_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (static-pie Linux ELF)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXDEMO_OBJ): $(LINUXDEMO_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

# Fixed-address static ET_EXEC used to verify private-CR3 loading and
# MAP_FIXED.  0x20000000 is above NexxoN's current low kernel/BSS window.
$(LINUXEXEC_ELF): $(LINUXEXEC_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (static ET_EXEC @0x20000000)"
	@$(MUSL_CC) -m64 -static -fno-pie -no-pie -fno-stack-protector \
	     -Wl,-Ttext-segment=0x20000000,-z,relro,-z,now -O2 -o $@ $<
	@$(READELF) -h $@ | awk '$$1 == "Type:" { seen=1; if ($$2 != "EXEC") exit 1 } \
	                         END { if (!seen) exit 1 }' \
	  || (echo "  FAIL  $@ is not ET_EXEC" && exit 1)
	@echo "  OK    ET_EXEC type verified"

$(LINUXEXEC_OBJ): $(LINUXEXEC_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

# Dynamically linked musl probe (PT_INTERP + libc.so) for the v4 milestone.
$(LINUXDYN_ELF): $(LINUXDYN_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (dynamic Linux ELF)"
	@$(MUSL_CC) -m64 -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<
	@$(READELF) -l $@ | grep -q 'INTERP' \
	  || (echo "  FAIL  $@ has no PT_INTERP" && exit 1)
	@echo "  OK    PT_INTERP verified"

$(LINUXDYN_OBJ): $(LINUXDYN_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXSH_ELF): $(LINUXSH_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Linux subsystem shell)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXSH_OBJ): $(LINUXSH_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXNET_ELF): $(LINUXNET_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Linux socket probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXNET_OBJ): $(LINUXNET_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXPTHREAD_ELF): $(LINUXPTHREAD_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Linux pthread probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXPTHREAD_OBJ): $(LINUXPTHREAD_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXEPOLL_ELF): $(LINUXEPOLL_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Linux epoll probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXEPOLL_OBJ): $(LINUXEPOLL_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXGLIBC_ELF): $(LINUXGLIBC_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (glibc dynamic probe)"
	@gcc -m64 -O2 -o $@ $< \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXGLIBC_OBJ): $(LINUXGLIBC_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXDRI_ELF): $(LINUXDRI_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (DRM /dev/dri probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXDRI_OBJ): $(LINUXDRI_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXMMAP_ELF): $(LINUXMMAP_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (large mmap probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXMMAP_OBJ): $(LINUXMMAP_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXDRMFB_ELF): $(LINUXDRMFB_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (DRM dumb buffer probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXDRMFB_OBJ): $(LINUXDRMFB_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXALSA_ELF): $(LINUXALSA_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (ALSA PCM probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXALSA_OBJ): $(LINUXALSA_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXX11_ELF): $(LINUXX11_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (X11 display probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXX11_OBJ): $(LINUXX11_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXXLIB_ELF): $(LINUXXLIB_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (glibc libX11 lifecycle probe)"
	@gcc -m64 -O2 -o $@ $< -lX11 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXXLIB_OBJ): $(LINUXXLIB_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXXEVENT_ELF): $(LINUXXEVENT_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (glibc libX11 event-loop probe)"
	@gcc -m64 -O2 -o $@ $< -lX11 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXXEVENT_OBJ): $(LINUXXEVENT_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVULKAN_ELF): $(LINUXVULKAN_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Vulkan ICD probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXVULKAN_OBJ): $(LINUXVULKAN_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(VULKAN_STUB_SO): $(VULKAN_STUB_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (freestanding Vulkan ICD stub .so)"
	@gcc -m64 -shared -fPIC -nostdlib -fno-builtin \
	     -fno-stack-protector -Wl,-Bsymbolic -O2 -o $@ $<

$(VULKAN_STUB_OBJ): $(VULKAN_STUB_SO)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVULKANSO_ELF): $(LINUXVULKANSO_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Vulkan ICD dlopen probe)"
	@$(MUSL_CC) -m64 -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $< -ldl
	@$(READELF) -l $@ | grep -q 'INTERP' \
	  || (echo "  FAIL  $@ has no PT_INTERP" && exit 1)

$(LINUXVULKANSO_OBJ): $(LINUXVULKANSO_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVKLOADER_ELF): $(LINUXVKLOADER_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (real Vulkan loader probe)"
	@gcc -m64 -O2 -o $@ $< -L/lib/x86_64-linux-gnu -l:libvulkan.so.1 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXVKLOADER_OBJ): $(LINUXVKLOADER_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVKINSTANCE_ELF): $(LINUXVKINSTANCE_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (Vulkan instance/device enumeration probe)"
	@gcc -m64 -O2 -o $@ $< -L/lib/x86_64-linux-gnu -l:libvulkan.so.1 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXVKINSTANCE_OBJ): $(LINUXVKINSTANCE_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVKQUEUE_ELF): $(LINUXVKQUEUE_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (Vulkan logical-device queue probe)"
	@gcc -m64 -O2 -o $@ $< -L/lib/x86_64-linux-gnu -l:libvulkan.so.1 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXVKQUEUE_OBJ): $(LINUXVKQUEUE_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVKCOMMAND_ELF): $(LINUXVKCOMMAND_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (Vulkan memory/command submit probe)"
	@gcc -m64 -O2 -o $@ $< -L/lib/x86_64-linux-gnu -l:libvulkan.so.1 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXVKCOMMAND_OBJ): $(LINUXVKCOMMAND_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVKSURFACE_ELF): $(LINUXVKSURFACE_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (Xlib Vulkan surface probe)"
	@gcc -m64 -O2 -o $@ $< -lX11 \
	     -L/lib/x86_64-linux-gnu -l:libvulkan.so.1 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXVKSURFACE_OBJ): $(LINUXVKSURFACE_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXVKSWAPCHAIN_ELF): $(LINUXVKSWAPCHAIN_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC   $< (Xlib Vulkan swapchain probe)"
	@gcc -m64 -O2 -o $@ $< -lX11 \
	     -L/lib/x86_64-linux-gnu -l:libvulkan.so.1 \
	     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2

$(LINUXVKSWAPCHAIN_OBJ): $(LINUXVKSWAPCHAIN_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUXPULSE_ELF): $(LINUXPULSE_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (PulseAudio native probe)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(LINUXPULSE_OBJ): $(LINUXPULSE_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUX32_ELF32_OBJ): $(LINUX32_SRC)
	@mkdir -p $(dir $@)
	@echo "  AS32  $<"
	@$(AS) -f elf32 $< -o $@

$(LINUX32_ELF): $(LINUX32_ELF32_OBJ)
	@echo "  LD32  $< -> $@"
	@ld -m elf_i386 -nostdlib -e _start -Ttext 0x20001000 -o $@ $<

$(LINUX32_OBJ): $(LINUX32_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUX32GLIBC_ELF): $(LINUX32GLIBC_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC32 $< (dynamic i386 glibc probe)"
	@gcc -m32 -no-pie -O2 -o $@ $< \
	     -Wl,-Ttext-segment=0x20000000,--dynamic-linker=/lib/ld-linux.so.2

$(LINUX32GLIBC_OBJ): $(LINUX32GLIBC_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUX32PIE_ELF): $(LINUX32PIE_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC32 $< (dynamic i386 PIE probe)"
	@gcc -m32 -fPIE -pie -O2 -o $@ $< \
	     -Wl,--dynamic-linker=/lib/ld-linux.so.2

$(LINUX32PIE_OBJ): $(LINUX32PIE_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(LINUX32PTHREAD_ELF): $(LINUX32PTHREAD_SRC)
	@mkdir -p $(dir $@)
	@echo "  GCC32 $< (dynamic i386 pthread probe)"
	@gcc -m32 -fPIE -pie -O2 -pthread -o $@ $< \
	     -Wl,--dynamic-linker=/lib/ld-linux.so.2

$(LINUX32PTHREAD_OBJ): $(LINUX32PTHREAD_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(STEAMWEBSTUB_ELF): $(STEAMWEBSTUB_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (steamwebhelper stub)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(BUILD_DIR)/userland/steamwebstub.o: $(STEAMWEBSTUB_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(WINSTUB_ELF): $(WINSTUB_SRC)
	@mkdir -p $(dir $@)
	@echo "  MUSLCC $< (Wine PE stub launcher)"
	@$(MUSL_CC) -m64 -static-pie -fPIE -fno-stack-protector \
	     -Wl,-z,relro,-z,now -O2 -o $@ $<

$(WINSTUB_OBJ): $(WINSTUB_ELF)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

# musl libc.so doubles as ld-musl-x86_64.so.1 on the target /lib tree.
$(MUSL_RT_COPY): $(MUSL_RT)
	@test -f $(MUSL_RT) \
	  || (echo "  FAIL  musl runtime missing: $(MUSL_RT)" && exit 1)
	@mkdir -p $(dir $@)
	@cp $(MUSL_RT) $@

$(MUSL_RT_OBJ): $(MUSL_RT_COPY)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

# ---------- Stage-1 / stage-2 boot binaries -------------------------------
# Flat binaries; nasm -f bin emits raw bytes with no ELF headers.
$(STAGE1_BIN): $(SRC_DIR)/boot/stage1.asm
	@mkdir -p $(dir $@)
	@echo "  AS    $<  (flat binary)"
	@$(AS) -f bin $< -o $@
	@test $$(stat -c %s $@) -eq 512 \
	  || (echo "  FAIL  stage1.bin is $$(stat -c %s $@) bytes (must be exactly 512)" && exit 1)

$(STAGE2_BIN): $(SRC_DIR)/boot/stage2.asm
	@mkdir -p $(dir $@)
	@echo "  AS    $<  (flat binary, -DELF64)"
	@$(AS) -f bin -DELF64 $< -o $@
	@test $$(stat -c %s $@) -le 32256 \
	  || (echo "  FAIL  stage2.bin is $$(stat -c %s $@) bytes (cap is 63 sectors = 32256)" && exit 1)

# Wrap each flat binary into an elf64 object whose payload is the raw bytes.
# objcopy generates three symbols per .bin:
#     _binary_<path>_start, _binary_<path>_end, _binary_<path>_size
# The path is the input file path with non-alnum chars turned into '_', i.e.
# build/boot/stage1.bin -> _binary_build_boot_stage1_bin_*, which is exactly
# what src/kernel/boot_info.c declares.
$(STAGE1_OBJ): $(STAGE1_BIN)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

$(STAGE2_OBJ): $(STAGE2_BIN)
	@echo "  EMB   $< -> $@"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $< $@

# ---------- Linking -------------------------------------------------------
$(KERNEL_ELF): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "  OK    Kernel ELF built ($$( stat -c %s $@ ) bytes)"
	@grub-file --is-x86-multiboot $@ \
	  && echo "  OK    Multiboot v1 header verified" \
	  || (echo "  FAIL  Kernel is NOT Multiboot v1 compliant" && exit 1)

# Installed-disk stage2 has a fixed LBA64..16383 kernel area.  The normal ELF
# keeps DWARF for QEMU/debugging and can exceed that area even though debug
# sections are not loaded.  Strip only non-runtime debug data for the payload
# written by installsys; preserve the full ELF on the LIVE image.
$(INSTALL_ELF): $(KERNEL_ELF)
	@echo "  STRIP $< -> $@ (installed-system payload)"
	@$(OBJCOPY) --strip-debug $< $@
	@grub-file --is-x86-multiboot $@ \
	  || (echo "  FAIL  Installed payload lost Multiboot header" && exit 1)
	@test $$(stat -c %s $@) -lt $$(( (16384 - 64) * 512 )) \
	  || (echo "  FAIL  Installed payload exceeds LBA64..16383" && exit 1)
	@echo "  OK    Installed payload fits boot area ($$(stat -c %s $@) bytes)"

# ---------- Bootable image (hybrid ISO/USB) -------------------------------
# grub-mkrescue emits a hybrid ISO9660 image that boots from CDROM, USB stick
# (via dd) and IDE/SATA drives.  The .img extension keeps the user-facing
# artifact a single, ready-to-flash file.
#
# The `module` line passes a SECOND copy of the kernel as a multiboot module;
# the running kernel reads those bytes back via mbi->mods_addr and feeds them
# to installsys, which writes them to the SATA disk so the HDD boots stand-alone.
$(IMAGE): $(KERNEL_ELF) $(INSTALL_ELF) $(ISO_DIR)/boot/grub/grub.cfg
	@echo "  CP    kernel -> iso/boot/nexxon.elf"
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/nexxon.elf
	@echo "  CP    kernel -> iso/boot/nexxon.payload  (installsys source)"
	@cp $(INSTALL_ELF) $(ISO_DIR)/boot/nexxon.payload
	@echo "  IMG   grub-mkrescue (x86_64 long mode) -> $@"
	@grub-mkrescue --compress=xz -o $@ $(ISO_DIR) 2>/dev/null
	@echo "  OK    Bootable 64-bit image: $@ ($$( stat -c %s $@ ) bytes)"

# ---------- Empty NXFS data disk ------------------------------------------
# The secondary, writable disk where the OS keeps the on-disk NXFS filesystem.
# The first boot detects the empty superblock and formats it automatically.
$(DATA_DISK):
	@echo "  DD    creating empty $(DATA_SIZE_MB) MiB NXFS disk -> $@"
	@dd if=/dev/zero of=$@ bs=1M count=$(DATA_SIZE_MB) status=none
	@echo "  OK    Data disk: $@"

# ---------- Web-rendering proxy (Node.js + Puppeteer) --------------------
proxy: $(PROXY_OUT)/.stamp

$(PROXY_OUT)/.stamp: $(PROXY_DIR)/server.js $(PROXY_DIR)/package.json
	@mkdir -p $(PROXY_OUT)
	@echo "  PROXY  packaging $(PROXY_DIR)/ -> $(PROXY_OUT)/"
	@cp $(PROXY_DIR)/server.js $(PROXY_OUT)/
	@cp $(PROXY_DIR)/package.json $(PROXY_OUT)/
	@echo "  PROXY  installing dependencies (npm install) ..."
	@cd $(PROXY_OUT) && npm install
	@echo "  PROXY  ready - run: cd $(PROXY_OUT) && node server.js"
	@touch $@

# ---------- Run targets ---------------------------------------------------
# The bootable rescue image is exposed as a CDROM; the writable NXFS data disk
# is attached through an ICH9 AHCI controller (matching the kernel's AHCI/SATA
# driver).  On real hardware the equivalent is "boot from USB stick, write to
# the internal SATA disk".  The data disk is created on demand - NOT a build
# artifact.
QEMU_FLAGS := -m 256M \
              -cdrom $(IMAGE) \
              -drive id=nxfsdisk,file=$(DATA_DISK),format=raw,if=none \
              -device ich9-ahci,id=ahci \
              -device ide-hd,drive=nxfsdisk,bus=ahci.0 \
              -boot order=d \
              -vga std \
              -no-reboot \
              -netdev user,id=net0,hostfwd=tcp::9090-:9090 \
              -device e1000,netdev=net0 \
              -device AC97 \
              -audiodev sdl,id=audio0 \
              -machine pcspk-audiodev=audio0

run: $(IMAGE)
	@test -f $(DATA_DISK) || (echo "  DD    creating persistent $(DATA_SIZE_MB) MiB AHCI disk -> $(DATA_DISK)"; \
	    dd if=/dev/zero of=$(DATA_DISK) bs=1M count=$(DATA_SIZE_MB) status=none)
	qemu-system-x86_64 $(QEMU_FLAGS)

run-debug: $(IMAGE)
	@test -f $(DATA_DISK) || dd if=/dev/zero of=$(DATA_DISK) bs=1M count=$(DATA_SIZE_MB) status=none
	qemu-system-x86_64 $(QEMU_FLAGS) -serial stdio -d int,cpu_reset -no-shutdown

# ---------- Maintenance ---------------------------------------------------
clean:
	@echo "  RM    build artifacts"
	@rm -rf $(BUILD_DIR) $(IMAGE) $(DATA_DISK) \
	        $(ISO_DIR)/boot/nexxon.elf $(ISO_DIR)/boot/nexxon.payload $(PROXY_OUT)

# ---------- Auto-generated header dependencies -----------------------------
# Placed at the very end (AFTER the default `all` goal) so an included .d
# rule can never hijack the default target.  Generated by -MMD -MP, these
# make `make` recompile any .c whose included headers changed, so editing a
# struct/enum layout in a .h can never leave stale objects with the wrong
# field offsets or enum indices (the silent-corruption trap that previously
# required a manual `make clean` — see docs/NEXT_STEPS_BARE_METAL.md §4).
-include $(C_OBJS:.o=.d)
