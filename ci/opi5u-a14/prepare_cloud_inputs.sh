#!/usr/bin/env bash
# One-time host upload inputs for GitHub Actions. Does NOT build or flash.
set -euo pipefail
TOP="${1:-$HOME/android/A14}"
TOP="$(realpath "$TOP")"
cd "$TOP"
DEST="$TOP/out/ota-kernel/cloud-inputs"
BASE="out/ota-kernel/cloud-baseline-target-files.zip"
CLANG="prebuilts/clang/host/linux-x86/clang-r487747c"
for f in "$BASE" "$CLANG/bin/clang" \
    "out/host/linux-x86/bin/ota_from_target_files" \
    "out/host/linux-x86/bin/mkbootimg" \
    "build/make/target/product/security/testkey.pk8" \
    "build/make/target/product/security/testkey.x509.pem"; do
  test -f "$f" || { echo "MISSING: $f" >&2; exit 1; }
done
mkdir -p "$DEST"
for f in \
  "$DEST/opi5u-a14-baseline.zip" \
  "$DEST/opi5u-a14-hosttools.tar.gz" \
  "$DEST/opi5u-a14-clang.tar.gz"; do
  test ! -e "$f" || { echo "Preserving existing $f; remove it explicitly before rerunning" >&2; exit 1; }
done
cp "$BASE" "$DEST/opi5u-a14-baseline.zip"
tar -I 'gzip -1' -cf "$DEST/opi5u-a14-clang.tar.gz" "$CLANG"
HOST_PATHS=(out/host/linux-x86/bin)
for opt in out/host/linux-x86/lib out/host/linux-x86/lib64 out/host/linux-x86/framework; do
  test ! -d "$opt" || HOST_PATHS+=("$opt")
done
HOST_PATHS+=(
  build/make/target/product/security/testkey.pk8
  build/make/target/product/security/testkey.x509.pem
)
tar -I 'gzip -1' -cf "$DEST/opi5u-a14-hosttools.tar.gz" "${HOST_PATHS[@]}"
for f in "$DEST"/*; do
  stat -c '%n | %s bytes' "$f"
  # GitHub Releases limit each individual asset to 2 GiB.
  test "$(stat -c %s "$f")" -lt 2147483648 || {
    echo "ASSET TOO LARGE: $f; do not upload" >&2; exit 1;
  }
done
(cd "$DEST" && sha256sum opi5u-a14-*)
echo 'READY: These are public-development TESTKEY artifacts. Upload only if using the same trusted AOSP test key.'
echo "gh release create opi5u-a14-cloud-bootstrap $DEST/opi5u-a14-baseline.zip $DEST/opi5u-a14-hosttools.tar.gz $DEST/opi5u-a14-clang.tar.gz --repo t3st0st3r0n4s/FE-kernel-rockchip --target feature/kernelsu-next --title 'OPi5U A14 cloud bootstrap (dev key)' --notes 'Host-proven A14 OTA baseline, host tools, and pinned Clang. Public Android test-key only.' --prerelease"
