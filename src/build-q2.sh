#!/bin/sh
# Builds the Daily Red Q2 card app for MIPS and packages it as
# dist/.dailyred/ (q2app + run + q2-dailyred binary, gpsp statically
# linked). Follows q2-boy docs/BUILD.md and src/build.sh exactly for the
# compiler flags and link line; only the frontend units differ.
#
#   sh src/build-q2.sh [--allow-skip]
#
# Requires: clang + ld.lld, and a Q2 sysroot (glibc 2.28 / NaN2008) at
# $SYSROOT (default ~/.cache/q2/sysroot). With --allow-skip the script
# exits 0 with a SKIP message when the toolchain/sysroot is absent, so CI
# can run it on runners without the Q2 toolchain (see .github/workflows/ci.yml).
#
# gpsp is fetched once into deps/gpsp (libretro/gpsp @ 5819380, plus
# patches/gpsp-q2.patch) and built to deps/gpsp/gpsp.a; re-runs reuse it.
#
# The ROM is NOT part of this package and is never copied in here: build
# it with scripts/build-rom-linux.sh and copy .build/rom/dailyred.gba into
# dist/.dailyred/dailyred.gba yourself before putting the folder on the SD
# card (see README.md). A built ROM must never be committed.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ALLOW_SKIP=0
[ "${1:-}" = "--allow-skip" ] && ALLOW_SKIP=1

skip() {
    echo "SKIP: $1"
    [ "$ALLOW_SKIP" = 1 ] && exit 0
    exit 1
}

command -v clang >/dev/null 2>&1 || skip "clang not installed (sudo apt install clang lld llvm)"
command -v ld.lld >/dev/null 2>&1 || skip "ld.lld not installed (sudo apt install lld)"
SYSROOT=${SYSROOT:-$HOME/.cache/q2/sysroot}
[ -d "$SYSROOT" ] || skip "Q2 sysroot not found at $SYSROOT (see q2-weather/tools/sysroot.sh or q2-utilities-app/tools/mksysroot.sh)"
[ -f "$SYSROOT/lib/libc.so.6" ] || skip "sysroot at $SYSROOT has no lib/libc.so.6"

GPSP_COMMIT="5819380c2ffb0900219d700a382ee68c464ebb99"
GPSP="$ROOT/deps/gpsp"
if [ ! -f "$GPSP/gpsp.a" ] || [ "$(git -C "$GPSP" rev-parse HEAD 2>/dev/null || echo none)" != "$GPSP_COMMIT" ]; then
    if [ ! -d "$GPSP/.git" ]; then
        echo ">> Cloning libretro/gpsp @ $(printf '%s' "$GPSP_COMMIT" | cut -c -7)"
        git clone https://github.com/libretro/gpsp.git "$GPSP"
    fi
    git -C "$GPSP" checkout -q "$GPSP_COMMIT"
    echo ">> Applying patches/gpsp-q2.patch"
    if git -C "$GPSP" apply --reverse --check "$ROOT/patches/gpsp-q2.patch" 2>/dev/null; then
        echo ">> gpsp-q2.patch already applied, skipping"
    elif git -C "$GPSP" apply --check "$ROOT/patches/gpsp-q2.patch" 2>/dev/null; then
        git -C "$GPSP" apply --whitespace=nowarn "$ROOT/patches/gpsp-q2.patch"
    else
        echo "ERROR: patches/gpsp-q2.patch does not apply to gpsp @ $GPSP_COMMIT"
        echo "and is not already applied. Inspect it before building."
        exit 1
    fi
    # -nostdlibinc + an explicit sysroot -isystem: clang auto-detects a
    # host gcc-mipsel-linux-gnu install (if one is present) and searches
    # ITS glibc headers BEFORE the sysroot's, which breaks the build with
    # fortify-declarator errors from the newer glibc. -nostdlibinc drops
    # the detected GCC include dir while keeping clang's builtin headers;
    # the two -isystem paths then supply glibc 2.28 only. (q2-pico8 uses
    # the blunter -nostdinc for the same reason.)
    Q2_CC="clang --target=mipsel-linux-gnu --sysroot=$SYSROOT -nostdlibinc -isystem $SYSROOT/usr/include -isystem $SYSROOT/usr/include/mipsel-linux-gnu -mabi=32 -mfp64 -mnan=2008 -mabs=2008"
    echo ">> Building gpsp.a (interpreter default, Q2 frame budget)"
    # A previous build with the wrong include paths may have left .o
    # files behind; the flags changed, so rebuild from clean.
    make -C "$GPSP" clean >/dev/null 2>&1 || true
    make -C "$GPSP" -j4 platform=mips32 STATIC_LINKING=1 TARGET=gpsp.a \
        CC="$Q2_CC" CXX="$Q2_CC" FRONTEND_SUPPORTS_RGB565=1
fi
[ -f "$GPSP/gpsp.a" ] || { echo "gpsp.a was not built"; exit 1; }

CC_Q2=${CC_Q2:-clang}
LD_Q2=${LD_Q2:-ld.lld}
out="$ROOT/dist/.dailyred/q2-dailyred"
mkdir -p "$ROOT/dist/.dailyred"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for unit in dailyred-launcher dailyred platform crt-q2; do
    echo ">> Compiling $unit.c"
    "$CC_Q2" --target=mipsel-linux-gnu --sysroot="$SYSROOT" \
        -nostdlibinc -isystem "$SYSROOT/usr/include" \
        -isystem "$SYSROOT/usr/include/mipsel-linux-gnu" -I"$GPSP/libretro/libretro-common/include" \
        -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008 \
        -D_GNU_SOURCE -std=gnu11 -O2 -Wall -Wextra -c "$ROOT/src/$unit.c" -o "$tmp/$unit.o"
done
echo ">> Linking q2-dailyred"
"$LD_Q2" -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 \
    --image-base=0x400000 -z noexecstack --hash-style=sysv --build-id=none -s \
    -o "$out" "$tmp/crt-q2.o" "$tmp/dailyred-launcher.o" "$tmp/dailyred.o" "$tmp/platform.o" \
    "$GPSP/gpsp.a" \
    "$SYSROOT/lib/libc.so.6" "$SYSROOT/lib/libm.so.6" "$SYSROOT/lib/libdl.so.2" \
    "$SYSROOT/lib/libpthread.so.0" "$SYSROOT/lib/libasound.so.2" \
    "$SYSROOT/lib/libgcc_s.so.1" "$SYSROOT/lib/ld-linux-mipsn8.so.1"
cp -a "$ROOT/package/.dailyred/q2app" "$ROOT/package/.dailyred/run" "$ROOT/dist/.dailyred/"
chmod 755 "$ROOT/dist/.dailyred/run" "$out"
echo
ls -lh "$out"
file "$out" 2>/dev/null || true
echo
echo "Card app: $ROOT/dist/.dailyred/"
echo "The ROM is NOT included. Copy your locally built ROM in as:"
echo "  cp .build/rom/dailyred.gba dist/.dailyred/dailyred.gba"
echo "Then copy dist/.dailyred/ to the SD card root as /.dailyred/"
