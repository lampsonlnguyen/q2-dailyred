#!/bin/sh
# Host unit tests for the Daily Red launcher logic (src/dailyred.c).
# No Q2 toolchain, ROM, or emulator needed: plain host cc.
# Usage: sh tests/run_tests.sh   (from the repo root)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$ROOT/src" \
    "$ROOT/src/dailyred.c" "$ROOT/tests/test_dailyred.c" \
    -o "$TMP/test_dailyred"
"$TMP/test_dailyred"
