#!/usr/bin/env bash
# Build the Q2-specific ROM and launcher using repository-local sources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [ ! -d .build/pokefirered/.git ]; then
  bash scripts/fetch-pokefirered.sh
else
  for name in dailyred-direct-battle.patch dailyred-wheel-rom.patch; do
    patch="$ROOT/patches/$name"
    if git -C .build/pokefirered apply --reverse --check "$patch" 2>/dev/null; then
      continue
    fi
    git -C .build/pokefirered apply --check "$patch"
    git -C .build/pokefirered apply "$patch"
  done
fi
bash scripts/build-rom-linux.sh
sh tests/run_tests.sh
sh src/build-q2.sh
grep -aq DAILYRED-CONF-V2 .build/rom/dailyred.gba
grep -aq DAILYRED-STATUS1 .build/rom/dailyred.gba
grep -qx audio=0 dist/.dailyred/q2app
cp .build/rom/dailyred.gba dist/.dailyred/dailyred.gba
echo "Local SD card package: $ROOT/dist/.dailyred/"
