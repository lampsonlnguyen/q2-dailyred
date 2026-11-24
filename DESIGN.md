# Daily Red on Q2 Pod — design

## Verdict

Daily Red ships as a **standalone `.dailyred` card app** — a single C binary
that statically links the gpSP GBA core (libretro/gpsp @ `5819380`, plus
`patches/gpsp-q2.patch`) and carries its own minimal launcher — **not** as a
ROM package launched through q2-boy. q2-boy has no single-ROM launch mode, no
hook to inject the per-day seed into the ROM before boot, and no way for an
outside process to read the in-RAM `DAILYRED-STATUS1` block; both must happen
inside the process that owns the core. Shipping the ROM as a q2-boy library
entry would also let the daily rule be bypassed by launching the ROM
unpatched from the picker (seed 0 → title screen → vanilla seeding). The app
reuses q2-boy's proven pieces — `platform.c`/`platform.h`, `crt-q2.c`, the
16×17 Liberation font renderer, the libretro glue, the audio2/gameview
protocol, the atomic-write and battery-save helpers — with the browser,
itch.io, covers, multi-core loading, config editor and save states deleted,
and a Daily Red launcher in their place. The ROM is built from source only
(pret/pokefirered + the Daily Red patch) and is never committed, never
shipped in the repo, and never distributed in a release archive by this
project's scripts; the release pipeline builds it, and the user copies their
locally built ROM into the app folder.

## 1. Ground rules

- **Build from source only.** The ROM comes from pret/pokefirered at pinned
  commit `037335f4c725d7c9aecdac87066f2002b4bd7e14` plus
  `patches/pokefirered-dailyred.patch`, built with `make MODERN=1`
  (see §7). Never ask Lampson for a ROM, never use a retail FireRed ROM,
  never commit a `.gba`, `.gba.gz`, BIOS, or ROM bytes (enforced by
  `.gitignore` and by CI's `no-rom-artifacts` job).
- **AGPL-3.0** for this repo's own code (see `LICENSE`). Upstream
  attribution and licences are preserved per `ATTRIBUTION.md` — including
  the Pokémon fan-project disclaimer, which must appear in the README and
  in any release notes.
- **The game never saves.** There is no Save menu and no `SaveGame` call
  anywhere in the Daily Red patch; every boot runs a fresh new game. All
  persistence is the launcher's own stats file on the SD card (§5). The
  emulator's `.srm` is kept on q2-boy's rhythm anyway (harmless, and a
  stale `.srm` is the upstream-tested boot path) but carries nothing.
- **Plain C, not C++.** `crt-q2.c`'s `__start` runs no `.init_array`, so
  file-scope C++ globals would stay unconstructed (q2-pico8 precedent;
  see the `q2_card_app_startup` skill). The launcher, the logic core and
  the platform layer are all C. If C++ ever creeps in, the init array must
  be run manually as the first statement of `main()`.

## 2. Architecture

```
SD card                                     Q2 Pod 1.9.0+
/.mnt/mmc/.dailyred/                        ┌─────────────────────────┐
  q2app        name=Daily Red, game=1,     │ q2-dailyred (C, MIPS) │
  run          audio=2, gameview=1,        │  ┌──────────────────┐ │
  q2-dailyred  row=games, wifi=0           │  │ launcher UI      │ │
  dailyred.gba (user's local build)        │  │ date → seed →    │ │
                                           │  │ patch RAM copy → │ │
  /mnt/mmc/dailyred/  (data dir)           │  │ boot gpSP → poll │ │
  stats.json    wins/losses/streaks        │  │ STATUS1 → record │ │
  dailyred.gba.srm  (battery save)         │  └──────────────────┘ │
  practice-seed.txt (optional)             │  gpsp.a (static)      │
  enable-dynarec.txt (optional)            │  platform.c (fb/ALSA) │
  q2-dailyred.log                          └─────────────────────────┘
```

- **App folder** `/.dailyred/` (id `dailyred`, 8 chars, within the firmware's
  1–15 `[a-z0-9-]`): `q2app`, `run`, the binary `q2-dailyred`, and the ROM
  `dailyred.gba` (placed there by the user after building it locally).
  `run` is the q2-weather pattern: copy the binary to `/tmp`, start a
  restart-on-crash loop in the background, return at once (the firmware
  runs `run` with `system()` on the UI thread; a minute of block restarts
  the Q2). The loop's pid lives in `/tmp/q2-dailyred`; a TERM to it stops
  the app. Because the binary runs from `/tmp`, the app folder is a fixed
  path (`/mnt/mmc/.dailyred`), not derived from `/proc/self/exe` — this is
  a deliberate deviation from q2-boy, whose `run` wrapper was not in its
  checkout.
- **Data dir** `/mnt/mmc/dailyred` (non-hidden, alongside the hidden app
  folder — the same split as q2-boy's `/mnt/mmc/q2-boy`). Never shared with
  q2-boy: stats, saves and flag files must not mix with another app's.
- **Manifest** (`package/.dailyred/q2app`): `name=Daily Red`, `game=1`
  (full-screen card game), `audio=2` (audio2 swap protocol), `gameview=1`
  (Now Playing handoff + Return+Centre force-close), `row=games`
  (Library → Games), `wifi=0` (the app never uses the network — verify
  this key is honoured on the target firmware; q2-boy does not set it).
  The launcher copies q2-boy's caps check (`/tmp/q2-addon.caps`) and
  manifest rewrite: if the firmware lacks `audio2`, the `audio=` line is
  rewritten to `1` and the user is told to exit, back out of Games and
  re-enter so Q2 Pod re-reads it.
- **No firmware patch is needed** on Q2 Pod 1.9.0+. The old q2-boy audio /
  display bridge patches are obsolete and id-specific (`boy`); Daily Red
  uses only the generic `audio2`/`gameview` caps and the generic
  `/tmp/q2-<id>.dim` mechanism. v1 targets 1.9.0+ and omits the 1.8.8
  legacy audio bridge (the argv device path is still accepted, as in
  q2-boy, for the firmware's own audio-device launch).

## 3. Seed and ROM config injection

- **Date source:** `time(NULL)` + `localtime_r()` — the firmware owns the
  timezone (its own clock / network time sync), so the launcher needs no
  tzdata and no DST tables. The q2-utilities-app city/DST tables are
  deliberately **not** copied; only the pure-integer Hinnant civil-date
  helpers (`days_from_civil` / `civil_from_days`) are reused for streak
  arithmetic, so a DST change can never skip or repeat a day (the C
  equivalent of the website's midday-anchored `addDays`). The launcher
  displays the computed date and seed on its title screen, so a wrong Q2
  clock is visible instead of silently dealing a different team.
- **Seed:** FNV-1a over `dailyred:YYYY-MM-DD`, exactly `web/daily.js`:
  offset basis `0x811c9dc5`, prime `0x01000193`, 32-bit wrap, `hash || 1`
  (0 is the ROM's "no injected seed" sentinel). Verified against the real
  `web/daily.js` under Node — all six vectors agree, including
  `2026-11-24 → 3525968457` and `2026-11-25 → 3509190838`
  (`src/dailyred.c`, `tests/test_dailyred.c`, 132 host checks).
- **ROM config** (`DAILYRED-CONF-V2`, 28 bytes): at launch the ROM is read
  into RAM, scanned for the 16-byte magic (the offset is **never**
  hard-coded — it moves with every rebuild), and the working copy is
  patched with today's seed (u32 LE), current `wins` (u16 LE, saturated),
  current `battles` = wins+losses (u16 LE, saturated), `daily` = 1. The
  stored ROM file is never written; the patched copy is re-made from the
  pristine file on every launch. If the magic is not found, that is a
  fatal build error ("ROM has no Daily Red config. Rebuild it.") — never
  a guessed address. If gpSP reports `need_fullpath`, the patched copy is
  staged as `<data>/dailyred-patched.gba` and deleted on exit (the
  in-memory path is used otherwise).
- **Practice mode (v1, flag-file only):** a `practice-seed.txt` in the data
  dir holding a decimal seed boots with `daily=0`, wins/battles = 0, and
  records nothing — the web `?seed=` equivalent. There is no on-screen
  keyboard in v1, so the flag file is the practice path; it also gives the
  emulator smoke test a way to boot without the daily lock. Practice boots
  show a "Practice (seed N, not recorded)" row on the launcher.

## 4. Status polling — the one unproven link

- The game keeps `sStatus` in EWRAM: magic `DAILYRED-STATUS1`, seed (u32
  LE), phase byte at +20 (0 exploring / 1 battle / 2 result / 3 finished),
  result byte at +21 (0 none / 1 won / 2 lost). The launcher polls it after
  every `core->run()`: scan the core's `SYSTEM_RAM` once for the magic
  followed by the injected seed, cache the offset, then read the two
  bytes — exactly `web/game-link.js`, at frame rate instead of 2 Hz
  (the state machine debounces: `markBattleStarted` and `recordResult`
  are each applied once per day).
- **Status link — PROVEN 2026-11-24.** The `--smoke` harness
  (`q2-dailyred --smoke <rom> <datadir> [date]`, run under qemu-mipsel
  with the Q2 sysroot) boots the real locally built ROM, renders a first
  video frame at 59.728 fps, and finds `DAILYRED-STATUS1` at
  `SYSTEM_RAM+137432` with phase EXPLORING (0) and result NONE (0), for
  both 2026-11-24 (seed 3525968457) and 2026-11-25 (seed 3509190838).
  gpSP's `retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)` returns its
  `ewram` (256 KiB, `deps/gpsp/libretro/libretro.c`), so the scan works
  exactly as designed. What is still unproven is the *firmware* run: the
  launcher UI, physical/touch input, the FINISHED/result recording path
  (which needs a played battle, not just a boot), and audio — see §9.
- **Fallback if the link is absent at runtime:** the launcher sets
  `pending=today` at boot (instead of at battle start) so a mid-battle
  quit cannot silently bypass the rule, and says so on screen ("Status
  link unavailable: leaving a battle counts as a loss"). This is strictly
  harsher than the web rule (exiting while merely exploring also counts),
  so it is a fallback, not the design.

## 5. Stats and the daily state machine

- **Stats file** `<data>/stats.json` — the exact equivalent of the
  website's localStorage `dailyred-stats`, same field names and semantics:
  `wins`, `losses`, `winStreak`, `dayStreak`, `lastPlayed`
  (`"YYYY-MM-DD"` of the last finished battle), `lastResult`
  (`"won"`/`"lost"`), `pending` (date of a battle that started but hasn't
  finished). JSON is the simplest faithful choice; a minimal reader/writer
  in `dailyred.c` handles it with no JSON library (corrupt or partial
  JSON falls back to `EMPTY` defaults per field, like `loadStats()`).
  Every write is atomic: `<path>.tmp` + `fflush` + `fsync` + `rename`
  (the same helper q2-boy's `file_write` uses).
- **Transitions** (ports of `web/daily.js`, verified by the host tests):
  - `settleAbandonedGame` runs on **every launch before Play is offered**:
    a `pending` date different from `lastPlayed` is recorded as a **loss
    on the day it started**; a `pending` equal to `lastPlayed` is just
    cleared. Quitting or powering off mid-battle = loss, exactly as on
    the website. Exiting while merely exploring records nothing.
  - `markBattleStarted`: the moment phase first reaches BATTLE,
    `pending = today` and the file is saved. From that moment the day
    counts as played.
  - `recordResult`: idempotent (no-op if `lastPlayed` is already today —
    it matters because the launcher records on every poll after the
    result is known). Consecutive-day logic: `dayStreak` increments only
    if `lastPlayed` was yesterday, else 1; a win extends `winStreak` only
    on a consecutive win, a loss zeroes it. Totals are never reset.
  - Mid-battle restart detection: if phase drops below BATTLE with result
    NONE after the battle was seen, record a loss immediately and end the
    run (the web does this on reload; the launcher does it on detection).
  - On FINISHED: the web waits 1000 ms then closes the emulator; the
    launcher does the same, then returns to the launcher screen. Further
    input must not start a second battle.
  - The launcher refuses Play when `lastPlayed == today` and shows the
    result plus a countdown to local midnight (`HH:MM:SS`, refreshed once
    a second; at midnight the screen redraws and a new battle unlocks).
    Midnight is computed via `mktime()` on tomorrow 00:00 local, so a DST
    transition day counts its real length.
  - `startRun` re-loads stats at launch (another process may have played);
    the in-game record message ("Record: x win(s) in x battle(s)") comes
    from the launcher's stats injected at boot — the in-game counters are
    incremented by the map script, but only the launcher's file is
    authoritative.

## 6. What was deliberately left out

- **Save states: disabled in daily mode.** q2-boy's 10-slot `.stateNN` +
  `.latest-state` scheme is not inherited: a state taken mid-battle would
  let a lost daily battle be rewound. v1 has no state UI at all (no
  `state_io`, no slots); the decision is documented here, not silently
  inherited. If states ever return, they must be daily-mode-gated by
  design, not by UI omission.
- **No fast-forward in v1.** The walk to RED is ~7 steps; FF is
  unnecessary, and FF + the daily rule is an interaction nobody has
  thought through. `gpsp_frameskip` answers `disabled`; the FF touch
  overlay is not shipped. Revisit if the pre-battle walk feels slow.
- **No ROM browser, no covers, no itch.io, no multi-core loading, no
  control editor, no per-system profiles.** One system, one ROM, hard-coded
  q2-boy default controls. The `controls.cfg` editor and the cover
  cache (with its runtime `libpng12`/`libjpeg` dlopen) are deleted, not
  adapted.
- **No settings persistence in v1** (picture scale/filter are in-memory
  and reset each launch). Only the audio source (`audio-source.cfg`,
  game/music) and the dynarec flag persist, copied from q2-boy. If
  settings persistence is wanted, add a small config file; do not resurrect
  the `controls.cfg` format.
- **No on-screen keyboard.** Practice seeds come from the flag file (§3).

## 7. Build

- **ROM** (local only, never committed):
  `scripts/setup-linux.sh` (xPack ARM GCC 14.2.1-1.1, project-local) *or*
  `sudo apt install gcc-arm-none-eabi` → `scripts/fetch-pokefirered.sh`
  (pret/pokefirered @ `037335f` + patch, into `.build/pokefirered`) →
  `scripts/build-rom-linux.sh` (→ `.build/rom/dailyred.gba`, 16 MB, verified
  to contain `DAILYRED-CONF-V2`). The build script detects from
  `pokefirered/Makefile` whether `TOOLCHAIN` is a directory or a command
  prefix, so both toolchain routes work. `libpng-dev` (system) satisfies
  `gbagfx`; no static libpng build is needed on Linux.
- **Card app** (MIPS): `src/build-q2.sh` — fetches libretro/gpsp @
  `5819380` into `deps/gpsp`, applies `patches/gpsp-q2.patch`, builds
  `gpsp.a` with the q2-boy flags (`clang --target=mipsel-linux-gnu`,
  `--sysroot=$SYSROOT`, `-march=mips32r2 -mabi=32 -mfp64 -mnan=2008
  -mabs=2008`, `platform=mips32 STATIC_LINKING=1 TARGET=gpsp.a
  FRONTEND_SUPPORTS_RGB565=1`), compiles `crt-q2.o dailyred-launcher.o
  dailyred.o platform.o`, and links with `ld.lld -m elf32ltsmip -e
  __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 --image-base=0x400000`
  against the sysroot's glibc 2.28 / ALSA libs. One deliberate deviation
  from q2-boy's flags: `-nostdlibinc -isystem $SYSROOT/usr/include` is
  added to both the gpsp and frontend compiles. Without it, clang
  auto-detects a host `gcc-mipsel-linux-gnu` install (present on this VM
  since the toolchain install) and searches its newer glibc headers
  *before* the sysroot's, failing with fortify-declarator errors in the
  host `unistd.h`. `-nostdlibinc` drops the detected GCC include dir
  while keeping clang's builtin headers; the two `-isystem` paths then
  supply glibc 2.28 only (q2-pico8 uses the blunter `-nostdinc` for the
  same reason). Output:
  `dist/.dailyred/` (`q2app`, `run`, `q2-dailyred`). The ROM is **not**
  copied in by the script; the user copies `.build/rom/dailyred.gba` to
  `dist/.dailyred/dailyred.gba` before putting the folder on the card.
- **CI** (`.github/workflows/ci.yml`): host logic tests + script syntax
  checks + launcher syntax check (stub `libretro.h`, host `cc`) +
  no-ROM-artifacts guard run on every push/PR. The MIPS build job runs
  `src/build-q2.sh --allow-skip`, which exits 0 with `SKIP:` when the Q2
  sysroot is absent — GitHub runners have clang but no sysroot, so the
  MIPS job proves the script's gating, not the binary. A real MIPS build
  happens on the dev VM. CI never uploads a ROM artifact.
- **Host tests:** `sh tests/run_tests.sh` — `cc` on `src/dailyred.c` +
  `tests/test_dailyred.c` (132 checks, 0 failures as of 2026-11-24),
  covering the seed vectors, civil-date round-trips and DST-boundary
  steps, ROM-config patching (including saturation and the missing-magic
  fatal), status-block scanning (including the wrong-seed and
  CONF-vs-STATUS magic distinction), and the full stats state machine.
  The launcher itself (`dailyred-launcher.c`) is MIPS-only; it is
  syntax-checked on the host against a stub `libretro.h` (CI does the
  same), and its real compile is `src/build-q2.sh`.

## 8. Controls, screen, audio

- **Physical** (q2-boy defaults, both GPIO raw codes and the firmware
  `'k'` fallback — the latter decoded only when the GPIO grab failed):
  Return=Up (103/170), Play/Pause=Down (108/171), Previous=Left (165/222),
  Next=Right (163/223), Centre=A (116/218). Held buttons do not
  auto-repeat; a tap is exactly one frame's worth of input
  (`pressed_this_frame`, cleared after each `core->run()`).
- **Menus:** Centre confirms, Return goes back, the wheel steps
  (21 units/tick, ±84 clamp, 300 ms idle reset; negative delta steps down,
  positive steps up — q2-boy's sign). Prev/Next do nothing in menus.
  A tap on a menu/launcher row activates it.
- **Touch overlays** (logical 375×320 canvas; visible set for GBA only):
  L (29,5,72), R (274,5,72), B (297,244,60), Menu (34,272,62),
  Select (126,281,56), Start (192,281,56), all 70% opacity. X/Y are
  suppressed on GBA (SNES-only). The D-pad, A, A+B, FF, Save and Load
  overlays are not shipped (D-pad and A are physical; FF/save/load are
  v1 omissions, §6). An edge touch swiped inward ≥36 px toggles overlay
  visibility. The Menu overlay opens the in-game menu: Resume, Sound
  (Game/Q2 Music), Q2 Now Playing (gameview only), Picture
  (Fit/Crop/Stretch/Integer + Sharp/Nearest), Exit to launcher.
- **Screen:** logical canvas 375×320, rotated into the physical 320×375
  panel by `platform.c` (copied verbatim from q2-boy). GBA frame 240×160
  RGB565 → 32-bit XRGB. FIT (default) is 375×250 centred with 35 px bars
  top and bottom; Integer is 1× (240×160 — 2× would be 480 wide);
  Crop fills 375×320 (loses top/bottom); Stretch is non-uniform.
  Filter: Sharp Bilinear (default) or Nearest.
- **Audio:** `audio=2` protocol copied from q2-boy — the app writes
  `game`/`music` to `/tmp/q2-dailyred.audio`, the firmware answers with
  the ALSA device line (5 s timeout, then a status message). ALSA via
  `platform.c` (`plughw:0,0` default, `plug:bluealsa` / `usb` routes,
  soft volume). The core is gated by `GET_AUDIO_VIDEO_ENABLE`
  (audio only while in-game, not while music mode). Now Playing:
  the app writes `now` to `/tmp/q2-dailyred.view` after pausing emulation
  and handing audio back; the firmware replaces it with `open`.
  Return+Centre held 1.2 s force-closes (gameview chord) unless
  `/tmp/q2-dailyred.quit` exists. **The emulator has no audio** — audio is
  untestable in the harness and must be verified on real hardware.
- **Input socket:** `/tmp/q2-dailyred.input.sock` (renamed from q2-boy's;
  the only intentional change to the copied `platform.c`).
- **Logging:** `<data>/q2-dailyred.log`, rotated past 128 KiB, with
  `launch_stage` fatal-signal reporting (menu / ROM load / save RAM load /
  first frame / gameplay / save-exit / ALSA). This is the only on-device
  diagnostic channel.

## 9. Unknowns and blockers

1. **SYSTEM_RAM status link — proven at core level, firmware run still
   pending** (§4). The qemu-mipsel `--smoke` run proves boot, video, and
   the status scan. Still to prove in the Q2 Pod firmware emulator
   (q2emu.py): the launcher UI rendering, physical/touch input, the
   Play → battle → FINISHED → record-result path with scripted input, the
   already-played screen + countdown, and the abandonment path. Audio
   cannot be tested in the emulator at all (no audio) and needs real
   hardware.
2. **MIPS toolchain not installed on this VM yet** (parallel work):
   clang/lld and the sysroot reinstall. The sysroot itself
   (`~/.cache/q2/sysroot`, glibc 2.28) survives; `src/build-q2.sh`
   fails closed with `SKIP:` until the toolchain lands.
3. **`run` wrapper provenance.** q2-boy's release `run` was not in its
   checkout; ours follows q2-weather's proven pattern and the
   `patch/addon.c` contract in q2-pod's `docs/internals.md` (pid file,
   background loop, return at once). Verify against a real q2-boy release
   ZIP when one is handy; the contract, not the precedent, is what matters.
4. **Interpreter performance for FireRed on-device is unmeasured.**
   FireRed is a demanding GBA title; the interpreter is the default and
   the dynarec is opt-in via `<data>/enable-dynarec.txt` (first byte
   `'1'`). Measure emulated fps + audio underruns in the emulator, then
   on real hardware; do not promise full speed until measured.
5. **Q2 clock / timezone trust.** The daily boundary is local midnight and
   the seed is a hash of the local date. If the Q2's timezone is wrong (or
   unset / UTC), "today" — and therefore the team — will be wrong, with no
   in-app way to detect the intended zone. The launcher shows the computed
   date so a wrong clock is visible; a "date looks wrong?" hint (e.g.
   device date far from the build date) was considered and rejected as a
   weak signal — do not over-engineer it.
6. **BIOS-less HLE.** gpSP runs FireRed with its internal HLE BIOS; no
   BIOS file is shipped or needed (same as q2-boy and the mGBA web build).
   Confirm in the emulator smoke test — if gpSP ever required a BIOS, that
   would be a distribution blocker.
7. **ROM size / load time:** 16 MB read fully into RAM at launch (within
   q2-boy's 32 MB limit); plus gpSP's `ROM_BUFFER_SIZE=8` translation
   cache. Fine on paper on the 128 MB device, but watch peak RSS
   alongside the canvas buffers.
8. **Licence interaction (flagged, not blocking scaffolding):** this
   repo's own code is AGPL-3.0; the q2-boy-derived files are
   GPL-2.0-or-later; the linked binary containing gpSP is distributed
   under GPL version 2 (q2-boy's wording). The AGPL-3.0/GPL-2.0-only
   combination needs a decision at release time — the same question
   q2-boy answers by being GPL-2.0-or-later throughout.
9. **`wifi=0` in the manifest** is set but unverified on the target
   firmware (q2-boy does not set it).
10. **Practice mode + the stats file format** are product decisions
    (flag-file practice, JSON stats mirroring `localStorage`) — recorded
    here so a later UI can change them deliberately.

## 10. File map

| Path | What it is |
|---|---|
| `DESIGN.md` | This file. |
| `README.md` | Build / install / play / daily rules / licences. |
| `ATTRIBUTION.md` | Upstream attributions and licences. |
| `LICENSE` | AGPL-3.0 (this repo's own code). |
| `.gitignore` | ROMs/BIOS/saves, `.build/`, `.toolchain/`, `deps/`, `build/`, `dist/`. |
| `patches/pokefirered-dailyred.patch` | Daily Red's 33-file patch (from upstream dailyred). |
| `patches/gpsp-q2.patch` | q2-boy's gpSP patch (verbatim). |
| `scripts/fetch-pokefirered.sh` | Fetch pret/pokefirered @ `037335f` + apply the patch. |
| `scripts/setup-linux.sh` | xPack ARM GCC 14.2.1-1.1, project-local (Linux port of setup-macos.sh). |
| `scripts/build-rom-linux.sh` | Build the ROM → `.build/rom/dailyred.gba` (never committed). |
| `src/dailyred.h`, `src/dailyred.c` | Seed, dates, stats, ROM patch, status scan (host-testable). |
| `src/dailyred-launcher.c` | Q2 entry point + gpSP glue + launcher/menu UI. |
| `src/platform.c`, `src/platform.h` | q2-boy, verbatim except the input-socket rename. |
| `src/crt-q2.c` | q2-boy, verbatim (NaN2008 `__start`, no init array). |
| `src/font.h`, `src/FONT-LICENSE` | q2-boy's 16×17 Liberation Sans renderer (SIL OFL 1.1). |
| `src/build-q2.sh` | MIPS cross-build → `dist/.dailyred/`. |
| `package/.dailyred/q2app`, `run` | Manifest + firmware launcher wrapper. |
| `tests/test_dailyred.c`, `tests/run_tests.sh` | Host unit tests for `dailyred.c`. |
| `.github/workflows/ci.yml` | Host tests, syntax checks, no-ROM guard, MIPS build (skip-if-no-sysroot). |
| `.build/` (untracked) | Local scratch: `pokefirered/`, `rom/`, `upstream-dailyred/` (analysis source). |
