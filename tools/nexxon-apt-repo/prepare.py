#!/usr/bin/env python3
"""Populate tools/nexxon-apt-repo/ with built userland ELFs for QEMU HTTP apt."""
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REPO = os.path.join(ROOT, "tools", "nexxon-apt-repo")
BUILD = os.path.join(ROOT, "build", "userland")
GLIBC_LD = os.environ.get(
    "GLIBC_LD", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
)
GLIBC_RT = os.environ.get("GLIBC_RT", "/lib/x86_64-linux-gnu/libc.so.6")
I386_LD = os.environ.get("I386_LD", "/lib32/ld-linux.so.2")
I386_RT = os.environ.get("I386_RT", "/lib32/libc.so.6")
STEAM_BOOTSTRAP = os.environ.get(
    "STEAM_BOOTSTRAP", "/tmp/steam-bootstrap/ubuntu12_32/steam"
)


def copy_one(src, dst_name):
    dst = os.path.join(REPO, dst_name)
    if not os.path.exists(src):
        print(f"missing {src}; run make first", file=sys.stderr)
        return 1
    shutil.copy2(src, dst)
    print(f"copied {src} -> {dst}")
    return 0


def build_x11_bundle():
    libs = (
        ("libX11.so.6", "/lib/x86_64-linux-gnu/libX11.so.6"),
        ("libxcb.so.1", "/lib/x86_64-linux-gnu/libxcb.so.1"),
        ("libXau.so.6", "/lib/x86_64-linux-gnu/libXau.so.6"),
        ("libXdmcp.so.6", "/lib/x86_64-linux-gnu/libXdmcp.so.6"),
        ("libbsd.so.0", "/lib/x86_64-linux-gnu/libbsd.so.0"),
        ("libmd.so.0", "/lib/x86_64-linux-gnu/libmd.so.0"),
    )
    dst = os.path.join(REPO, "x11-libs.bundle")
    try:
        with open(dst, "wb") as out:
            out.write(b"NXXLIB1\0")
            out.write(struct.pack("<I", len(libs)))
            for name, src in libs:
                with open(src, "rb") as inp:
                    data = inp.read()
                encoded = name.encode("ascii")
                out.write(struct.pack("<II", len(encoded), len(data)))
                out.write(encoded)
                out.write(data)
        print(f"built X11 library bundle -> {dst}")
        return 0
    except OSError as exc:
        print(f"cannot build X11 library bundle: {exc}", file=sys.stderr)
        return 1


def build_vulkan_bundle():
    libs = (
        ("libvulkan.so.1", "/lib/x86_64-linux-gnu/libvulkan.so.1"),
        ("libm.so.6", "/lib/x86_64-linux-gnu/libm.so.6"),
    )
    dst = os.path.join(REPO, "vkloader-libs.bundle")
    try:
        with open(dst, "wb") as out:
            out.write(b"NXVKLIB1")
            out.write(struct.pack("<I", len(libs)))
            for name, src in libs:
                with open(src, "rb") as inp:
                    data = inp.read()
                encoded = name.encode("ascii")
                out.write(struct.pack("<II", len(encoded), len(data)))
                out.write(encoded)
                out.write(data)
        print(f"built Vulkan loader bundle -> {dst}")
        return 0
    except OSError as exc:
        print(f"cannot build Vulkan loader bundle: {exc}", file=sys.stderr)
        return 1


def build_glibc_bundle():
    libs = (
        ("ld-linux-x86-64.so.2", GLIBC_LD),
        ("libc.so.6", GLIBC_RT),
    )
    dst = os.path.join(REPO, "glibc-libs.bundle")
    try:
        with open(dst, "wb") as out:
            out.write(b"NXGLIBC1")
            out.write(struct.pack("<I", len(libs)))
            for name, src in libs:
                with open(src, "rb") as inp:
                    data = inp.read()
                encoded = name.encode("ascii")
                out.write(struct.pack("<II", len(encoded), len(data)))
                out.write(encoded)
                out.write(data)
        print(f"built glibc runtime bundle -> {dst}")
        return 0
    except OSError as exc:
        print(f"cannot build glibc runtime bundle: {exc}", file=sys.stderr)
        return 1


def build_i386_bundle():
    libs = (
        ("ld-linux.so.2", I386_LD),
        ("libc.so.6", I386_RT),
        ("libdl.so.2", "/lib32/libdl.so.2"),
        ("librt.so.1", "/lib32/librt.so.1"),
        ("libm.so.6", "/lib32/libm.so.6"),
        ("libpthread.so.0", "/lib32/libpthread.so.0"),
        ("libgcc_s.so.1", "/usr/lib32/libgcc_s.so.1"),
        ("libstdc++.so.6", "/usr/lib32/libstdc++.so.6"),
    )
    dst = os.path.join(REPO, "i386-libs.bundle")
    try:
        with open(dst, "wb") as out:
            out.write(b"NXI386R1")
            out.write(struct.pack("<I", len(libs)))
            for name, src in libs:
                with open(src, "rb") as inp:
                    data = inp.read()
                encoded = name.encode("ascii")
                out.write(struct.pack("<II", len(encoded), len(data)))
                out.write(encoded)
                out.write(data)
        print(f"built i386 runtime bundle -> {dst}")
        return 0
    except OSError as exc:
        print(f"cannot build i386 runtime bundle: {exc}", file=sys.stderr)
        return 1


def build_i386_steam_gfx_bundle():
    script = os.path.join(ROOT, "tools", "i386-steam-shim", "build.sh")
    try:
        subprocess.check_call(["bash", script], cwd=ROOT)
    except (subprocess.CalledProcessError, OSError) as exc:
        print(f"cannot build i386 Steam gfx shims: {exc}", file=sys.stderr)
        return 1
    libs = (
        ("libX11.so.6", "i386-libX11.so.6"),
        ("libGL.so.1", "i386-libGL.so.1"),
        ("libGLX.so", "i386-libGLX.so"),
        ("steamui.so", "i386-steamui.so"),
        ("libXrandr.so.2", "i386-libXrandr.so.2"),
    )
    dst = os.path.join(REPO, "i386-steam-gfx.bundle")
    try:
        with open(dst, "wb") as out:
            out.write(b"NXI386G1")
            out.write(struct.pack("<I", len(libs)))
            for name, src_name in libs:
                src = os.path.join(REPO, src_name)
                with open(src, "rb") as inp:
                    data = inp.read()
                encoded = name.encode("ascii")
                out.write(struct.pack("<II", len(encoded), len(data)))
                out.write(encoded)
                out.write(data)
        print(f"built i386 Steam gfx bundle -> {dst}")
        return 0
    except OSError as exc:
        print(f"cannot build i386 Steam gfx bundle: {exc}", file=sys.stderr)
        return 1


def build_steam_seed_bundle():
    dst = os.path.join(REPO, "steam-client-seed.bundle")
    entries = (
        ("steam_client_ubuntu12.installed", b"installed\n"),
        ("steam_client_metrics.bin", b"NEXXON_SEED\n"),
        ("steamui.so", b""),
    )
    steamui = os.path.join(REPO, "i386-steamui.so")
    if os.path.exists(steamui):
        with open(steamui, "rb") as f:
            steamui_data = f.read()
        entries = (
            ("steam_client_ubuntu12.installed", b"installed\n"),
            ("steam_client_metrics.bin", b"NEXXON_SEED\n"),
            ("steamui.so", steamui_data),
        )
    try:
        with open(dst, "wb") as out:
            out.write(b"NXSTMCL1")
            out.write(struct.pack("<I", len(entries)))
            for name, data in entries:
                encoded = name.encode("ascii")
                out.write(struct.pack("<II", len(encoded), len(data)))
                out.write(encoded)
                out.write(data)
        print(f"built Steam client seed bundle -> {dst}")
        return 0
    except OSError as exc:
        print(f"cannot build Steam client seed bundle: {exc}", file=sys.stderr)
        return 1


def write_packages():
    desc = {
        "netprobe": "HTTP apt integration probe (musl static ELF)",
        "linuxdemo": "Musl static integration demo (optional net mirror)",
        "glibc-ld.so": "glibc dynamic loader seed for linuxglibc",
        "glibc-libc.so": "glibc libc.so.6 seed for linuxglibc",
        "steam-bootstrap.elf": "Official Valve Steam bootstrap (ubuntu12_32)",
        "steam-client-seed.bundle": "Steam client install markers for NexxoN",
        "i386-steam-gfx.bundle": "i386 libX11/GL/steamui for Steam update UI",
        "steamwebstub.elf": "steamwebhelper CEF subprocess stub",
    }
    skip = {".py", ".bundle"}
    names = []
    for entry in sorted(os.listdir(REPO)):
        path = os.path.join(REPO, entry)
        if not os.path.isfile(path):
            continue
        if entry == "Packages":
            continue
        if any(entry.endswith(s) for s in skip) and entry != "steam-bootstrap.elf":
            if entry.endswith(".bundle") and entry not in (
                "glibc-libs.bundle",
                "i386-libs.bundle",
                "x11-libs.bundle",
                "vkloader-libs.bundle",
                "steam-client-seed.bundle",
                "i386-steam-gfx.bundle",
            ):
                continue
        names.append(entry)
    lines = []
    for name in names:
        lines.append(f"Package: {name}")
        lines.append("Version: 1.0-nexxon")
        lines.append("Architecture: amd64")
        lines.append(f"Filename: {name}")
        lines.append(f"Description: {desc.get(name, 'NexxoN apt payload')}")
        lines.append("")
    pkg_path = os.path.join(REPO, "Packages")
    with open(pkg_path, "w", encoding="ascii") as out:
        out.write("\n".join(lines))
    print(f"wrote Packages index ({len(names)} entries) -> {pkg_path}")
    return 0


def fetch_steam_cdn_probe():
    dst = os.path.join(REPO, "steam-cdn-probe.html")
    try:
        subprocess.check_call(
            [
                "curl", "-fsSL", "--max-time", "30",
                "-o", dst,
                "https://client-update.steamstatic.com/",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        print(f"fetched Steam CDN probe -> {dst}")
        return 0
    except (subprocess.CalledProcessError, OSError) as exc:
        print(f"warning: Steam CDN probe fetch skipped: {exc}", file=sys.stderr)
        with open(dst, "w", encoding="utf-8") as f:
            f.write("<html><body>NexxoN steam CDN apt fallback probe</body></html>\n")
        print(f"wrote apt fallback Steam CDN probe -> {dst}")
        return 0


def main():
    os.makedirs(REPO, exist_ok=True)
    subprocess.check_call(["make", "-C", ROOT, "kernel"], stdout=subprocess.DEVNULL)
    rc = 0
    for name in ("netprobe", "linuxdemo"):
        if copy_one(os.path.join(BUILD, "linuxdemo.elf"), name):
            rc = 1
    for name, src in (
        ("linuxepoll.elf", "linuxepoll.elf"),
        ("linuxglibc.elf", "linuxglibc.elf"),
        ("linuxdri.elf", "linuxdri.elf"),
        ("linuxmmap.elf", "linuxmmap.elf"),
        ("linuxdrmfb.elf", "linuxdrmfb.elf"),
        ("linuxalsa.elf", "linuxalsa.elf"),
        ("linuxx11.elf", "linuxx11.elf"),
        ("linuxxlib.elf", "linuxxlib.elf"),
        ("linuxxevent.elf", "linuxxevent.elf"),
        ("linuxvulkan.elf", "linuxvulkan.elf"),
        ("linuxvulkanso.elf", "linuxvulkanso.elf"),
        ("linuxvkloader.elf", "linuxvkloader.elf"),
        ("linuxvkinstance.elf", "linuxvkinstance.elf"),
        ("linuxvkqueue.elf", "linuxvkqueue.elf"),
        ("linuxvkcommand.elf", "linuxvkcommand.elf"),
        ("linuxvksurface.elf", "linuxvksurface.elf"),
        ("linuxvkswapchain.elf", "linuxvkswapchain.elf"),
        ("linuxpulse.elf", "linuxpulse.elf"),
        ("linux32.elf", "linux32.elf"),
        ("linux32glibc.elf", "linux32glibc.elf"),
        ("linux32pie.elf", "linux32pie.elf"),
        ("linux32pthread.elf", "linux32pthread.elf"),
        ("steamwebstub.elf", "steamwebstub.elf"),
        ("glibc-ld.so", GLIBC_LD),
        ("glibc-libc.so", GLIBC_RT),
        ("i386-ld.so", I386_LD),
        ("i386-libc.so", I386_RT),
    ):
        if copy_one(os.path.join(BUILD, src) if src.endswith(".elf") else src, name):
            if name.startswith("glibc-"):
                print(f"warning: glibc seed {name} missing", file=sys.stderr)
            else:
                rc = 1
    if build_x11_bundle():
        rc = 1
    if build_vulkan_bundle():
        rc = 1
    if build_glibc_bundle():
        rc = 1
    if build_i386_bundle():
        rc = 1
    if build_i386_steam_gfx_bundle():
        rc = 1
    if build_steam_seed_bundle():
        rc = 1
    if os.path.exists(STEAM_BOOTSTRAP):
        if copy_one(STEAM_BOOTSTRAP, "steam-bootstrap.elf"):
            rc = 1
    else:
        print("optional Steam bootstrap not present; skipping", file=sys.stderr)
    fetch_steam_cdn_probe()
    if write_packages():
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
