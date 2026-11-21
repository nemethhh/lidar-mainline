#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Run inside the donut-build container (ws = this workspace on the host):
#   docker run --rm -v <ws>:/work -v <ws>/src:/src donut-build bash /work/build.sh <target> [args]
#
# Source of truth for our changes are the overlay trees /work/lidar-linux and
# /work/lidar-uboot plus, once present, /work/donut-linux and /work/donut-uboot
# (overlays mirroring the upstream tree layout; a donut overlay file shadows
# the same file in the lidar overlay). They are synced onto the pristine
# checkouts in /src before every build, so the trees there are disposable.
#
# O names the kernel build directory /src/build/O (default: donut).
#
# Targets:
#   fetch                   clone linux v7.3 and u-boot v2026.10 into /src
#   sync                    sync overlays only
#   linux-config [O]        defconfig + lidar.config (+ donut.config unless O=lidar)
#   linux [O] [make args]   build (default: Image.gz dtbs modules)
#   linux-dir O DIR         build one directory, e.g. sound/soc/mediatek/mt8516/
#   wifi [O]                out-of-tree MT76x8 WLAN driver (donut-wifi/, vendor
#                           donut.config) into out/wifi/
#   uboot [board]           build U-Boot mt8516_amazon_<board>_defconfig (default
#                           lidar) and pack out/<board>-uboot.img
#   bootimg [O]             pack Image.gz + DTB for direct boot by Amazon LK
#   testimg [O]             ext4 test system (kernel, DTB, Alpine initramfs;
#                           donut has its own builder, lidar keeps its own)
#   patches                 export overlays as git patches to /work/out/patches
set -euo pipefail
J=${J:-$(nproc)}
SRC=/src
OUT=/work/out
mkdir -p "$OUT"

sync_tree() { # overlay tree — apply one overlay pass
	local ov=$1 tree=$2
	[ -d "$ov" ] || return 0
	rsync -rc --chmod=F644 --exclude '*.orig' "$ov/" "$tree/"
}

in_overlay() { # file overlays… — does any of the overlay dirs contain the file?
	local f=$1 ov
	for ov in "${@:2}"; do
		[ -e "$ov/$f" ] && return 0
	done
	return 1
}

sync_revert() { # tree overlays… — undo overlay changes no overlay provides anymore
	local tree=$1; shift
	local f
	# Tracked files go back to their pristine state.
	git -C "$tree" diff --name-only | while IFS= read -r f; do
		in_overlay "$f" "$@" || git -C "$tree" checkout -q -- "$f"
	done
	# Overlay-added files are removed again.
	git -C "$tree" ls-files --others --exclude-standard | while IFS= read -r f; do
		in_overlay "$f" "$@" || rm -f "$tree/$f"
	done
}

sync_all() {
	# lidar overlay first, donut overlay second: a file present in both
	# overlays ends up with the donut version.
	sync_tree /work/lidar-linux "$SRC/linux"
	sync_tree /work/lidar-uboot "$SRC/u-boot"
	sync_tree /work/donut-linux "$SRC/linux"
	sync_tree /work/donut-uboot "$SRC/u-boot"
	# After both overlay passes, files dropped from every overlay revert.
	sync_revert "$SRC/linux" /work/lidar-linux /work/donut-linux
	sync_revert "$SRC/u-boot" /work/lidar-uboot /work/donut-uboot
}

usage() { sed -n '3,27p' "$0"; exit 1; }

lo() { echo "$SRC/build/${1:-donut}"; }

fragments() { # O name — config fragments in merge order
	echo /work/lidar.config
	[ "${1:-donut}" = lidar ] || echo /work/donut.config
}

# The fragments are the source of truth: reconfigure when one is newer.
linux_config_if_stale() {
	local o n f; o=$(lo "${1:-}"); n=${1:-donut}
	[ -f "$o/.config" ] || { linux_config "$n"; return; }
	for f in $(fragments "$n"); do
		if [ "$f" -nt "$o/.config" ]; then linux_config "$n"; return; fi
	done
}

need_kernel() { # build dir
	[ -f "$1/arch/arm64/boot/Image.gz" ] ||
		{ echo "run: build.sh linux $(basename "$1") first" >&2; exit 1; }
}

linux_config() {
	local o n f rest line sym
	o=$(lo "${1:-}"); n=${1:-donut}
	make -C "$SRC/linux" O="$o" -s defconfig
	"$SRC/linux/scripts/kconfig/merge_config.sh" -m -O "$o" "$o/.config" $(fragments "$n") >/dev/null
	make -C "$SRC/linux" O="$o" -s olddefconfig
	# report fragment options that did not stick; a line for the same symbol
	# in a later fragment is an intended override (a donut delta on a lidar
	# line, e.g. dropping a lidar-only device), not a failure
	rest=$(fragments "$n" | tr '\n' ' ')
	while :; do
		f=${rest%% *}
		rest=${rest#"$f"}; rest=${rest# }
		while IFS= read -r line; do
			case "$line" in
			CONFIG_*=*) sym=${line%%=*} ;;
			"# CONFIG_"*" is not set") sym=${line#\# }; sym=${sym%% *} ;;
			*) continue ;;
			esac
			if [ -n "$rest" ] && grep -hq "^$sym=\|^# $sym is not set$" $rest; then continue; fi
			case "$line" in
			CONFIG_*=*) grep -qx "$line" "$o/.config" || echo "config: not applied: $line" ;;
			"# CONFIG_"*" is not set")
				if grep -q "^$sym=" "$o/.config"; then echo "config: still enabled: $sym"; fi ;;
			esac
		done < "$f"
		[ -n "$rest" ] || break
	done
}

cmd=${1:-}
shift || true
case "$cmd" in
fetch)
	[ -d "$SRC/linux/.git" ] || git clone --depth 1 -b v7.3 \
		https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$SRC/linux"
	[ -d "$SRC/u-boot/.git" ] || git clone --depth 1 -b v2026.10 \
		https://source.denx.de/u-boot/u-boot.git "$SRC/u-boot"
	;;
sync) sync_all ;;
linux-config) sync_all; linux_config "${1:-}" ;;
linux)
	sync_all
	n=${1:-}; shift || true
	o=$(lo "$n")
	linux_config_if_stale "$n"
	[ $# -gt 0 ] || set -- Image.gz dtbs modules
	make -C "$SRC/linux" O="$o" -j"$J" "$@"
	;;
linux-dir)
	[ $# -eq 2 ] || usage
	sync_all
	o=$(lo "$1")
	linux_config_if_stale "$1"
	[ -f "$o/scripts/mod/modpost" ] || make -C "$SRC/linux" O="$o" -j"$J" modules_prepare
	make -C "$SRC/linux" O="$o" -j"$J" W=1 "$2"
	;;
wifi)
	# Out-of-tree MT76x8 WLAN driver: donut.config is included by the
	# module Makefile; the kernel build dir must be a full build (Module.symvers
	# carries the in-tree symbol CRCs the module vermagic/CRC check needs).
	o=$(lo "${1:-}")
	sync_all
	[ -f "$o/Module.symvers" ] ||
		{ echo "run: build.sh linux $(basename "$o") first (need Module.symvers)" >&2; exit 1; }
	make -C "$SRC/linux" O="$o" -j"$J" M=/work/donut-wifi modules
	mkdir -p "$OUT/wifi"
	cp /work/donut-wifi/*.ko "$OUT/wifi/"
	# keep the repo copy pristine; the artifact lives in out/wifi/
	make -C "$SRC/linux" O="$o" M=/work/donut-wifi clean
	;;
uboot)
	board=${1:-lidar}
	sync_all
	o=$SRC/build/u-boot
	make -C "$SRC/u-boot" O="$o" -s "mt8516_amazon_${board}_defconfig"
	make -C "$SRC/u-boot" O="$o" -j"$J"
	# donut packs with donutboot (LK payload rules for its board/variant);
	# lidar keeps its own packer so its artifact stays byte-identical
	pack=/work/tools/lidarboot.py
	if [ "$board" = donut ]; then pack=/work/tools/donutboot.py; fi
	python3 "$pack" pack --kernel "$o/u-boot.bin" \
		--dtb "$o/u-boot.dtb" -o "$OUT/${board}-uboot.img"
	cp "$o/u-boot.bin" "$OUT/${board}-u-boot.bin"
	cp "$o/u-boot.dtb" "$OUT/${board}-u-boot.dtb"
	;;
testimg)
	# donut builds its own bring-up image; lidar keeps its test system
	n=${1:-donut}
	o=$(lo "$n")
	need_kernel "$o"
	img=/work/tools/mk-testimg.sh
	[ "$n" = donut ] && img=/work/tools/mk-donut-testimg.sh
	bash "$img" "$o"
	;;
bootimg)
	# Kernel booted directly by Amazon LK (no U-Boot): Image.gz + DTB in boot_X
	o=$(lo "${1:-lidar}")
	need_kernel "$o"
	python3 /work/tools/lidarboot.py pack --kernel "$o/arch/arm64/boot/Image.gz" \
		--dtb "$o/arch/arm64/boot/dts/mediatek/mt8516-amazon-lidar.dtb" \
		--cmdline "console=ttyS0,921600n8 earlycon=uart8250,mmio32,0x11005000" \
		-o "$OUT/lidar-linux-boot.img"
	;;
patches)
	sync_all
	rm -rf "$OUT/patches"; mkdir -p "$OUT/patches"
	for t in linux u-boot; do
		git -C "$SRC/$t" add -A >/dev/null
		git -C "$SRC/$t" diff --cached --stat
		git -C "$SRC/$t" diff --cached > "$OUT/patches/$t-donut.diff"
		git -C "$SRC/$t" reset -q
	done
	;;
*) usage ;;
esac
