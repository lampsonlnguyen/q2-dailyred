#!/usr/bin/env bash
# Installs the ARM cross-compiler for building the Daily Red ROM on Linux,
# into ./.toolchain (no sudo, nothing written outside this project):
#
#   - xPack GNU Arm Embedded GCC 14.2.1-1.1 (arm-none-eabi-gcc + binutils +
#     newlib), the same release upstream Daily Red pins for macOS
#     (scripts/setup-macos.sh), but the linux-x64 tarball.
#
# Linux does NOT need the static libpng build from setup-macos.sh:
# pokefirered's host tool gbagfx uses the system libpng
# (libpng-dev + pkg-config, already present on this VM). If pkg-config
# cannot find libpng, install it first: sudo apt-get install libpng-dev
# pkg-config.
#
# Alternative (simpler, no download): sudo apt install gcc-arm-none-eabi
# (noble ships GCC 13.2, well within what pokefirered MODERN=1 supports)
# and skip this script entirely — scripts/build-rom-linux.sh will find
# the system compiler. Re-running this script is safe: finished steps
# are skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="$ROOT/.toolchain"
mkdir -p "$TC/downloads"

GCC_VER="14.2.1-1.1"
GCC_NAME="xpack-arm-none-eabi-gcc-$GCC_VER"
GCC_HOST="linux-x64"

if [ ! -x "$TC/arm-none-eabi/bin/arm-none-eabi-gcc" ]; then
  tarball="$GCC_NAME-$GCC_HOST.tar.gz"
  url="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$GCC_VER/$tarball"
  echo ">> Downloading $tarball"
  curl -fL --retry 3 -o "$TC/downloads/$tarball" "$url"
  curl -fsL -o "$TC/downloads/$tarball.sha" "$url.sha"
  expected="$(awk '{print $1}' "$TC/downloads/$tarball.sha")"
  actual="$(sha256sum "$TC/downloads/$tarball" | awk '{print $1}')"
  [ "$expected" = "$actual" ] || { echo "Checksum mismatch for $tarball"; exit 1; }
  echo ">> Checksum OK, extracting"
  tar -xzf "$TC/downloads/$tarball" -C "$TC"
  rm -f "$TC/arm-none-eabi"
  ln -s "$GCC_NAME" "$TC/arm-none-eabi"
  rm -f "$TC/downloads/$tarball"
fi
"$TC/arm-none-eabi/bin/arm-none-eabi-gcc" --version | head -1
# Fail early if the compiler can't actually run.
echo 'int f(void){return 0;}' | "$TC/arm-none-eabi/bin/arm-none-eabi-gcc" -x c -c -o /dev/null -

if ! pkg-config --exists libpng 2>/dev/null; then
  echo "WARNING: pkg-config cannot find libpng. Install it before building the ROM:"
  echo "  sudo apt-get install libpng-dev pkg-config"
fi

echo
echo "Toolchain ready. Build with: scripts/build-rom-linux.sh"
