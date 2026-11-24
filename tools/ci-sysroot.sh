#!/bin/sh
# Rebuild the Q2 Pod MIPS sysroot in a clean GitHub Actions runner.
# Firmware runtime libraries come from DiamondBond/q2-pod; development
# headers come from SHA256-pinned Debian Buster mipsel packages.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SYSROOT=${SYSROOT:-"$HOME/.cache/q2/sysroot"}
mkdir -p "$SYSROOT/lib" "$ROOT/.build"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
Q2_POD_RELEASE=${Q2_POD_RELEASE:-1.9.2}
echo "Downloading Q2 Pod runtime $Q2_POD_RELEASE"
curl -fL --retry 3 "https://github.com/DiamondBond/q2-pod/releases/download/$Q2_POD_RELEASE/update.tar" -o "$TMP/update.tar"
tar -xOf "$TMP/update.tar" recovery-update/rootfs.squashfs > "$TMP/rootfs.squashfs"
while read -r sum path; do
  curl -fsSL --retry 3 "https://archive.debian.org/debian/pool/main/$path" -o "$TMP/package.deb"
  echo "$sum  $TMP/package.deb" | sha256sum -c --quiet
  (cd "$TMP" && ar x package.deb data.tar.xz && tar -xJf data.tar.xz -C "$SYSROOT" ./usr/include)
  rm -f "$TMP/package.deb" "$TMP/data.tar.xz"
done <<'PACKAGES'
e28bac8637e98d78045f0fd05613ed2ab298fb1aa6387633b6d609119774d590 g/glibc/libc6-dev_2.28-10+deb10u1_mipsel.deb
5f6ca1c34bb326b38fa44838b1ed051c33a2591bf7bfa26276e54fc85c00427c l/linux/linux-libc-dev_4.19.249-2_mipsel.deb
729cf99def60974b1c893c21dbf242fded3b8c34547e547458c33fd48a41e3e7 a/alsa-lib/libasound2-dev_1.1.8-1_mipsel.deb
PACKAGES
gnu="$SYSROOT/usr/include/mipsel-linux-gnu/gnu"
cp "$gnu/stubs-o32_hard.h" "$gnu/stubs-o32_hard_2008.h"
unsquashfs -q -n -d "$TMP/root" "$TMP/rootfs.squashfs" lib usr/lib > /dev/null
for name in ld-linux-mipsn8.so.1 libc.so.6 libpthread.so.0 libm.so.6 libdl.so.2 librt.so.1 libgcc_s.so.1; do
  test -f "$TMP/root/lib/$name" || { echo "Missing Q2 runtime: $name" >&2; exit 1; }
  cp -L "$TMP/root/lib/$name" "$SYSROOT/lib/"
done
for name in libasound.so.2; do
  test -f "$TMP/root/usr/lib/$name" || { echo "Missing Q2 runtime: $name" >&2; exit 1; }
  cp -L "$TMP/root/usr/lib/$name" "$SYSROOT/lib/"
done
echo "Created Q2 Pod sysroot: $SYSROOT"
