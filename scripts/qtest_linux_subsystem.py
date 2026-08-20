#!/usr/bin/env python3
"""QEMU harness: Linux subsystem v33 — Steam graphical + i386 ladder."""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(ROOT, "nexxon-os.img")
DATA = os.path.join(ROOT, "nexxon-data.img")
REPO = os.path.join(ROOT, "tools", "nexxon-apt-repo")
QMP = "/tmp/nexxon-qmp.sock"
SERIAL = "/tmp/nexxon-com1.log"
PPM = os.path.join(ROOT, "linux-subsystem-test.ppm")
APT_PORT = 8000


def ppm_has_nonblack_pixels(path, sample_step=16, min_hits=8):
    """Return True when enough non-black RGB samples exist in a PPM."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            return False
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        dims = line.decode().strip().split()
        if len(dims) < 2:
            return False
        width, height = int(dims[0]), int(dims[1])
        maxval = int(f.readline().decode().strip())
        if maxval <= 0:
            return False
        row_bytes = width * 3
        hits = 0
        for y in range(0, height, sample_step):
            f.seek(y * row_bytes)
            row = f.read(row_bytes)
            if len(row) < row_bytes:
                break
            for x in range(0, width * 3, sample_step * 3):
                r, g, b = row[x], row[x + 1], row[x + 2]
                if r > 8 or g > 8 or b > 8:
                    hits += 1
                    if hits >= min_hits:
                        return True
    return False


def qmp_cmd(sock, cmd, timeout=30.0):
    sock.settimeout(timeout)
    sock.sendall((json.dumps(cmd) + "\n").encode())
    while True:
        line = sock.recv(4096)
        if not line:
            raise RuntimeError("QMP closed")
        for part in line.decode(errors="replace").splitlines():
            if not part.strip():
                continue
            msg = json.loads(part)
            if "return" in msg or "error" in msg:
                return msg
            if msg.get("event") == "STOP":
                pass


def wait_serial(marker, timeout=120.0):
    deadline = time.time() + timeout
    with open(SERIAL, "r", errors="replace") as f:
        f.seek(0, os.SEEK_END)
        buf = ""
        while time.time() < deadline:
            chunk = f.read()
            if chunk:
                buf += chunk
                if marker in buf:
                    return buf
            time.sleep(0.2)
    raise TimeoutError(f"timeout waiting for {marker!r} in COM1\n---\n{buf[-4000:]}")


QCODE_MAP = {"\n": "ret", " ": "spc", "-": "minus", "/": "slash", ".": "dot"}


def send_keys(sock, text):
    for ch in text:
        key = QCODE_MAP.get(ch, ch)
        qmp_cmd(sock, {"execute": "send-key", "arguments": {
            "keys": [{"type": "qcode", "data": key}],
        }})


def mouse_click(sock, x, y, sw=1024, sh=768):
    ax = int(x * 32767 / max(sw - 1, 1))
    ay = int(y * 32767 / max(sh - 1, 1))
    for axis, val in (("x", ax), ("y", ay)):
        qmp_cmd(sock, {"execute": "input-send-event", "arguments": {
            "device": "mouse", "type": "abs",
            "value": {"axis": axis, "input": val}}})
    qmp_cmd(sock, {"execute": "input-send-event", "arguments": {
        "device": "mouse", "type": "btn",
        "value": {"down": True, "button": "left"}}})
    qmp_cmd(sock, {"execute": "input-send-event", "arguments": {
        "device": "mouse", "type": "btn",
        "value": {"down": False, "button": "left"}}})


def focus_shell(sock):
    mouse_click(sock, 360, 220)
    time.sleep(0.3)


def start_apt_server():
    subprocess.check_call([sys.executable, os.path.join(REPO, "prepare.py")],
                          cwd=ROOT)
    try:
        # DEVNULL, not PIPE: an unread PIPE fills up with per-request log
        # lines mid-test and the server blocks on write — downloads then
        # hang with no response on an established connection.
        proc = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(APT_PORT), "--bind", "127.0.0.1"],
            cwd=REPO,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError as exc:
        raise RuntimeError(f"failed to start apt HTTP server on {APT_PORT}: {exc}") from exc
    time.sleep(0.5)
    if proc.poll() is not None:
        raise RuntimeError(
            f"apt HTTP server exited early (is port {APT_PORT} in use?)")
    return proc


def main():
    if os.path.exists(QMP):
        os.remove(QMP)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    if os.path.exists(DATA):
        os.remove(DATA)
    subprocess.check_call(
        ["dd", "if=/dev/zero", f"of={DATA}", "bs=1M", "count=64", "status=none"]
    )

    apt_srv = start_apt_server()
    qemu = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-m",
            "256M",
            "-cdrom",
            IMAGE,
            "-drive",
            f"id=nxfsdisk,file={DATA},format=raw,if=none",
            "-device",
            "ich9-ahci,id=ahci",
            "-device",
            "ide-hd,drive=nxfsdisk,bus=ahci.0",
            "-boot",
            "order=d",
            "-vga",
            "std",
            "-display",
            "none",
            "-no-reboot",
            "-serial",
            f"file:{SERIAL}",
            "-qmp",
            f"unix:{QMP},server=on,wait=off",
            "-netdev",
            "user,id=net0",
            "-device",
            "e1000,netdev=net0",
        ],
        cwd=ROOT,
    )

    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            if os.path.exists(QMP):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("QMP socket not created")

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(QMP)
        sock.recv(4096)
        qmp_cmd(sock, {"execute": "qmp_capabilities"})

        wait_serial("entering shell", timeout=90)
        time.sleep(1.0)
        focus_shell(sock)
        send_keys(sock, "linux\n")
        wait_serial("entering private CR3 '/bin/sh'", timeout=60)
        time.sleep(1.0)
        focus_shell(sock)
        send_keys(sock, "apt-get update\n")
        wait_serial("apt update: fetched Packages", timeout=45)
        time.sleep(0.8)
        focus_shell(sock)
        if os.environ.get("NEXXON_STEAM_GRAPHICAL"):
            send_keys(sock, "apt-get install steam\n")
            wait_serial("apt installed steam", timeout=480)
            time.sleep(0.8)
            focus_shell(sock)
            send_keys(sock, "steam\n")
            wait_serial(
                "sync run '/programs/ubuntu12_32/steam' argc", timeout=120
            )
            wait_serial(
                "sync run '/programs/ubuntu12_32/steam' finished code=0",
                timeout=300,
            )
            qmp_cmd(sock, {"execute": "screendump",
                           "arguments": {"filename": PPM}})
            with open(SERIAL, errors="replace") as f:
                steam_log = f.read()
            # Marker sets differ between official bootstrap generations:
            # the modern steam_latest prints "Steam Client launched with:" +
            # "Using update UI:", the 1.0.0.x archive bootstrap prints
            # "Startup - updater built".  Both are the binary's own output.
            if "Startup - Steam Client launched with:" in steam_log:
                required = (
                    "Process started with command-line:",
                    "Using update UI:",
                    "NEXXON_STEAM_CLIENT_OK",
                )
            else:
                required = (
                    "Startup - updater built",
                    "NEXXON_STEAM_CLIENT_OK",
                )
            if not all(marker in steam_log for marker in required):
                raise RuntimeError("Steam graphical markers missing")
            if ("Using update UI: xwin" in steam_log or
                    "Using update UI: glx" in steam_log):
                if not ppm_has_nonblack_pixels(PPM):
                    raise RuntimeError("Steam graphical screendump is blank")
            print(steam_log[-12000:])
            print("PASS: Steam full install + update UI")
            return 0
        if os.environ.get("NEXXON_STEAM_FAST"):
            send_keys(sock, "apt-get install steam-bootstrap\n")
            wait_serial("apt installed steam-bootstrap", timeout=480)
            time.sleep(0.8)
            focus_shell(sock)
            send_keys(sock, "steam\n")
            wait_serial(
                "sync run '/programs/ubuntu12_32/steam' argc", timeout=120
            )
            wait_serial(
                "sync run '/programs/ubuntu12_32/steam' finished code=0",
                timeout=300,
            )
            qmp_cmd(sock, {"execute": "screendump",
                           "arguments": {"filename": PPM}})
            with open(SERIAL, errors="replace") as f:
                steam_log = f.read()
            # Generation-agnostic markers (see the graphical branch note).
            if "Startup - Steam Client launched with:" in steam_log:
                required = (
                    "Process started with command-line:",
                    "Using update UI: console",
                    "NEXXON_STEAM_CDN_OK",
                )
            else:
                required = (
                    "Startup - updater built",
                    "NEXXON_STEAM_CDN_OK",
                )
            if not all(marker in steam_log for marker in required):
                raise RuntimeError("Steam bootstrap markers missing")
            print(steam_log[-12000:])
            print("PASS: official Steam bootstrap + update UI child")
            return 0
        if os.environ.get("NEXXON_I386_FAST"):
            send_keys(sock, "apt-get install linux32pthread\n")
            wait_serial("apt installed linux32pthread", timeout=120)
            time.sleep(1.0)
            focus_shell(sock)
            send_keys(sock, "linux32pthread\n")
            wait_serial(
                "[linux/thr/i386] clone",
                timeout=120,
            )
            wait_serial(
                "sync run '/programs/linux32pthread' finished code=0",
                timeout=180,
            )
            qmp_cmd(sock, {"execute": "screendump",
                           "arguments": {"filename": PPM}})
            print("PASS: i386 CLONE_THREAD pthread fast path")
            return 0
        send_keys(sock, "apt-get install linuxnet\n")
        wait_serial("apt installed linuxnet", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxnet\n")
        wait_serial("sync run '/programs/linuxnet' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxpthread\n")
        wait_serial("apt installed linuxpthread", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxpthread\n")
        wait_serial("sync run '/programs/linuxpthread' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxepoll\n")
        wait_serial("apt installed linuxepoll", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxepoll\n")
        wait_serial("sync run '/programs/linuxepoll' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxglibc\n")
        wait_serial("apt installed linuxglibc", timeout=120)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxglibc\n")
        wait_serial("sync run '/programs/linuxglibc' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxdri\n")
        wait_serial("apt installed linuxdri", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxdri\n")
        wait_serial("sync run '/programs/linuxdri' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxmmap\n")
        wait_serial("apt installed linuxmmap", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxmmap\n")
        wait_serial("sync run '/programs/linuxmmap' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxdrmfb\n")
        wait_serial("apt installed linuxdrmfb", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxdrmfb\n")
        wait_serial("sync run '/programs/linuxdrmfb' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxalsa\n")
        wait_serial("apt installed linuxalsa", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxalsa\n")
        wait_serial("sync run '/programs/linuxalsa' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxx11\n")
        wait_serial("apt installed linuxx11", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxx11\n")
        wait_serial("sync run '/programs/linuxx11' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxxlib\n")
        wait_serial("apt installed linuxxlib", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxxlib\n")
        wait_serial("sync run '/programs/linuxxlib' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxxevent\n")
        wait_serial("apt installed linuxxevent", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxxevent\n")
        wait_serial("sync run '/programs/linuxxevent' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvulkan\n")
        wait_serial("apt installed linuxvulkan", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvulkan\n")
        wait_serial("sync run '/programs/linuxvulkan' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvulkanso\n")
        wait_serial("apt installed linuxvulkanso", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvulkanso\n")
        wait_serial("sync run '/programs/linuxvulkanso' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvkloader\n")
        wait_serial("apt installed linuxvkloader", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvkloader\n")
        wait_serial("sync run '/programs/linuxvkloader' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvkinstance\n")
        wait_serial("apt installed linuxvkinstance", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvkinstance\n")
        wait_serial("sync run '/programs/linuxvkinstance' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvkqueue\n")
        wait_serial("apt installed linuxvkqueue", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvkqueue\n")
        wait_serial("sync run '/programs/linuxvkqueue' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvkcommand\n")
        wait_serial("apt installed linuxvkcommand", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvkcommand\n")
        wait_serial("sync run '/programs/linuxvkcommand' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvksurface\n")
        wait_serial("apt installed linuxvksurface", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvksurface\n")
        wait_serial("sync run '/programs/linuxvksurface' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxvkswapchain\n")
        wait_serial("apt installed linuxvkswapchain", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxvkswapchain\n")
        wait_serial("sync run '/programs/linuxvkswapchain' finished code=0", timeout=90)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linuxpulse\n")
        wait_serial("apt installed linuxpulse", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linuxpulse\n")
        wait_serial("sync run '/programs/linuxpulse' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linux32\n")
        wait_serial("apt installed linux32", timeout=30)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linux32\n")
        wait_serial("sync run '/programs/linux32' finished code=0", timeout=60)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "apt-get install linux32pie\n")
        wait_serial("apt installed linux32pie", timeout=120)
        time.sleep(0.8)
        focus_shell(sock)
        send_keys(sock, "linux32pie\n")
        wait_serial("sync run '/programs/linux32pie' finished code=0", timeout=90)
        time.sleep(2.5)
        focus_shell(sock)
        time.sleep(0.5)
        focus_shell(sock)
        send_keys(sock, "apt-get install linux32pthread\n")
        wait_serial("apt installed linux32pthread", timeout=120)
        time.sleep(1.0)
        focus_shell(sock)
        send_keys(sock, "linux32pthread\n")
        wait_serial("sync run '/programs/linux32pthread' finished code=0", timeout=180)

        qmp_cmd(sock, {"execute": "screendump", "arguments": {"filename": PPM}})
        time.sleep(0.5)

        with open(SERIAL, errors="replace") as f:
            log = f.read()
        print("=== COM1 tail ===")
        print(log[-6000:])
        print("=== screendump ===")
        print(PPM, os.path.getsize(PPM) if os.path.exists(PPM) else "missing")

        ok = all(
            m in log
            for m in (
                "seeded rootfs /bin/sh",
                "entering private CR3 '/bin/sh'",
                "DNS apt.nexxon",
                "apt update: fetched Packages",
                "apt installed linuxnet",
                "[linux/sock] TCP connect",
                "sync run '/programs/linuxnet' finished code=0",
                "[linux/thr] clone",
                "sync run '/programs/linuxpthread' finished code=0",
                "[linux/epoll] wait",
                "sync run '/programs/linuxepoll' finished code=0",
                "apt bundle installed ld-linux-x86-64.so.2",
                "apt bundle installed libc.so.6",
                "sync run '/programs/linuxglibc' finished code=0",
                "[linux/dri] DRM_VERSION",
                "sync run '/programs/linuxdri' finished code=0",
                "[linux/mmap] fault-in",
                "sync run '/programs/linuxmmap' finished code=0",
                "[linux/dri] CREATE_DUMB",
                "sync run '/programs/linuxdrmfb' finished code=0",
                "[linux/alsa] pcm write",
                "sync run '/programs/linuxalsa' finished code=0",
                "[linux/x11] display :0",
                "[linux/x11] setup ok",
                "sync run '/programs/linuxx11' finished code=0",
                "sync run '/programs/linuxxlib' finished code=0",
                "[linux/x11] MapNotify",
                "[linux/x11] Expose",
                "sync run '/programs/linuxxevent' finished code=0",
                "[linux/vulkan] seeded ICD manifest",
                "sync run '/programs/linuxvulkan' finished code=0",
                "[linux/vulkan] seeded ICD library",
                "sync run '/programs/linuxvulkanso' finished code=0",
                "sync run '/programs/linuxvkloader' finished code=0",
                "sync run '/programs/linuxvkinstance' finished code=0",
                "sync run '/programs/linuxvkqueue' finished code=0",
                "sync run '/programs/linuxvkcommand' finished code=0",
                "sync run '/programs/linuxvksurface' finished code=0",
                "sync run '/programs/linuxvkswapchain' finished code=0",
                "[linux/pulse] native @/run/user/0/pulse/native ready",
                "[linux/pulse] handshake",
                "sync run '/programs/linuxpulse' finished code=0",
                "[linux/i386] ELF32 compat",
                "sync run '/programs/linux32' finished code=0",
                "sync run '/programs/linux32pie' finished code=0",
                "[linux/thr/i386] clone",
                "sync run '/programs/linux32pthread' finished code=0",
            )
        )
        if not ok:
            print("FAIL: missing markers", file=sys.stderr)
            with open(SERIAL, errors="replace") as f:
                print("=== FULL COM1 ===", file=sys.stderr)
                print(f.read(), file=sys.stderr)
            return 1
        print("PASS: linux subsystem steam-prep ladder (pre-client)")
        return 0
    finally:
        qemu.terminate()
        apt_srv.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
        try:
            apt_srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            apt_srv.kill()


if __name__ == "__main__":
    sys.exit(main())
