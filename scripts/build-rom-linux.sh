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
# pret/pokefirered @ 037335f uses `TOOLCHAIN := $(DEVKITARM)` and invokes
# arm-none-eabi-* from PATH. It does NOT use TOOLCHAIN as a command prefix.
# Prefer the project-local xPack toolchain, otherwise use the system compiler.
# Do not pass TOOLCHAIN= to make: it unnecessarily overrides upstream logic.
if [ -x "$ROOT/.toolchain/arm-none-eabi/bin/arm-none-eabi-gcc" ]; then
  export PATH="$ROOT/.toolchain/arm-none-eabi/bin:$PATH"
fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  echo "ARM compiler not found; install gcc-arm-none-eabi or run scripts/setup-linux.sh."
  exit 1
fi
arm-none-eabi-gcc --version | head -1

# Prevent an unrelated DEVKITARM environment setting from enabling devkitPro
# -lsysbase on standard Ubuntu and xPack compilers.
JOBS="$(nproc 2>/dev/null || echo 4)"
if [ "$#" -gt 0 ]; then
  make -C "$GAME" -j"$JOBS" DEVKITARM= MODERN=1 "$@"
  exit 0
fi

make -C "$GAME" -j"$JOBS" DEVKITARM= MODERN=1

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
