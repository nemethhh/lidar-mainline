#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# M0 harvest - pull the evidence set (design doc section 5.4) off the rooted
# donut unit (Amazon Echo Dot 3rd gen, NS6529-era; the production WHA oracle)
# over adb. HOST-side script; READ-ONLY on the device: nothing is written.
#
# Guards (the unit is the production WHA oracle - never run casually):
#   --dry-run     print the exact command plan, execute nothing
#   --i-mean-it   execute for real; without one of the two flags the script
#                 refuses to run (exit 2, zero adb invocations)
#
# Harvest set:
#   - the five device-only MT7668 files (name-for-name the same list as
#     rootfs/usr/lib/firmware/donut-firmware-links.sh; test_stub.sh pins the
#     sync), searched in /system/vendor/firmware and /vendor/firmware, every
#     hit recorded with its location
#   - full /system/vendor/firmware snapshot (+ /vendor/firmware if it is a
#     different directory)
#   - IDME (/proc/idme), full getprop, /proc/cmdline, kernel log (dmesg)
#   - GPT evidence: raw by-name listings, per-name TSV (link target, resolved
#     node, size via blockdev --getsize64), /proc/partitions, /dev/block
#     node sizes (major:minor), and the decoy map for amonet-TWRP decoy
#     by-name links (techo5 porting-plan wisdom: prefer the <name>_real link)
#   - every boot-chain partition image (both boot slots + swdl + the LK
#     partitions + misc + full userdata p16 + the rest of the selector),
#     pulled from RESOLVED nodes (never through a decoy link)
#   - i2c population probe, best-effort (IS31FL3208@0x6F / TMP112@0x4B
#     population decision input, design doc section 9)
#
# Output tree (--out, default m0-harvest-<UTC stamp>):
#   partitions/            pulled partition images (+ partitions.tsv summary)
#   firmware/              full firmware dir snapshots
#   wifi-firmware/         the five blobs + FOUND-WHERE.txt
#   evidence/              getprop, cmdline, idme, kernel log, by-name
#                          report, decoy map, i2c probe, scans
#   manifest.txt/.json     via manifest.py
#
# Needs on the host: adb (platform-tools), python3, GNU coreutils, ~2 GB
# free disk. Needs on the unit: root adb (amonet-family units give it).
# set -euo pipefail throughout; quoting everywhere; no eval.

set -euo pipefail

SERIAL=${DONUT_ADB_SERIAL:-192.168.10.155:5555}
OUT=""
MODE=""   # "" = refuse | dry | real

die() { echo "harvest: $*" >&2; exit 2; }
usage() {
	cat >&2 <<'EOF'
usage: harvest.sh [--serial SERIAL] [--out DIR] (--dry-run | --i-mean-it)

  --serial S    adb serial (default: $DONUT_ADB_SERIAL or 192.168.10.155:5555)
  --out DIR     evidence dir (default: m0-harvest-<UTC timestamp>; must be
                fresh or empty)
  --dry-run     print the exact command plan, execute nothing
  --i-mean-it   execute for real against the unit (production WHA oracle)
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
		--serial) SERIAL=${2:?--serial needs an argument}; shift 2;;
		--out) OUT=${2:?--out needs an argument}; shift 2;;
		--dry-run) MODE=dry; shift;;
		--i-mean-it) MODE=real; shift;;
		-h|--help) usage; exit 0;;
		*) usage; die "unknown argument: $1";;
	esac
done
[ -n "$MODE" ] || die "refusing to run: pass --dry-run (print the plan) or --i-mean-it (the unit at $SERIAL is the production WHA oracle; no command has been executed)"
[ -n "$OUT" ] || OUT=m0-harvest-$(date -u +%Y%m%d-%H%M%S)
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
	die "--out $OUT exists and is not empty; pick a fresh evidence dir (evidence must never mix runs)"
fi

ADB=(adb -s "$SERIAL")
TOOLS_DIR=$(cd "$(dirname "$0")" && pwd)

# The five device-only MT7668 files (design doc section 3 device-only set).
# Keep name-for-name in sync with rootfs/usr/lib/firmware/donut-firmware-links.sh
# - test_stub.sh cross-checks the two lists.
WIFI_BLOBS=(
	WIFI_RAM_CODE_MT7668.bin
	WIFI_RAM_CODE2_SDIO_MT7668.bin
	EEPROM_MT7668.bin
	wifi.cfg
	TxPwrLimit_MT76x8.dat
)

print_cmd() { printf '+'; printf ' %q' "$@"; printf '\n'; }
run() {  # plan always, execute in real mode; failures are fatal
	print_cmd "$@"
	if [ "$MODE" = real ]; then "$@"; fi
}
run_ok() {  # best-effort variant
	print_cmd "$@"
	if [ "$MODE" = real ]; then "$@" || echo "  (best-effort: failed, continuing)"; fi
}
run_capture() {  # stdout (CR-stripped) -> file + $CAP; failures are fatal
	local f=$1; shift
	printf '+'; printf ' %q' "$@"; printf '  -> %q\n' "$f"
	CAP=""
	if [ "$MODE" = real ]; then
		"$@" | tr -d '\r' > "$f"
		CAP=$(cat "$f")
	fi
}
run_devsh() {  # run_devsh <outfile> <script-text>: pipe a POSIX device script
	          # through 'adb shell sh' (stdin), stdout CR-stripped to outfile
	printf '+ adb -s %s shell sh  > %s   # device script on stdin:\n' "$SERIAL" "$1"
	printf '%s\n' "$2" | sed 's/^/    | /'
	if [ "$MODE" = real ]; then
		"${ADB[@]}" shell sh < <(printf '%s\n' "$2") | tr -d '\r' > "$1"
	fi
}

require_file() {  # real mode only; evidence completeness checks
	[ -s "$1" ] || die "required evidence '$1' is empty/missing - $2"
}

# Boot-chain partition selector applied to the live by-name list. donut GPT
# per techo5 docs/hardware-donut.md: kb dkb lk_a tee1 lk_b tee2 expdb misc(p8)
# persist(p9) boot_a/b(p10/p11) swdl(p12,32M) system_a/b(p13/14) cache(p15)
# userdata(p16,1.5G); the preloader lives on mmcblk0boot0. Vendor-generic
# extras are matched too; huge non-boot-chain partitions are excluded and
# reported as unselected.
is_boot_chain() {
	case $1 in
		system|system_*|cache|product*|odm*|data|reserved*|flashinfo|sgpf*|proinfo|nvram*|bootpara) return 1;;
		userdata|boot|boot_*|swdl|lk|lk_*|lk2|tee|tee_*|tee1|tee2|expdb|misc|persist|para|kb|dkb|nvcfg|nvdata|seccfg|metadata|protect*|frp|preloader|preloader_*|recovery|recovery_*|vbmeta|vbmeta_*|dtbo|dtbo_*|md1img*|spmfw*|scp*|sspm*|mvpu*|vcu*|logo) return 0;;
		*) return 1;;
	esac
}

# Device scripts (POSIX sh, run on the unit via 'adb shell sh'). audit.sh
# carries a byte-identical copy of DEV_BYNAME - keep the two in sync.
DEV_BYNAME=$(cat <<'EOF'
# M0-DEV-SCRIPT: byname
echo "## by-name listings (raw ls -l - decoy and _real links visible here)"
for d in /dev/block/platform/*/by-name; do
	[ -e "$d" ] || continue
	echo "-- $d"
	ls -l "$d" 2>/dev/null || echo "(ls -l failed)"
done
echo "## /proc/partitions (major minor blocks-KiB name)"
cat /proc/partitions
echo "## /dev/block mmcblk nodes (ls -l: major:minor + byte size)"
ls -l /dev/block/mmcblk* 2>/dev/null || echo "(no mmcblk nodes listed)"
echo "## per-name resolution (TSV: name, link-target, resolved-node, size-bytes)"
for f in /dev/block/platform/*/by-name/*; do
	[ -e "$f" ] || continue
	n=${f##*/}
	t=$(readlink "$f" 2>/dev/null || echo "")
	tf=$(readlink -f "$f" 2>/dev/null || echo "")
	s=""
	[ -n "$tf" ] && s=$(blockdev --getsize64 "$tf" 2>/dev/null || echo "")
	printf '%s\t%s\t%s\t%s\n' "$n" "$t" "$tf" "$s"
done
true
EOF
)

DEV_I2C=$(cat <<'EOF'
# M0-DEV-SCRIPT: i2c
echo "## /dev/i2c-* nodes"
ls -l /dev/i2c-* 2>/dev/null || echo "(no /dev/i2c-* nodes)"
echo "## /sys/bus/i2c/devices (address + bound driver name)"
for d in /sys/bus/i2c/devices/*; do
	[ -e "$d/name" ] && printf '%s %s\n' "${d##*/}" "$(cat "$d/name")"
done
echo "## i2cdetect sweeps (only if i2c-tools is installed on the unit)"
if command -v i2cdetect >/dev/null 2>&1; then
	for b in 0 1 2; do
		i2cdetect -y "$b" 2>/dev/null || echo "(i2cdetect bus $b failed)"
	done
else
	echo "(i2cdetect not installed on the unit)"
fi
echo "## kernel log i2c lines (is31fl3208@0x6F / tmp112@0x4B decision input)"
dmesg 2>/dev/null | grep -iE 'i2c|is31|fl3208|fl3236|tmp1|tas2770|tsl2572|tlv320' || echo "(no i2c lines in the kernel log)"
true
EOF
)

DEV_FWDIRS=$(cat <<'EOF'
# M0-DEV-SCRIPT: fwdirs
for d in /system/vendor/firmware /vendor/firmware; do
	if [ -d "$d" ]; then
		printf '%s\t%s\n' "$d" "$(readlink -f "$d")"
	else
		printf '%s\tmissing\n' "$d"
	fi
done
true
EOF
)

# DEV_WIFISCAN is built from the WIFI_BLOBS array (single source of truth).
DEV_WIFISCAN='# M0-DEV-SCRIPT: wifiscan
for d in /system/vendor/firmware /vendor/firmware; do
	[ -d "$d" ] || continue'
for b in "${WIFI_BLOBS[@]}"; do
	DEV_WIFISCAN+=$(printf '\n\t[ -f "$d/%s" ] && printf "%%s\\t%%s\\t%%s\\n" "%s" "$d" "$d/%s"' "$b" "$b" "$b")
done
DEV_WIFISCAN+='
done
true'

BC_SELECTOR='userdata|boot*|swdl|lk*|tee*|expdb|misc|persist|para|kb|dkb|nvcfg|nvdata|seccfg|metadata|protect*|frp|preloader*|recovery*|vbmeta*|dtbo*|md1img*|spmfw*|scp*|sspm*|mvpu*|vcu*|logo (excluded: system*/cache/data/...)'

echo "harvest: serial=$SERIAL out=$OUT mode=$MODE"

if [ "$MODE" = real ]; then
	command -v adb >/dev/null || die "adb not in PATH"
	command -v python3 >/dev/null || die "python3 not in PATH"
	mkdir -p "$OUT/partitions" "$OUT/firmware" "$OUT/wifi-firmware" "$OUT/evidence"
fi

# --- reach the unit, confirm root ---------------------------------------
case $SERIAL in
	*:*) run_ok "${ADB[@]}" connect "$SERIAL";;
esac
run_ok "${ADB[@]}" root   # amonet-family units: adbd already root / adb root
print_cmd "${ADB[@]}" shell id -u
if [ "$MODE" = real ]; then
	u=$("${ADB[@]}" shell id -u | tr -d '\r')
	[ "$u" = 0 ] || die "adb shell is not root (id -u = $u); root adb is required for partition pulls - on amonet-family units 'adb root' provides it"
	echo '  (root: yes)'
fi

# --- plain evidence ------------------------------------------------------
run_capture "$OUT/evidence/getprop.txt" "${ADB[@]}" shell getprop
if [ "$MODE" = real ]; then require_file "$OUT/evidence/getprop.txt" "getprop is basic M0 evidence (ro.build.version expected NS6529-era)"; fi
run_capture "$OUT/evidence/cmdline.txt" "${ADB[@]}" shell cat /proc/cmdline
if [ "$MODE" = real ]; then require_file "$OUT/evidence/cmdline.txt" "the cmdline carries bootopt/slot state"; fi
run_capture "$OUT/evidence/idme.txt" "${ADB[@]}" shell cat /proc/idme
if [ "$MODE" = real ]; then require_file "$OUT/evidence/idme.txt" "IDME (MAC/serial) is an M0 done-when item"; fi
run_capture "$OUT/evidence/kernel-log.txt" "${ADB[@]}" shell dmesg
if [ "$MODE" = real ]; then require_file "$OUT/evidence/kernel-log.txt" "the stock-boot kernel log is an M0 done-when item"; fi

# --- GPT / by-name / decoy evidence --------------------------------------
run_devsh "$OUT/evidence/by-name-report.txt" "$DEV_BYNAME"
if [ "$MODE" = real ]; then require_file "$OUT/evidence/by-name-report.txt" "the GPT by-name map is the restore runbook's foundation"; fi

run_devsh "$OUT/evidence/i2c-probe.txt" "$DEV_I2C"

if [ "$MODE" = real ]; then
	# per-name TSV -> decoy map + partition selection (decoy-aware)
	declare -A RAW=() RESOLVED=() SIZE=()
	while IFS=$'\t' read -r name rlink rnode rsize; do
		[ -n "$name" ] || continue
		RAW[$name]=$rlink
		RESOLVED[$name]=$rnode
		SIZE[$name]=$rsize
	done < <(sed -n '/^## per-name resolution/,$p' "$OUT/evidence/by-name-report.txt" | tail -n +2)
	[ ${#RAW[@]} -gt 0 ] || die "by-name report carried no per-name rows - is this the donut unit?"

	# decoy map: amonet TWRP links by-name entries to /tmp/ota-decoy files so
	# an OTA cannot overwrite the unlock; the <name>_real link is the truth
	{
		echo "# decoy map - amonet-TWRP decoy by-name links (techo5 porting-plan wisdom)."
		echo "# Writes go to the resolved node of the <name>_real link, never the decoy."
		any=0
		while IFS= read -r name; do
			[ -n "$name" ] || continue
			base=${name%_real}
			note=""
			case ${RAW[$name]} in
				*ota-decoy*) note="decoy link (points into /tmp/ota-decoy)";;
			esac
			if [ "$name" != "$base" ]; then
				note="real variant of $base - authoritative source for writes"
			elif [ -z "$note" ] && [ -z "${RESOLVED[$name]}" ]; then
				note="dangling link (resolved target absent)"
			elif [ -z "$note" ] && [ -n "${RESOLVED[$name]}" ] && [ "${RAW[$name]}" != "${RESOLVED[$name]}" ]; then
				note="indirect link (raw target != resolved node)"
			fi
			[ -n "$note" ] || continue
			printf '%s\t%s\t%s\t%s\n' "$name" "${RAW[$name]}" "${RESOLVED[$name]}" "$note"
			any=1
		done < <(printf '%s\n' "${!RAW[@]}" | sort)
		[ "$any" = 1 ] || echo "(no decoy/indirect links observed)"
	} > "$OUT/evidence/decoy-map.txt"

	# partition pulls: prefer the _real target; pull from the resolved node
	selected=0
	unselected=""
	{
		echo "# name	source-node	size-bytes	link-target	via"
		while IFS= read -r base; do
			[ -n "$base" ] || continue
			if [ -n "${RESOLVED[${base}_real]:-}" ]; then
				src=${RESOLVED[${base}_real]}; via=_real; rawlink=${RAW[${base}_real]}
			elif [ -n "${RESOLVED[$base]:-}" ]; then
				src=${RESOLVED[$base]}; via=direct; rawlink=${RAW[$base]}
			else
				continue
			fi
			sz=${SIZE[${base}_real]:-${SIZE[$base]:-}}
			if is_boot_chain "$base"; then
				printf '%s\t%s\t%s\t%s\t%s\n' "$base" "$src" "$sz" "$rawlink" "$via"
			elif [ -z "$unselected" ]; then
				unselected=$base
			else
				unselected+=", $base"
			fi
		done < <(printf '%s\n' "${!RESOLVED[@]}" | sed 's/_real$//' | sort -u)
	} > "$OUT/partitions.tsv"

	while IFS=$'\t' read -r base src sz rawlink via; do
		[ -n "$base" ] || continue
		case $base in '#'*) continue;; esac
		run "${ADB[@]}" pull "$src" "$OUT/partitions/$base.img"
		selected=$((selected + 1))
	done < "$OUT/partitions.tsv"
	# the preloader lives on the boot0 hw partition regardless of by-name links
	for bn in mmcblk0boot0 mmcblk0boot1; do
		if [ ! -e "$OUT/partitions/$bn.img" ]; then
			run_ok "${ADB[@]}" pull "/dev/block/$bn" "$OUT/partitions/$bn.img"
		fi
	done
	echo "harvest: pulled $selected boot-chain partitions (selector: $BC_SELECTOR)"
	[ -z "$unselected" ] || echo "harvest: by-name partitions NOT pulled (not boot-chain): $unselected"
else
	echo "# plan: after the by-name scan, pull every boot-chain partition the by-name dir shows:"
	echo "#   adb -s $SERIAL pull <resolved-node> $OUT/partitions/<name>.img   (selector: $BC_SELECTOR)"
	echo "#   preferring the <name>_real resolved node (decoy-aware), plus /dev/block/mmcblk0boot0 and mmcblk0boot1,"
	echo "#   then partition.tsv, the firmware snapshots and manifest.py as listed above"
fi

# --- WiFi blobs ----------------------------------------------------------
run_devsh "$OUT/evidence/wifi-blob-scan.tsv" "$DEV_WIFISCAN"
if [ "$MODE" = real ]; then
	declare -A CANON=()
	while IFS=$'\t' read -r name dir path; do
		[ -n "$name" ] || continue
		[ -n "${CANON[$name]:-}" ] || CANON[$name]=$path
	done < "$OUT/evidence/wifi-blob-scan.tsv"
	missing=()
	for b in "${WIFI_BLOBS[@]}"; do
		if [ -n "${CANON[$b]:-}" ]; then
			run "${ADB[@]}" pull "${CANON[$b]}" "$OUT/wifi-firmware/$b"
		else
			missing+=("$b")
			echo "harvest: WARNING: $b NOT FOUND in /system/vendor/firmware /vendor/firmware" >&2
		fi
	done
	{
		echo "# found-locations for the five device-only MT7668 files (name, dir, path, canonical marker)"
		for b in "${WIFI_BLOBS[@]}"; do
			while IFS=$'\t' read -r name dir path; do
				[ "$name" = "$b" ] || continue
				printf '%s\t%s\t%s\t%s\n' "$name" "$dir" "$path" \
					"$([ "${CANON[$b]}" = "$path" ] && echo canonical || echo also-here)"
			done < "$OUT/evidence/wifi-blob-scan.tsv"
			[ -n "${CANON[$b]:-}" ] || printf '%s\tNOT FOUND\t-\tmissing\n' "$b"
		done
	} > "$OUT/wifi-firmware/FOUND-WHERE.txt"
fi

# --- firmware dir snapshots ----------------------------------------------
run_devsh "$OUT/evidence/firmware-dirs.tsv" "$DEV_FWDIRS"
if [ "$MODE" = real ]; then
	sysfw=$(awk -F'\t' '$1 == "/system/vendor/firmware" { print $2 }' "$OUT/evidence/firmware-dirs.tsv")
	[ -n "$sysfw" ] && [ "$sysfw" != missing ] ||
		die "/system/vendor/firmware is absent - the five MT7668 blobs cannot be where the vendor layout puts them"
	run "${ADB[@]}" pull /system/vendor/firmware "$OUT/firmware/system-vendor-firmware"
	vfw=$(awk -F'\t' '$1 == "/vendor/firmware" { print $2 }' "$OUT/evidence/firmware-dirs.tsv")
	if [ -n "$vfw" ] && [ "$vfw" != missing ] && [ "$vfw" != "$sysfw" ]; then
		run "${ADB[@]}" pull /vendor/firmware "$OUT/firmware/vendor-firmware"
	else
		echo "harvest: /vendor/firmware resolves to $vfw - same content, second snapshot skipped"
	fi
fi

# --- manifest + summary --------------------------------------------------
run python3 "$TOOLS_DIR/manifest.py" "$OUT"

if [ "$MODE" = real ]; then
	echo "harvest: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1) total"
	if [ "${missing[*]:-}" ]; then
		echo "harvest: MISSING device-only blobs: ${missing[*]}" >&2
		echo "harvest: M0 done-when is NOT met (all firmware files pulled and hashed); fix and re-harvest" >&2
	fi
	echo "harvest: feed-forward reminders (docs/m0-checklist.md):"
	echo "  - misc/boot_a/boot_b/swdl sizes (partitions.tsv) -> MISC_SIZE for tools/mk-donut-testimg.sh + donutboot.py boot-slot max_size"
	echo "  - five blobs (wifi-firmware/) -> stage into rootfs/usr/lib/firmware/, name-for-name"
	echo "  - i2c probe (evidence/i2c-probe.txt) -> IS31FL3208@0x6F / TMP112@0x4B population decision"
	echo "  - decoy map + partitions.tsv -> docs/restore.md write targets"
	[ ${#missing[@]} -eq 0 ] || exit 3
fi
echo "harvest: done ($MODE)"
