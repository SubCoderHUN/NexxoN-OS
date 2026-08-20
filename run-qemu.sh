#!/usr/bin/env bash
# ============================================================================
# NexxoN OS - QEMU thin-client test launcher
# ============================================================================
# Starts QEMU with:
#   * Host network forwarding  (host:9090 -> guest:9090) so the browser
#     can reach the Node.js/Puppeteer proxy running on the dev machine.
#   * AC97 audio device + SDL audio backend for PCM playback.
#   * E1000 NIC (matches the OS driver).
#
# Prerequisites:
#   1. Build the OS:      make
#   2. Start the proxy:   cd build/proxy && npm install && node server.js
#   3. Run this script:   ./run-qemu.sh
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

QEMU=qemu-system-i386
IMAGE=nexxon-os.img
DATA_DISK=nxfs-disk.img

if [ ! -f "$IMAGE" ]; then
    echo "ERROR: $IMAGE not found.  Run 'make' first."
    exit 1
fi

# nxfs-disk.img is no longer a build artifact — create the persistent AHCI
# disk on demand so storage survives reboots in QEMU.
if [ ! -f "$DATA_DISK" ]; then
    echo "  Creating persistent 64 MiB AHCI disk -> $DATA_DISK"
    dd if=/dev/zero of="$DATA_DISK" bs=1M count=64 status=none
fi

echo "=== NexxoN OS Thin-Client QEMU ==="
echo "  Proxy must be running on host:9090"
echo "  Network:  hostfwd tcp::9090 -> guest:9090"
echo "  Audio:    AC97 + SDL backend"
echo "  NIC:      E1000 (user-net)"
echo "  Storage:  AHCI/SATA ($DATA_DISK, persistent)"
echo ""

exec "$QEMU" \
    -m 256M \
    -cdrom "$IMAGE" \
    -drive "id=nxfsdisk,file=$DATA_DISK,format=raw,if=none" \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=nxfsdisk,bus=ahci.0 \
    -boot order=d \
    -vga std \
    -no-reboot \
    -netdev "user,id=net0,hostfwd=tcp::9090-:9090" \
    -device e1000,netdev=net0 \
    -device AC97 \
    -audiodev "sdl,id=audio0" \
    -machine "pcspk-audiodev=audio0"
