# Attribution

This repo's own code (the launcher, the logic core, the build scripts, the
docs) is licensed under the GNU Affero General Public License v3.0 — see
`LICENSE`. The work below is by others and is preserved here under its own
terms. Nothing in this repo contains a built ROM, a retail ROM, a BIOS, or
Nintendo game content as binary data: the game is built from source by the
user (see README.md).

## Daily Red (upstream)

- **Daily Red** by amoi-builds — https://github.com/amoi-builds/dailyred
  — the daily-battle concept, the FireRed patch, the website logic
  (`web/daily.js`, `web/rom-config.js`, `web/game-link.js`) that
  `src/dailyred.c` ports, and the seed/stats/state-machine design in
  `DESIGN.md`. The patch is vendored as
  `patches/pokefirered-dailyred.patch` (33 files, from the upstream repo).
- **Fan-project disclaimer (must be carried over):** Daily Red is a free,
  unofficial fan project, not affiliated with or endorsed by Nintendo,
  Creatures Inc., GAME FREAK inc. or The Pokémon Company. Pokémon and all
  related names, characters and game content are © Nintendo, Creatures
  Inc. and GAME FREAK inc.

## pret/pokefirered

- **pret/pokefirered** — https://github.com/pret/pokefirered — the
  community decompilation of Pokémon FireRed that the ROM is built from
  (pinned commit `037335f4c725d7c9aecdac87066f2002b4bd7e14`). Fetched at
  build time by `scripts/fetch-pokefirered.sh`; never vendored, never
  distributed. The decompilation is Nintendo-derived content: source-only,
  build locally, same constraint as upstream.

## gpSP (GBA core)

- **libretro/gpsp** — https://github.com/libretro/gpsp — the GBA emulator
  core, statically linked into the card app. Built from pinned commit
  `5819380c2ffb0900219d700a382ee68c464ebb99` with
  `patches/gpsp-q2.patch` (q2-boy's patch, vendored verbatim).
- The combined gpSP build is distributed under **GPL version 2**
  (q2-boy's wording). The core is GPL-2.0; see
  https://github.com/libretro/gpsp for its licence and source.
- gpSP runs FireRed with its internal HLE BIOS. No BIOS file is shipped,
  needed, or distributed.

## q2-boy (Pocket Boy)

- **q2-boy** by lampsonlnguyen — https://github.com/lampsonlnguyen/q2-boy
  (Pocket Boy v0.4.2) — the architecture this app copies: the gpSP
  static-link design, the MIPS build flags and link line, the libretro
  glue, the audio2/gameview protocol, the battery-save and atomic-write
  helpers, and the control/screen defaults.
- Files copied from q2-boy, under **GPL-2.0-or-later** (their SPDX headers
  say so; q2-boy's README: "The frontend is licensed under
  GPL-2.0-or-later, with the combined gpSP build distributed under GPL
  version 2"):
  - `src/platform.c`, `src/platform.h` — framebuffer/ALSA/input layer.
    Verbatim except one rename: the firmware input socket is
    `/tmp/q2-dailyred.input.sock` instead of `/tmp/q2-boy.input.sock`.
  - `src/crt-q2.c` — verbatim (NaN2008 `__start`, no init array).
  - `src/font.h`, `src/FONT-LICENSE` — the 16×17 glyph renderer.
  - `patches/gpsp-q2.patch` — verbatim.
  - `src/dailyred-launcher.c` adapts (does not copy wholesale) q2-boy's
    `src/main.c` glue: environment/video/audio/input callbacks,
    `file_write`/`read_text`/`has_cap`, manifest-audio and audio2
    polling, battery save, ROM loading, scaling/drawing, and the main
    loop. The browser, itch.io, covers, multi-core loading, config
    editor and save states were deleted, not adapted.
  - `package/.dailyred/run` follows q2-weather's `run` pattern (same
    firmware contract); see `q2-pod` below for the contract source.

## Font

- **Liberation Fonts** (Liberation Sans) — https://github.com/liberationfonts
  — the 16×17 glyphs in `src/font.h`, under the **SIL Open Font License
  1.1** (full text in `src/FONT-LICENSE`). Digitized data copyright
  © 2010 Google Corporation; © 2012 Red Hat, Inc.

## Q2 Pod firmware

- **Q2 Pod** by DiamondBond — https://github.com/DiamondBond/q2-pod —
  the firmware this card app targets (1.9.0+), and the card-app contract
  (`q2app` manifest, `run` wrapper, `audio2`/`gameview` caps,
  `/tmp/q2-<id>.audio` / `.view` / `.input.sock`) documented in its
  `docs/internals.md`. No firmware patch is needed or shipped; the old
  q2-boy bridge patches are obsolete on 1.9.0+ and were not copied.

## Licence interaction (flagged)

This repo is AGPL-3.0; the q2-boy-derived files are GPL-2.0-or-later; the
linked binary containing gpSP is distributed under GPL version 2 (q2-boy's
wording). The AGPL-3.0/GPL-2.0-only combination needs a decision at release
time — see DESIGN.md §9, item 8. Do not relicense any vendored file by
editing its header: the headers above are the licence.
