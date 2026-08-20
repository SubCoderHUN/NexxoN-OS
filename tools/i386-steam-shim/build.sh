#!/usr/bin/env bash
# Build i386 Steam graphics shims (libX11.so.6, libGL.so.1, libGLX.so)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/tools/nexxon-apt-repo"
SHIM="$ROOT/tools/i386-steam-shim"
CFLAGS="-m32 -O2 -fPIC -Wall"

build_so() {
    local src="$1" out="$2" soname="$3" extra="${4:-}"
    gcc $CFLAGS -shared -Wl,-soname,"$soname" $extra \
        -o "$OUT/$out" "$src" -lc
    echo "built $OUT/$out"
}

build_so "$SHIM/libX11_shim.c" "i386-libX11.so.6" "libX11.so.6"
build_so "$SHIM/libGL_shim.c" "i386-libGL.so.1" "libGL.so.1"
cp -f "$OUT/i386-libGL.so.1" "$OUT/i386-libGLX.so"
build_so "$SHIM/steamui_stub.c" "i386-steamui.so" "steamui.so"
build_so "$SHIM/libXrandr_shim.c" "i386-libXrandr.so.2" "libXrandr.so.2"
echo "i386 Steam gfx shims ready"
