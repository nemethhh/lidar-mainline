#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Self-test for the stacked-overlay sync in build.sh (runs on the host).
#
# Asserts the stacking contract with marker files (TEST-MARKER at the overlay
# root, a name no overlay file uses):
#   1. a file present in both lidar-linux/ and donut-linux/ syncs to the donut
#      version (donut shadows lidar),
#   2. after deleting the donut copy, the next sync restores the lidar
#      version.
# The markers are removed again afterwards (also on failure), leaving the
# overlays and src/ pristine. Requires `build.sh fetch` to have run once.
set -euo pipefail
WS=$(cd "$(dirname "$0")/.." && pwd)
MARKER=TEST-MARKER
IMG=donut-build

fail() { echo "selftest: FAIL: $*" >&2; exit 1; }

[ -d "$WS/src/linux/.git" ] && [ -d "$WS/src/u-boot/.git" ] ||
	fail "src/linux or src/u-boot missing; run first:
  docker run --rm -v \$PWD:/work -v \$PWD/src:/src $IMG bash /work/build.sh fetch"
docker image inspect "$IMG" >/dev/null 2>&1 ||
	fail "docker image $IMG missing; run: docker build -t $IMG $WS"

sync() { docker run --rm -v "$WS":/work -v "$WS/src":/src "$IMG" bash /work/build.sh sync; }

cleanup() {
	rm -f "$WS/lidar-linux/$MARKER" "$WS/donut-linux/$MARKER"
	sync >/dev/null
	# donut-linux/ and donut-uboot/ stay as the (still empty) second layer
	mkdir -p "$WS/donut-linux" "$WS/donut-uboot"
	[ ! -e "$WS/src/linux/$MARKER" ] || fail "cleanup: $MARKER still in src/linux"
}
trap cleanup EXIT

echo "selftest: donut file shadows lidar file"
mkdir -p "$WS/donut-linux"
printf 'lidar\n' > "$WS/lidar-linux/$MARKER"
printf 'donut\n' > "$WS/donut-linux/$MARKER"
sync >/dev/null
[ "$(cat "$WS/src/linux/$MARKER")" = donut ] ||
	fail "src/linux/$MARKER is not the donut version"

echo "selftest: deleting the donut copy restores the lidar file"
rm "$WS/donut-linux/$MARKER"
sync >/dev/null
[ "$(cat "$WS/src/linux/$MARKER")" = lidar ] ||
	fail "src/linux/$MARKER was not restored to the lidar version"

echo "selftest: lidar overlay still applied after the sync churn"
[ -f "$WS/src/linux/arch/arm64/boot/dts/mediatek/mt8516-amazon-lidar.dts" ] ||
	fail "lidar overlay file missing in src/linux"

echo "selftest: PASS"
