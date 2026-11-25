#!/usr/bin/env bash
# Recreates .build/pokefirered from a clean checkout: fetches
# pret/pokefirered at the pinned commit and applies the Daily Red changes
# from patches/pokefirered-dailyred.patch.
#
# Linux port of upstream Daily Red's scripts/get-pokefirered.sh. The only
# differences: the source lands in .build/pokefirered (local scratch, never
# committed) and the patch comes from this repo's patches/ directory.
#
# No ROM is downloaded or used here — the ROM is built from this source by
# scripts/build-rom-linux.sh. Never substitute a retail FireRed ROM.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/.build/pokefirered"
PATCH="$ROOT/patches/pokefirered-dailyred.patch"
PRET_URL="https://github.com/pret/pokefirered.git"
PRET_COMMIT="037335f4c725d7c9aecdac87066f2002b4bd7e14"

if [ -e "$GAME" ]; then
  echo "pokefirered/ already exists at $GAME. Delete it first to start from a fresh copy."
  exit 1
fi

if [ ! -f "$PATCH" ]; then
  echo "Patch missing: $PATCH"
  exit 1
fi

mkdir -p "$ROOT/.build"
git init -q "$GAME"
git -C "$GAME" remote add origin "$PRET_URL"
echo ">> Fetching pret/pokefirered @ ${PRET_COMMIT:0:10}"
git -C "$GAME" fetch -q --depth 1 origin "$PRET_COMMIT"
git -C "$GAME" checkout -q FETCH_HEAD
echo ">> Applying $(basename "$PATCH")"
git -C "$GAME" apply --whitespace=nowarn "$PATCH"
for name in dailyred-direct-battle.patch dailyred-wheel-rom.patch; do
  echo ">> Applying $name"
  git -C "$GAME" apply --check "$ROOT/patches/$name"
  git -C "$GAME" apply --whitespace=nowarn "$ROOT/patches/$name"
done
echo "Done. Build with: scripts/build-rom-linux.sh"
