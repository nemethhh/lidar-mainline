#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Self-test for the stacked-overlay sync in build.sh (runs on the host).
#
# Asserts the stacking contract with marker files (TEST-MARKER at the overlay
# root, a name no overlay file uses):
#   1. a file present in the lidar-linux/, donut-linux/ and crumpet-linux/
#      overlays syncs to the crumpet version (crumpet shadows donut shadows
#      lidar),
#   2. after deleting the crumpet copy, the next sync restores the donut
#      version,
#   3. after deleting the donut copy too, the next sync restores the lidar
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
	rm -f "$WS/lidar-linux/$MARKER" "$WS/donut-linux/$MARKER" "$WS/crumpet-linux/$MARKER"
	sync >/dev/null
	# the overlay layers stay layered lidar -> donut -> crumpet
	mkdir -p "$WS/donut-linux" "$WS/donut-uboot" "$WS/crumpet-linux"
	[ ! -e "$WS/src/linux/$MARKER" ] || fail "cleanup: $MARKER still in src/linux"
}
trap cleanup EXIT

echo "selftest: crumpet file shadows donut file shadows lidar file"
mkdir -p "$WS/crumpet-linux"
printf 'lidar\n' > "$WS/lidar-linux/$MARKER"
printf 'donut\n' > "$WS/donut-linux/$MARKER"
printf 'crumpet\n' > "$WS/crumpet-linux/$MARKER"
sync >/dev/null
[ "$(cat "$WS/src/linux/$MARKER")" = crumpet ] ||
	fail "src/linux/$MARKER is not the crumpet version"

echo "selftest: deleting the crumpet copy restores the donut file"
rm "$WS/crumpet-linux/$MARKER"
sync >/dev/null
[ "$(cat "$WS/src/linux/$MARKER")" = donut ] ||
	fail "src/linux/$MARKER was not restored to the donut version"

echo "selftest: deleting the donut copy too restores the lidar file"
rm "$WS/donut-linux/$MARKER"
sync >/dev/null
[ "$(cat "$WS/src/linux/$MARKER")" = lidar ] ||
	fail "src/linux/$MARKER was not restored to the lidar version"

echo "selftest: lidar overlay still applied after the sync churn"
[ -f "$WS/src/linux/arch/arm64/boot/dts/mediatek/mt8516-amazon-lidar.dts" ] ||
	fail "lidar overlay file missing in src/linux"

echo "selftest: PASS"
