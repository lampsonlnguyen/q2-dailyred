(
  set -euo pipefail
  cd ~/github/q2-dailyred

  downloads=/mnt/c/Users/Lam/Downloads
  source_dir=.build/pokefirered
  dest=/mnt/c/Users/Lam/Music/.dailyred

  apply_once() {
    local checkout=$1 patch=$2
    if git -C "$checkout" apply --reverse --check "$patch" 2>/dev/null; then
      printf 'Already applied: %s\n' "${patch##*/}"
    else
      git -C "$checkout" apply --check "$patch"
      git -C "$checkout" apply "$patch"
    fi
  }

  for name in dailyred-direct-battle.patch dailyred-wheel-rom.patch dailyred-wheel-app.patch; do
    if [ ! -f "$downloads/$name" ]; then
      printf 'Missing patch: %s\n' "$downloads/$name" >&2
      exit 1
    fi
  done

  [ -d "$source_dir" ] || bash scripts/fetch-pokefirered.sh

  apply_once "$source_dir" "$downloads/dailyred-direct-battle.patch"
  apply_once "$source_dir" "$downloads/dailyred-wheel-rom.patch"
  apply_once . "$downloads/dailyred-wheel-app.patch"

  bash scripts/build-rom-linux.sh 2>&1 | tee /tmp/dailyred-build.log
  sh tests/run_tests.sh
  sh src/build-q2.sh 2>&1 | tee /tmp/dailyred-q2-build.log

  rom=.build/rom/dailyred.gba
  app=dist/.dailyred
  grep -aob 'DAILYRED-CONF-V2' "$rom"
  grep -aob 'DAILYRED-STATUS1' "$rom"
  grep -qx 'audio=0' "$app/q2app"
  test -s "$app/q2-dailyred"
  test -s "$app/run"

  backup=$(mktemp -d "$dest.before-wheel.XXXXXX")
  for name in dailyred.gba q2-dailyred q2app run; do
    if [ -f "$dest/$name" ]; then
      cp -p -- "$dest/$name" "$backup/$name"
    fi
  done

  cp -- "$rom" "$dest/dailyred.gba"
  cp -- "$app/q2-dailyred" "$app/q2app" "$app/run" "$dest/"
  cmp -- "$rom" "$dest/dailyred.gba"
  for name in q2-dailyred q2app run; do
    cmp -- "$app/$name" "$dest/$name"
  done

  printf '\nVerified Windows files in %s\nBackup: %s\n' "$dest" "$backup"
  sha256sum "$dest/dailyred.gba" "$dest/q2-dailyred"
)