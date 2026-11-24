#!/usr/bin/env bash
# Builds the modified FireRed ROM with an ARM cross-compiler and puts a
# plain copy in .build/rom/dailyred.gba for the Q2 card app.
#
# Linux port of upstream Daily Red's scripts/build-rom.sh. Differences:
#   - Toolchain: the project-local xPack install from scripts/setup-linux.sh
#     (.toolchain/arm-none-eabi) if present, else the system
#     arm-none-eabi-gcc (e.g. from `sudo apt install gcc-arm-none-eabi`),
#     else $TOOLCHAIN if the caller sets it.
#   - No gzip step: the Q2 loads the 16 MB ROM straight from the SD card,
#     so there is nothing to gain by gzipping it (upstream gzips only for
#     the website download).
#   - Output goes to .build/rom/dailyred.gba (local scratch, never
#     committed), not web/game/.
#   - The built ROM is verified to contain the DAILYRED-CONF-V2 magic;
#     a build without it is a failed build.
#
#   scripts/build-rom-linux.sh         # normal build
#   scripts/build-rom-linux.sh clean   # any extra args are passed to make
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/.build/pokefirered"
OUTDIR="$ROOT/.build/rom"

if [ ! -d "$GAME" ]; then
  echo "pokefirered/ not found at $GAME. Run scripts/fetch-pokefirered.sh first."
  exit 1
fi

# --- Resolve the toolchain -------------------------------------------------
# pokefirered's Makefile uses TOOLCHAIN in one of two ways depending on
# version: either as a directory holding bin/arm-none-eabi-gcc, or as a
# command prefix (…/bin/arm-none-eabi-gcc). Detect which from the Makefile
# instead of guessing, so both the xPack and system installs work.
TC_ARG="${TOOLCHAIN:-}"
if [ -z "$TC_ARG" ]; then
  if [ -x "$ROOT/.toolchain/arm-none-eabi/bin/arm-none-eabi-gcc" ]; then
    TC_ROOT="$ROOT/.toolchain/arm-none-eabi"
  elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    TC_BIN="$(command -v arm-none-eabi-gcc)"
    TC_ROOT="$(dirname "$(dirname "$TC_BIN")")"
  else
    echo "No ARM toolchain found. Either run scripts/setup-linux.sh"
    echo "(xPack GCC 14.2.1, project-local) or install the system one:"
    echo "  sudo apt-get install gcc-arm-none-eabi"
    echo "Or set TOOLCHAIN to the toolchain dir or command prefix."
    exit 1
  fi
  if grep -qE '\$\(TOOLCHAIN\)/bin/' "$GAME/Makefile" 2>/dev/null; then
    TC_ARG="$TC_ROOT"
  elif grep -qE '\$\(TOOLCHAIN\)-gcc' "$GAME/Makefile" 2>/dev/null; then
    TC_ARG="$TC_ROOT/bin/arm-none-eabi"
  else
    echo "Could not tell how $GAME/Makefile uses TOOLCHAIN. Set TOOLCHAIN"
    echo "explicitly (directory or command prefix) and re-run."
    grep -n "TOOLCHAIN" "$GAME/Makefile" | head -10 || true
    exit 1
  fi
fi

# Prove the compiler runs before the long build starts.
if [ -x "$TC_ARG/bin/arm-none-eabi-gcc" ]; then
  CC="$TC_ARG/bin/arm-none-eabi-gcc"
elif command -v "$TC_ARG-gcc" >/dev/null 2>&1; then
  CC="$TC_ARG-gcc"
else
  echo "TOOLCHAIN '$TC_ARG' does not resolve to an arm-none-eabi-gcc."
  exit 1
fi
"$CC" --version | head -1

JOBS="$(nproc 2>/dev/null || echo 4)"

if [ "$#" -gt 0 ]; then
  make -C "$GAME" -j"$JOBS" TOOLCHAIN="$TC_ARG" MODERN=1 "$@"
  exit 0
fi

make -C "$GAME" -j"$JOBS" TOOLCHAIN="$TC_ARG" MODERN=1

ROM="$GAME/pokefirered_modern.gba"
if [ ! -f "$ROM" ]; then
  echo "Build finished but $ROM is missing."
  exit 1
fi
# The launcher scans for this magic at runtime; a ROM without it would
# boot to the vanilla title screen instead of the daily battle.
if ! grep -q "DAILYRED-CONF-V2" "$ROM"; then
  echo "ERROR: $ROM has no DAILYRED-CONF-V2 magic. The Daily Red patch"
  echo "did not apply cleanly — re-run scripts/fetch-pokefirered.sh."
  exit 1
fi
mkdir -p "$OUTDIR"
cp "$ROM" "$OUTDIR/dailyred.gba"
echo
echo "Built $ROM ($(wc -c < "$ROM" | tr -d ' ') bytes, expect 16777216)"
echo "Q2 copy: $OUTDIR/dailyred.gba"
echo "Copy it into the card app folder as dailyred.gba (see README.md)."
