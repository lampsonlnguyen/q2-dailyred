# Daily Red for Q2 — User Guide

## 1. Download and install

Requires a Shanling Q2 running Q2 Pod 1.9.0 or newer.

1. Open the [Releases page](https://github.com/lampsonlnguyen/q2-dailyred/releases) and download the Q2 application archive for a release containing the Q2-specific controls.
2. Extract the archive and locate the hidden `.dailyred` folder (enable hidden files if necessary).
3. Build the matching game ROM using the instructions below.
4. Place `dailyred.gba` inside the extracted `.dailyred` folder.
5. Copy the complete folder to the root of the SD card, not inside Music or Games.
6. Safely eject the card, then open Library → Games → Daily Red.

The ROM is not distributed with the application. No retail FireRed ROM or BIOS is required by this build.

## 2. Patch and build the ROM (Windows WSL / Ubuntu)

The patches modify FireRed **source code**, not an existing .gba file. You do not need a binary ROM patcher. Use the source code matching your application release.

On Windows, if Ubuntu WSL is not already installed, open PowerShell and run:

```powershell
wsl --install -d Ubuntu
```

Complete Ubuntu setup (restart Windows if prompted). Open an Ubuntu terminal. Linux users can start here directly.

### Install dependencies

```bash
sudo apt update
sudo apt install -y git build-essential libpng-dev pkg-config gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
```

Alternatively, install the project-local ARM compiler using `bash scripts/setup-linux.sh` after cloning (requires curl).

### Obtain the matching source

```bash
git clone https://github.com/lampsonlnguyen/q2-dailyred.git
cd q2-dailyred
git tag --list
```

Check out the tag shown on the release you downloaded (replace RELEASE_TAG with its actual tag):

```bash
git checkout RELEASE_TAG
```

These instructions apply to releases containing the automatic Q2 ROM patch integration.

### Apply patches and compile

```bash
bash scripts/fetch-pokefirered.sh
bash scripts/build-rom-linux.sh
```

The fetch script downloads the pinned FireRed source and applies these patches in order:

| Patch | Purpose |
|---|---|
| `pokefirered-dailyred.patch` | Daily Red gameplay and launcher integration |
| `dailyred-direct-battle.patch` | Automatically starts the battle |
| `dailyred-wheel-rom.patch` | Click-wheel-friendly battle selection |

The output is `.build/rom/dailyred.gba` (expected size: 16 MiB). Verify the launcher interface markers:

```bash
ls -lh .build/rom/dailyred.gba
grep -aob 'DAILYRED-CONF-V2' .build/rom/dailyred.gba
grep -aob 'DAILYRED-STATUS1' .build/rom/dailyred.gba
```

Each grep command should print an offset. On Windows, run `explorer.exe .build/rom` from Ubuntu to open the output folder and copy the ROM into the extracted application folder.

You do **not** need the MIPS compiler or Q2 sysroot if you already downloaded the compiled Q2 application.

### Final SD card layout

```text
SD card root/
└── .dailyred/
    ├── q2app
    ├── run
    ├── q2-dailyred
    └── dailyred.gba
```

Do not rename the files or create an extra nested .dailyred directory.

## 3. Playing

- **Play today's battle:** one official battle per local calendar day. The battle begins automatically. Quitting an official battle after it starts counts as a loss.
- **Practice:** unlimited attempts, including after completing the official daily battle. A new team is generated internally for each launch; practice never affects your official record or streaks.
- **Exit:** close the launcher.

| Control | Action |
|---|---|
| Click wheel | Cycle menu and battle choices |
| Centre | Confirm / A |
| Return | Back / B |
| Swipe inward from either screen edge | Open pause menu |
| Previous / Next / Play-Pause | Not mapped to GBA controls |

The pause menu offers Resume, Picture and Exit to launcher. Game audio is disabled to allow Q2 music playback.

## 4. Updates and troubleshooting

- **No ROM found:** verify the exact path `/.dailyred/dailyred.gba` on the SD card.
- **ROM has no Daily Red config:** rebuild from the matching source tag, not a retail ROM.
- **Patch fails or source directory exists:** preserve any local changes by moving the old scratch checkout aside (choose a new backup name if needed), then rerun the fetch and build:
  ```bash
  mv .build/pokefirered .build/pokefirered-backup
  bash scripts/fetch-pokefirered.sh
  bash scripts/build-rom-linux.sh
  ```
- **Wrong date or team:** check the Q2's local date and time settings.
- **Game fails to launch:** check that all four files exist and report the firmware version and `dailyred/q2-dailyred.log` from the SD card in a [GitHub issue](https://github.com/lampsonlnguyen/q2-dailyred/issues).
- **Updating:** replace the application files and rebuild the matching ROM. Back up `dailyred/stats.json` from the SD card; retain the separate non-hidden `dailyred` data directory to preserve your record.

## 5. Building the application yourself (optional)

With clang, lld and a compatible Q2 sysroot at `~/.cache/q2/sysroot` (or set SYSROOT), run:

```bash
bash scripts/patch-rom-to-use-wheel.sh
```

This builds the ROM, runs host tests, builds the Q2 application and stages the local package in `dist/.dailyred/`. See [README](README.md) and [DESIGN](DESIGN.md) for development details. Never commit or distribute the generated ROM or save files.

Daily Red is an unofficial fan project, not affiliated with Nintendo, Creatures Inc., GAME FREAK inc. or The Pokémon Company. See [ATTRIBUTION.md](ATTRIBUTION.md).
