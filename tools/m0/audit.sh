#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# M0 unlock-state audit for the rooted donut unit (Amazon Echo Dot 3rd gen,
# the production WHA oracle). Records evidence, judges nothing: the patched
# (kaeru-family) LK markers it reports are read against the STOCK reference,
# and interpretation is left to the operator via docs/m0-checklist.md.
#
# Passive by construction: the unit is NEVER rebooted, unlocked, or flashed
# by this script. The fastboot getvar sweep runs only if fastboot is already
# reachable (the normal state is Fire OS, where it is not).
#
# Modes:
#   --dry-run          print the exact command plan, execute nothing
#   --i-mean-it        full audit against the unit over adb/fastboot
#   --harvest DIR      offline re-analysis of a prior tools/m0 harvest
#                      (NO device access at all - no adb, no fastboot)
# Extra inputs:
#   --serial S         adb/fastboot serial (default $DONUT_ADB_SERIAL or
#                      192.168.10.155:5555)
#   --out DIR          audit evidence dir (default m0-audit-<UTC stamp>)
#   --stock FILE       stock LK reference for the compare helper
#                      (default: ota-donut/2019/ota_out/lk.img, the NS6529
#                      factory image)
#   --lk FILE          additional LK image to analyze (repeatable)
#   --notes-file FILE  pre-filled manual fields (key=value lines) instead of
#                      the interactive prompt
#
# Output: audit-report.md (fastboot sweep, slot map, LK strings/marker
# table vs stock, misc slot-tries evidence, manual fields) + manual-fields.txt
# + manifest.txt/.json via manifest.py.
# set -euo pipefail throughout; quoting everywhere; no eval.

set -euo pipefail

SERIAL=${DONUT_ADB_SERIAL:-192.168.10.155:5555}
STOCK_LK=${DONUT_STOCK_LK:-/home/am/Work/SRE-Device/ota-donut/2019/ota_out/lk.img}
OUT=""
MODE=""   # dry | real | offline
HARVEST=""
NOTES_FILE=""
EXTRA_LK=()

ADB=(adb -s "$SERIAL")
FB=(fastboot -s "$SERIAL")
TOOLS_DIR=$(cd "$(dirname "$0")" && pwd)

# Patched-LK candidate markers (kaeru family). Reported as evidence, never
# as a verdict - the unit is EXPECTED to run a patched LK; stock strings are
# subtracted first so stock-common words do not show up as "markers".
MARKERS=(kaeru amonet kamakiri swdl)

# Manual fields the machine cannot answer (design doc: USB pads = TM4/TM5).
FIELD_KEYS=(usb_pads_tm4_tm5 vbus_source uart_console case_seal evidence_photos)
FIELD_PROMPTS=(
	"USB D-/D+ on test pads TM4/TM5: soldered wires / pogo clip / untouched"
	"VBUS source on those pads: barrel adapter only / USB 5V wired / unknown"
	"UART console access: test pads TX only / TX+RX / none"
	"case seal state: sealed / opened and resealed / opened"
	"photo/reference evidence taken: yes (where) / no"
)

die() { echo "audit: $*" >&2; exit 2; }
usage() {
	cat >&2 <<'EOF'
usage: audit.sh [--serial S] [--out DIR] [--stock FILE] [--lk FILE]...
                [--notes-file FILE] (--dry-run | --i-mean-it | --harvest DIR)

  --dry-run       print the plan, execute nothing
  --i-mean-it     audit the unit over adb/fastboot (passive: never reboots,
                  never unlocks, never flashes)
  --harvest DIR   offline re-analysis of a prior harvest; no device access
  --stock FILE    stock LK reference (default ota-donut/2019/ota_out/lk.img)
  --lk FILE       extra LK image to analyze (repeatable)
  --notes-file F  manual fields as key=value lines (keys: usb_pads_tm4_tm5,
                  vbus_source, uart_console, case_seal, evidence_photos)
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
		--serial) SERIAL=${2:?--serial needs an argument}; ADB=(adb -s "$SERIAL"); FB=(fastboot -s "$SERIAL"); shift 2;;
		--out) OUT=${2:?--out needs an argument}; shift 2;;
		--harvest) HARVEST=${2:?--harvest needs an argument}; shift 2;;
		--stock) STOCK_LK=${2:?--stock needs an argument}; shift 2;;
		--lk) EXTRA_LK+=(${2:?--lk needs an argument}); shift 2;;
		--notes-file) NOTES_FILE=${2:?--notes-file needs an argument}; shift 2;;
		--dry-run) MODE=dry; shift;;
		--i-mean-it) MODE=real; shift;;
		-h|--help) usage; exit 0;;
		*) usage; die "unknown argument: $1";;
	esac
done
if [ -n "$HARVEST" ]; then
	[ "$MODE" = "" ] || die "--harvest is offline: combine it with neither --dry-run nor --i-mean-it"
	[ -d "$HARVEST/partitions" ] && [ -d "$HARVEST/evidence" ] ||
		die "--harvest $HARVEST does not look like a harvest dir (needs partitions/ + evidence/)"
	MODE=offline
fi
[ -n "$MODE" ] || die "refusing to run: pass --dry-run (print the plan), --i-mean-it (audit the unit; the unit is the production WHA oracle), or --harvest DIR (offline)"
[ -n "$OUT" ] || OUT=m0-audit-$(date -u +%Y%m%d-%H%M%S)
if [ "$MODE" != dry ] && [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
	die "--out $OUT exists and is not empty; pick a fresh audit dir"
fi
command -v strings >/dev/null || die "strings (binutils) not in PATH - the LK marker check needs it"

print_cmd() { printf '+'; printf ' %q' "$@"; printf '\n'; }
run() {
	print_cmd "$@"
	if [ "$MODE" = real ]; then "$@"; fi
}
run_ok() {
	print_cmd "$@"
	if [ "$MODE" = real ]; then "$@" || echo "  (best-effort: failed, continuing)"; fi
}
run_capture() {
	local f=$1; shift
	printf '+'; printf ' %q' "$@"; printf '  -> %q\n' "$f"
	CAP=""
	if [ "$MODE" = real ]; then
		"$@" | tr -d '\r' > "$f"
		CAP=$(cat "$f")
	fi
}
run_devsh() {
	printf '+ adb -s %s shell sh  > %s   # device script on stdin:\n' "$SERIAL" "$1"
	printf '%s\n' "$2" | sed 's/^/    | /'
	if [ "$MODE" = real ]; then
		"${ADB[@]}" shell sh < <(printf '%s\n' "$2") | tr -d '\r' > "$1"
	fi
}
require_file() { [ -s "$1" ] || die "required evidence '$1' is empty/missing - $2"; }

# Device script - byte-identical to harvest.sh's DEV_BYNAME (keep in sync).
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

echo "audit: serial=$SERIAL out=$OUT mode=$MODE stock=$(basename "$STOCK_LK" 2>/dev/null || echo "$STOCK_LK")"

# --- fastboot getvar sweep (only if already reachable) -------------------
sweep_vars=(unlocked secure product serialno current-slot hw-revision battery-soc-ok off-mode-charge partition-size:boot_a partition-size:boot_b partition-size:swdl partition-size:lk_a partition-size:lk_b partition-size:misc partition-type:boot_a partition-type:boot_b)
if [ "$MODE" = real ]; then
	mkdir -p "$OUT"
	if ! command -v fastboot >/dev/null; then
		echo "audit: fastboot not on PATH - getvar sweep skipped (unit in Fire OS; the normal state)"
	elif ! command -v adb >/dev/null; then
		echo "audit: adb not on PATH - cannot audit the running unit (use --harvest for offline analysis)"
		exit 2
	else
		run_capture "$OUT/fastboot-devices.txt" "${FB[@]}" devices
		if [ -n "$CAP" ]; then
			echo "audit: fastboot reachable - sweeping getvar"
			print_cmd "${FB[@]}" getvar all
			"${FB[@]}" getvar all > "$OUT/fastboot-getvar-all.txt" 2>&1
			: > "$OUT/fastboot-getvars.txt"
			for v in "${sweep_vars[@]}"; do
				printf '+'; printf ' %q' "${FB[@]}" getvar "$v"; printf '\n'
				{ echo "## getvar $v"; "${FB[@]}" getvar "$v" 2>&1 || echo "(getvar failed)"; } >> "$OUT/fastboot-getvars.txt"
				done
			rm -f "$OUT/fastboot-devices.txt"
		else
			echo "audit: fastboot shows no device - sweep skipped (unit running Fire OS; that is expected and NOT an unlock verdict)"
		fi
	fi
elif [ "$MODE" = dry ]; then
	echo '# plan: fastboot devices; if a device shows, sweep:'
	print_cmd "${FB[@]}" getvar all
	for v in "${sweep_vars[@]}"; do print_cmd "${FB[@]}" getvar "$v"; done
	echo '# (fastboot is never made reachable: no reboot, no unlock)'
else
	echo "audit: offline mode - fastboot not queried"
fi

# --- adb evidence + LK/misc pulls ----------------------------------------
if [ "$MODE" != offline ]; then
	case $SERIAL in
		*:*) run_ok "${ADB[@]}" connect "$SERIAL";;
	esac
	run_ok "${ADB[@]}" root
	print_cmd "${ADB[@]}" shell id -u
	if [ "$MODE" = real ]; then
		u=$("${ADB[@]}" shell id -u | tr -d '\r')
		[ "$u" = 0 ] || die "adb shell is not root (id -u = $u); root adb required (amonet-family units: 'adb root')"
		echo '  (root: yes)'
		mkdir -p "$OUT/evidence" "$OUT/lk" "$OUT/misc"
	fi
	run_capture "$OUT/evidence/getprop.txt" "${ADB[@]}" shell getprop
	if [ "$MODE" = real ]; then require_file "$OUT/evidence/getprop.txt" "getprop carries the slot/bootstate props"; fi
	run_capture "$OUT/evidence/cmdline.txt" "${ADB[@]}" shell cat /proc/cmdline
	run_devsh "$OUT/evidence/by-name-report.txt" "$DEV_BYNAME"
	if [ "$MODE" = real ]; then require_file "$OUT/evidence/by-name-report.txt" "the by-name map carries the slot map"; fi

	if [ "$MODE" = real ]; then
		found_lk=0
		while IFS=$'\t' read -r name rlink rnode rsize; do
			[ -n "$name" ] || continue
			base=${name%_real}
			case $base in
				lk*)
					[ -n "$rnode" ] || { echo "audit: WARNING: $name has no resolved node (decoy?)"; continue; }
					if [ ! -e "$OUT/lk/$base.img" ]; then
						run "${ADB[@]}" pull "$rnode" "$OUT/lk/$base.img"
						found_lk=$((found_lk + 1))
					fi
					;;
				misc)
					[ -n "$rnode" ] || { echo "audit: WARNING: misc has no resolved node"; continue; }
					run "${ADB[@]}" pull "$rnode" "$OUT/misc/misc.img"
					;;
			esac
		done < <(sed -n '/^## per-name resolution/,$p' "$OUT/evidence/by-name-report.txt" | tail -n +2)
		[ "$found_lk" -gt 0 ] || die "no LK partitions found in by-name - cannot audit the bootloader"
	fi
fi

# --- collect LK inputs (pulled + extra + offline harvest) ----------------
declare -A LK_SEEN=()
LK_IMGS=()
add_lk() {
	local f=$1
	[ -f "$f" ] || return 0
	local h; h=$(sha256sum "$f" | awk '{print $1}')
	if [ -z "${LK_SEEN[$h]:-}" ]; then
		LK_SEEN[$h]="$f"
		LK_IMGS+=("$f")
	fi
}
if [ "$MODE" = real ]; then
	for f in "$OUT"/lk/*.img; do add_lk "$f"; done
elif [ "$MODE" = offline ]; then
	for f in "$HARVEST"/partitions/lk*.img; do add_lk "$f"; done
fi
for f in "${EXTRA_LK[@]}"; do add_lk "$f"; done
if [ "$MODE" != dry ]; then
	[ "${#LK_IMGS[@]}" -gt 0 ] || die "no LK images to analyze (pull failed, or pass --lk FILE / --harvest DIR)"
fi

MISC_IMG=""
if [ "$MODE" = real ] && [ -f "$OUT/misc/misc.img" ]; then MISC_IMG=$OUT/misc/misc.img; fi
if [ "$MODE" = offline ] && [ -f "$HARVEST/partitions/misc.img" ]; then MISC_IMG=$HARVEST/partitions/misc.img; fi

# --- slot map ------------------------------------------------------------
build_slot_map() {  # <getprop> <cmdline> <byname-report> <outfile>
	{
		echo "# slot map evidence (A/B boot slots + swdl + LK partitions; donut GPT per techo5 docs/hardware-donut.md)"
		echo "## getprop slot lines"
		grep -i "slot" "$1" || echo "(no slot props in getprop)"
		echo "## /proc/cmdline"
		cat "$2"
		echo "## boot-chain partition sizes (name, size-bytes, resolved node)"
		sed -n '/^## per-name resolution/,$p' "$3" | tail -n +2 |
			awk -F'\t' '$1 ~ /^(boot_a|boot_b|swdl|lk_a|lk_b|lk|lk2|misc)(_real)?$/ { printf "%s\t%s\t%s\n", $1, $4, $3 }'
	} > "$4"
}
if [ "$MODE" = real ]; then
	mkdir -p "$OUT/evidence"
	build_slot_map "$OUT/evidence/getprop.txt" "$OUT/evidence/cmdline.txt" \
		"$OUT/evidence/by-name-report.txt" "$OUT/evidence/slot-map.txt"
elif [ "$MODE" = offline ]; then
	mkdir -p "$OUT/evidence"
	build_slot_map "$HARVEST/evidence/getprop.txt" "$HARVEST/evidence/cmdline.txt" \
		"$HARVEST/evidence/by-name-report.txt" "$OUT/evidence/slot-map.txt"
else
	echo '# plan: slot map built from getprop + cmdline + by-name TSV -> evidence/slot-map.txt'
fi

# --- manual fields -------------------------------------------------------
declare -A FIELDV=()
if [ -n "$NOTES_FILE" ]; then
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		case $line in '#'*) continue;; esac
		k=${line%%=*}
		v=${line#*=}
		known=""
		for kk in "${FIELD_KEYS[@]}"; do
			if [ "$kk" = "$k" ]; then known=1; break; fi
		done
		if [ -n "$known" ]; then FIELDV[$k]=$v; else
			echo "audit: notes-file: unknown key '$k' (known: ${FIELD_KEYS[*]})" >&2
		fi
	done < "$NOTES_FILE"
elif [ "$MODE" = real ] && [ -t 0 ]; then
	echo "audit: manual fields the machine cannot answer (Ctrl-D leaves them UNFILLED):"
	for i in "${!FIELD_KEYS[@]}"; do
		k=${FIELD_KEYS[$i]}
		printf '  %s (%s): ' "$k" "${FIELD_PROMPTS[$i]}"
		IFS= read -r ans || ans=""
		[ -n "$ans" ] || ans=UNFILLED
		FIELDV[$k]=$ans
	done
else
	if [ "$MODE" = real ]; then
		echo "audit: stdin is not a tty and no --notes-file - manual fields left UNFILLED (fill $OUT/manual-fields.txt)"
	else
		echo "audit: manual fields left UNFILLED (fill them with --notes-file or a tty run)"
	fi
	for k in "${FIELD_KEYS[@]}"; do FIELDV[$k]=UNFILLED; done
fi
if [ "$MODE" != dry ]; then
	mkdir -p "$OUT"
	{
		echo "# M0 audit manual fields (operator-answered; keys: ${FIELD_KEYS[*]})"
		for i in "${!FIELD_KEYS[@]}"; do
			printf '%s=%s\n' "${FIELD_KEYS[$i]}" "${FIELDV[${FIELD_KEYS[$i]}]}"
		done
	} > "$OUT/manual-fields.txt"
fi

# --- report --------------------------------------------------------------
if [ "$MODE" != dry ]; then
	mkdir -p "$OUT"
	{
		echo "# M0 unlock-state audit - donut unit"
		echo
		echo "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "- mode: $MODE$([ -n "$HARVEST" ] && echo " (offline source: $HARVEST)")"
		echo "- unit serial: $SERIAL"
		echo "- stock LK reference: $STOCK_LK"
		echo "- this report records evidence; it does NOT judge unlock state."
		echo "  The unit is expected to run a patched (kaeru-family) LK; interpretation lives in docs/m0-checklist.md."
		echo
		echo "## 1. fastboot getvar sweep"
		if [ "$MODE" = offline ]; then
			echo "- offline re-analysis: fastboot not queried; run audit.sh --i-mean-it for the sweep."
		elif [ -f "$OUT/fastboot-getvar-all.txt" ]; then
			echo "- reachable; 'getvar all' captured in fastboot-getvar-all.txt, targeted vars in fastboot-getvars.txt."
			grep -iE "unlocked|secure|current-slot" "$OUT/fastboot-getvar-all.txt" | sed 's/^/  - /' || true
		else
			echo "- fastboot NOT reachable at audit time (unit running Fire OS - the normal state; NOT an unlock verdict)."
			echo "  If the unit ever lands in the bootloader, re-run this audit to capture the sweep."
		fi
		echo
		echo "## 2. slot map"
		if [ "$MODE" = offline ]; then
			cat "$OUT/evidence/slot-map.txt"
		elif [ -f "$OUT/evidence/slot-map.txt" ]; then
			cat "$OUT/evidence/slot-map.txt"
		else
			echo "(dry-run: no slot map captured)"
		fi
		echo
		echo "## 3. LK partition images vs stock reference"
		echo "- patched-LK candidate markers: ${MARKERS[*]} (stock strings subtracted first; presence is evidence, not a verdict)"
		for img in "${LK_IMGS[@]}"; do
			echo
			echo "### $(basename "$img")"
			echo "- sha256 \`$(sha256sum "$img" | awk '{print $1}')\`, $(stat -c %s "$img") bytes"
			if [ -f "$STOCK_LK" ]; then
				if cmp -s "$img" "$STOCK_LK"; then
					echo "- stock compare: IDENTICAL to $(basename "$STOCK_LK")"
				else
					echo "- stock compare: DIFFERENT from $(basename "$STOCK_LK") (first diff at byte $(cmp -l "$img" "$STOCK_LK" 2>/dev/null | head -1 | awk '{print $1}'))"
				fi
				st=$(mktemp); ut=$(mktemp)
				strings -n 8 "$STOCK_LK" | tr -d '\r' | sort -u > "$st"
				strings -n 8 "$img" | tr -d '\r' | sort -u > "$ut"
				nonly=$(comm -13 "$st" "$ut" | grep -c . || true)
				echo "- strings >=8 chars: $(grep -c . < "$ut" || true) unique; unit-only vs stock: $nonly"
				if [ "$nonly" -gt 0 ]; then
					echo "  sample unit-only strings:"
					comm -13 "$st" "$ut" | grep . | head -12 | sed 's/^/  - /'
				fi
				echo "  candidate markers:"
				any=0
				for m in "${MARKERS[@]}"; do
					if grep -q "$m" "$ut"; then
						echo "  - $m: PRESENT"
						any=1
					else
						echo "  - $m: absent"
					fi
				done
				[ "$any" = 1 ] || echo "  - (no candidate markers present in unit-only strings)"
				rm -f "$st" "$ut"
			else
				echo "- stock compare: SKIPPED (stock LK not found at $STOCK_LK)"
				echo "  candidate markers (grep against the image, no stock baseline):"
				any=0
				for m in "${MARKERS[@]}"; do
					if grep -aq "$m" "$img"; then
						echo "  - $m: PRESENT"
						any=1
					else
						echo "  - $m: absent"
					fi
				done
				[ "$any" = 1 ] || echo "  - (none of the candidate markers appear in the image)"
			fi
		done
		echo
		echo "## 4. misc (boot-control) evidence"
		if [ -n "$MISC_IMG" ] && [ -f "$MISC_IMG" ]; then
			echo "- sha256 \`$(sha256sum "$MISC_IMG" | awk '{print $1}')\`, $(stat -c %s "$MISC_IMG") bytes"
			echo "  NOTE: misc mutates on boot (slot tries); treat this copy as audit-time state, re-harvest before relying on it."
			echo "  first 512 bytes:"
			od -A x -t x1z -v "$MISC_IMG" | head -32 | sed 's/^/    /'
			echo "  strings:"
			strings -n 4 "$MISC_IMG" | tr -d '\r' | head -24 | sed 's/^/    /'
		else
			echo "- misc image not available in this mode (offline harvest without partitions/misc.img, or dry-run)."
		fi
		echo
		echo "## 5. manual fields (operator-answered)"
		for i in "${!FIELD_KEYS[@]}"; do
			k=${FIELD_KEYS[$i]}
			echo "- ${FIELD_PROMPTS[$i]}"
			echo "  - \`${k}\`: ${FIELDV[$k]}"
		done
		echo
		echo "## 6. where this feeds"
		echo "- deploy-path decision (design section 5/8): patched-LK markers present -> root-shell dd deploy is viable; absent -> kamakiri-donut kit first."
		echo "- USB-pad fields -> kamakiri BROM-entry feasibility (action button held, USB on TM4/TM5)."
		echo "- restore runbook: docs/restore.md; M0 execution order: docs/m0-checklist.md."
	} > "$OUT/audit-report.md"
	run python3 "$TOOLS_DIR/manifest.py" "$OUT"
	echo "audit: report -> $OUT/audit-report.md"
fi
echo "audit: done ($MODE)"
