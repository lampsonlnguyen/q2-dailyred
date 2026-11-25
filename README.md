# q2-dailyred

Battle RED with 100 levels Pokémon every day. A port of
[Daily Red](https://github.com/amoi-builds/dailyred) to Q2 Pod: one battle
per day against RED on a snowy summit, with the day's six random level-100
Gen 1–3 Pokémon — the same team for everyone that day, like Wordle.

Every battle runs on the real FireRed engine: the game is the
[pret/pokefirered](https://github.com/pret/pokefirered) decompilation with
Daily Red's source patch, built into a GBA ROM and played on the Q2 through
the gpSP core (the q2-boy GBA core). Nothing about battles is reimplemented
in the launcher.

> **Fan project.** Daily Red is a free, unofficial fan project, not
> affiliated with or endorsed by Nintendo, Creatures Inc., GAME FREAK inc.
> or The Pokémon Company. Pokémon and all related names, characters and
> game content are © Nintendo, Creatures Inc. and GAME FREAK inc.

## Downloads and user guide

- [GitHub Releases](https://github.com/lampsonlnguyen/q2-dailyred/releases)
- [User Guide — installation, ROM patching, controls and troubleshooting](USER_GUIDE.md)

## How it works

```
pret/pokefirered @ 037335f + Daily Red patch + Q2 ROM patches
  ── make MODERN=1 ──▶ dailyred.gba (16 MB, built locally, never committed)
Q2 launcher ──▶ today's local date → FNV-1a seed → patch a RAM copy of the ROM
  ──▶ gpSP boots it → the game generates today's team → RED battle starts automatically
  ──▶ launcher polls the game's RAM status block → records win/loss in stats.json
```

- **One battle per day.** The seed is a hash of the local date, so everyone
  gets the same team on the same day. Once you finish (or quit mid-battle),
  the day is done: the launcher shows the result and a countdown to local
  midnight.
- **Quitting mid-battle counts as a loss.** The day counts as played from
  the moment RED's battle starts — same as the website.
- **Your record lives on the SD card** (`/mnt/mmc/dailyred/stats.json`):
  wins, losses, win streak, day streak. The game itself never saves.
- **Practice mode:** select Practice at any time, even after completing the daily battle. Each launch generates a new seed internally; practice boots with `daily=0` and records nothing.

See [DESIGN.md](DESIGN.md) for the full design (seed, stats state machine,
controls, screen, audio, and the list of unknowns).

## Build

The Q2-specific launcher changes are integrated into the application source. The fetch script applies the upstream Daily Red patch followed by the direct-battle and wheel-navigation ROM patches. For a one-command local build after installing both toolchains, run `bash scripts/patch-rom-to-use-wheel.sh`. It also supports an existing Daily Red source checkout and packages the locally built ROM in `dist/.dailyred/`. No Windows-specific paths are required.

You need git, a Linux host, and (for the card app) a Q2 sysroot. No ROM is
downloaded or used anywhere in this process — the ROM is built from source.

```sh
# 1. ARM toolchain for the ROM — pick ONE:
scripts/setup-linux.sh              # xPack GCC 14.2.1, project-local, no sudo
#   ... or: sudo apt install gcc-arm-none-eabi   (system GCC 13.2, simpler)

# 2. Fetch the decompilation and apply Daily Red and Q2 ROM patches (local scratch only)
scripts/fetch-pokefirered.sh        # pret/pokefirered @ 037335f → .build/pokefirered

# 3. Build the ROM (16 MB, verified to contain the DAILYRED-CONF-V2 magic)
scripts/build-rom-linux.sh          # → .build/rom/dailyred.gba

# 4. MIPS toolchain for the card app (one time; see DESIGN.md §8):
#    clang + ld.lld, and a Q2 sysroot at ~/.cache/q2/sysroot
sh src/build-q2.sh                  # → dist/.dailyred/ (q2app + run + q2-dailyred)

# 5. Put the locally built ROM into the card package (NOT done by the build)
cp .build/rom/dailyred.gba dist/.dailyred/dailyred.gba
```

Host logic tests (seed, stats, ROM patch, status scan) need only a host `cc`:

```sh
sh tests/run_tests.sh
```

## Install

Copy `dist/.dailyred/` to the SD card root as `/.dailyred/` (so the card
holds `/.dailyred/q2app`, `/.dailyred/run`, `/.dailyred/q2-dailyred` and
`/.dailyred/dailyred.gba`). Then open Library → Games → **Daily Red** on
the Q2 (Q2 Pod 1.9.0+).

The first launch creates `/mnt/mmc/dailyred/` (stats, saves, log). If the
ROM is missing, the launcher says so instead of failing.

## Play

| Q2 button | In battle | In menus |
|---|---|---|
| Return | B / Back | Back |
| Play/Pause | Not mapped to GBA | — |
| Previous / Next | Not mapped to GBA | — |
| Centre | A / Confirm | Confirm |
| Wheel | Cycle battle choices | Step (21/tick) |

Swipe inward from either screen edge to open Pause (Resume / Picture / Exit to launcher). Game audio is disabled so Q2 music can continue.

- The battle starts automatically on entering the summit. Losing does not white you out.
- After the closing messages the game fades to black; the launcher returns
  to the title screen after a second.
- The launcher shows the local date; seeds are generated internally.

## Daily rules

- One battle per day, per the Q2's local date.
- The day counts as played the moment RED's battle starts. Leaving
  mid-battle — quitting, powering off, or the game restarting — is a loss.
- Win and day streaks break when a day is missed. Totals never reset.
- Practice runs are never recorded and never lock
  the daily battle.

## Licences

This repo's own code is AGPL-3.0 (`LICENSE`). It builds on Daily Red
(upstream), pret/pokefirered, libretro/gpsp, q2-boy and Q2 Pod — see
[ATTRIBUTION.md](ATTRIBUTION.md) for the full list, including the
GPL-2.0-or-later files copied from q2-boy and the GPL version 2 terms for
the combined gpSP binary.

**Never commit a ROM, BIOS, or save file.** `.gitignore` and CI enforce it:
a `.gba`, `.gba.gz`, BIOS or save file in the repo is a failed build.
