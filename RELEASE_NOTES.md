# Daily Red for Q2

## Installation

Download the Q2 application archive, extract `.dailyred`, and build the matching ROM from the source code at this release's tag. Follow the [User Guide](https://github.com/lampsonlnguyen/q2-dailyred/blob/main/USER_GUIDE.md) for full Windows WSL/Linux instructions, controls and troubleshooting.

ROM-only build (after installing dependencies and checking out the matching release tag):

```bash
bash scripts/fetch-pokefirered.sh
bash scripts/build-rom-linux.sh
```

Copy `.build/rom/dailyred.gba` into `.dailyred/`, then copy that folder to the SD card root. Open Library → Games → Daily Red. No Q2 sysroot is required when using the prebuilt application. No ROM or BIOS is included.

Daily Red is an unofficial fan project, not affiliated with Nintendo, Creatures Inc., GAME FREAK inc. or The Pokémon Company.
