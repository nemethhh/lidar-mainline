#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Stub self-test for the M0 kit (host-only: NO device, NO network ever).
#
# Builds a fake adb + fake fastboot in a temp PATH that answers the exact
# commands tools/m0/harvest.sh and tools/m0/audit.sh issue against a canned
# Fire OS filesystem (donut GPT per techo5 docs/hardware-donut.md, including
# amonet-TWRP decoy by-name links), then asserts:
#   1. refusal: no mode flag -> nonzero exit, ZERO adb invocations
#   2. --dry-run -> plan printed, ZERO adb invocations, no output dir
#   3. --i-mean-it -> full sequence runs; stub files land; decoy links are
#      NOT pulled (<name>_real resolved instead); the five MT7668 blobs are
#      found + pulled with locations recorded; manifest.txt/.json hash the
#      pulled bytes (manifest.py --check green)
#   3b. missing blob -> harvest still records everything but exits 3
#   4. audit --dry-run -> plan printed, ZERO adb/fastboot invocations
#   5. audit --i-mean-it (fake fastboot reachable) -> getvar sweep captured,
#      LK marker table reports kaeru/kamakiri/swdl vs a stock stub, manual
#      fields pre-filled from --notes-file
#   5b. audit vs the REAL stock lk.img (skipped when the reference is absent)
#   6. audit --harvest (offline) -> full report, ZERO adb/fastboot calls
#   7. manifest.py --check detects a tampered file
#   8. name-for-name sync: harvest.sh's five-blob list == the list in
#      rootfs/usr/lib/firmware/donut-firmware-links.sh
set -euo pipefail

WS=$(cd "$(dirname "$0")/../.." && pwd)
M0=$WS/tools/m0
FWLINKS=$WS/rootfs/usr/lib/firmware/donut-firmware-links.sh
STOCK_DEFAULT=${DONUT_STOCK_LK:-/home/am/Work/SRE-Device/ota-donut/2019/ota_out/lk.img}

fail() { echo "test_stub: FAIL: $*" >&2; exit 1; }
ok()   { echo "test_stub: ok: $*"; }

# syntax checks first
for f in "$M0/harvest.sh" "$M0/audit.sh" "$M0/test_stub.sh"; do
	bash -n "$f" || fail "bash -n $f"
done
ok "bash -n harvest.sh audit.sh test_stub.sh"
python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$M0/manifest.py" ||
	fail "manifest.py does not parse"
ok "manifest.py parses"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
BIN=$T/bin
mkdir -p "$BIN"
LOG=$T/adb.log
FBLOG=$T/fastboot.log
STUB=$T/stub
mkdir -p "$STUB"
export STUB_LOG=$LOG FBLOG=$FBLOG STUB_DIR=$STUB
PATH="$BIN:$PATH"
export PATH

# --- canned unit ---------------------------------------------------------
cat > "$STUB/getprop.txt" <<'EOF'
[ro.product.device]: [donut_puffin]
[ro.product.model]: [AEODN]
[ro.build.version.release]: [9]
[ro.build.version.incremental]: [6574.1]
[ro.serialno]: [DONUTSTUB001]
[ro.boot.slot_suffix]: [a]
[ro.boot.verifiedbootstate]: [orange]
[ro.boot.flash.locked]: [0]
EOF
cat > "$STUB/cmdline.txt" <<'EOF'
bootopt=64S3,32N2,64N2 console=ttyS0,921600n8 androidboot.hardware=donut androidboot.serialno=DONUTSTUB001 androidboot.slot_suffix=a androidboot.bootdevice=11230000.mmc
EOF
cat > "$STUB/idme.txt" <<'EOF'
unique_id=DONUTSTUBUNIQUE0001
device_type_id=A32DOYMUN6DTXA
serial_no=DONUTSTUB001
mac_wlan=02:00:aa:bb:cc:f1
mac_bt=02:00:aa:bb:cc:f2
mac_ble=02:00:aa:bb:cc:f3
EOF
cat > "$STUB/dmesg.txt" <<'EOF'
[    0.000000] Booting Linux on physical CPU 0x0
[    0.875001] i2c /dev entries driver
[    1.024010] mt6577-i2c 11007000.i2c: feature: use 2 pause buttons
[    1.234567] is31fl3236 0-0041: 12 RGB leds registered
[    1.345678] tas2770 1-0040: smart amp probed
[    1.456789] tlv320adc3101 1-0018: probed
[    1.456790] tlv320adc3101 1-0019: probed
[    1.567890] tsl2572 2-0039: ambient light sensor probed
EOF

# donut GPT per techo5 docs/hardware-donut.md + amonet-TWRP decoy links
# (lk_a, tee1, preloader decoyed into /tmp/ota-decoy; _real is the truth).
cat > "$STUB/byname-output.txt" <<'EOF'
## by-name listings (raw ls -l - decoy and _real links visible here)
-- /dev/block/platform/bootdevice/by-name
lrwxrwxrwx root root 2019-01-01 01:00 kb -> /dev/block/mmcblk0p1
lrwxrwxrwx root root 2019-01-01 01:00 dkb -> /dev/block/mmcblk0p2
lrwxrwxrwx root root 2019-01-01 01:00 lk_a -> /tmp/ota-decoy/lk_a
lrwxrwxrwx root root 2019-01-01 01:00 lk_a_real -> /dev/block/mmcblk0p3
lrwxrwxrwx root root 2019-01-01 01:00 tee1 -> /tmp/ota-decoy/tee1
lrwxrwxrwx root root 2019-01-01 01:00 tee1_real -> /dev/block/mmcblk0p4
lrwxrwxrwx root root 2019-01-01 01:00 lk_b -> /dev/block/mmcblk0p5
lrwxrwxrwx root root 2019-01-01 01:00 tee2 -> /dev/block/mmcblk0p6
lrwxrwxrwx root root 2019-01-01 01:00 expdb -> /dev/block/mmcblk0p7
lrwxrwxrwx root root 2019-01-01 01:00 misc -> /dev/block/mmcblk0p8
lrwxrwxrwx root root 2019-01-01 01:00 persist -> /dev/block/mmcblk0p9
lrwxrwxrwx root root 2019-01-01 01:00 boot_a -> /dev/block/mmcblk0p10
lrwxrwxrwx root root 2019-01-01 01:00 boot_b -> /dev/block/mmcblk0p11
lrwxrwxrwx root root 2019-01-01 01:00 swdl -> /dev/block/mmcblk0p12
lrwxrwxrwx root root 2019-01-01 01:00 system_a -> /dev/block/mmcblk0p13
lrwxrwxrwx root root 2019-01-01 01:00 system_b -> /dev/block/mmcblk0p14
lrwxrwxrwx root root 2019-01-01 01:00 cache -> /dev/block/mmcblk0p15
lrwxrwxrwx root root 2019-01-01 01:00 userdata -> /dev/block/mmcblk0p16
lrwxrwxrwx root root 2019-01-01 01:00 preloader -> /tmp/ota-decoy/preloader
lrwxrwxrwx root root 2019-01-01 01:00 preloader_real -> /dev/block/mmcblk0boot0
## /proc/partitions (major minor blocks-KiB name)
major minor  #blocks  name

 179        0    3814656 mmcblk0
 179        1        512 mmcblk0p1
 179        2        512 mmcblk0p2
 179        3       1024 mmcblk0p3
 179        4       8192 mmcblk0p4
 179        5       1024 mmcblk0p5
 179        6       8192 mmcblk0p6
 179        7       8192 mmcblk0p7
 179        8       1024 mmcblk0p8
 179        9      16384 mmcblk0p9
 179       10      16384 mmcblk0p10
 179       11      16384 mmcblk0p11
 179       12      32768 mmcblk0p12
 179       13    1048576 mmcblk0p13
 179       14    1048576 mmcblk0p14
 179       15      32768 mmcblk0p15
 179       16    1572864 mmcblk0p16
 179       32       4096 mmcblk0boot0
 179       64       4096 mmcblk0boot1
## /dev/block mmcblk nodes (ls -l: major:minor + byte size)
brw------- root root 179, 0  2019-01-01 01:00 mmcblk0
brw------- root root 179, 3  2019-01-01 01:00 mmcblk0p3
brw------- root root 179, 8  2019-01-01 01:00 mmcblk0p8
brw------- root root 179, 16 2019-01-01 01:00 mmcblk0p16
brw------- root root 179, 32 2019-01-01 01:00 mmcblk0boot0
brw------- root root 179, 64 2019-01-01 01:00 mmcblk0boot1
## per-name resolution (TSV: name, link-target, resolved-node, size-bytes)
kb	/dev/block/mmcblk0p1	/dev/block/mmcblk0p1	524288
dkb	/dev/block/mmcblk0p2	/dev/block/mmcblk0p2	524288
lk_a	/tmp/ota-decoy/lk_a
lk_a_real	/dev/block/mmcblk0p3	/dev/block/mmcblk0p3	1048576
tee1	/tmp/ota-decoy/tee1
tee1_real	/dev/block/mmcblk0p4	/dev/block/mmcblk0p4	8388608
lk_b	/dev/block/mmcblk0p5	/dev/block/mmcblk0p5	1048576
tee2	/dev/block/mmcblk0p6	/dev/block/mmcblk0p6	8388608
expdb	/dev/block/mmcblk0p7	/dev/block/mmcblk0p7	8388608
misc	/dev/block/mmcblk0p8	/dev/block/mmcblk0p8	1048576
persist	/dev/block/mmcblk0p9	/dev/block/mmcblk0p9	16777216
boot_a	/dev/block/mmcblk0p10	/dev/block/mmcblk0p10	16777216
boot_b	/dev/block/mmcblk0p11	/dev/block/mmcblk0p11	16777216
swdl	/dev/block/mmcblk0p12	/dev/block/mmcblk0p12	33554432
system_a	/dev/block/mmcblk0p13	/dev/block/mmcblk0p13	1073741824
system_b	/dev/block/mmcblk0p14	/dev/block/mmcblk0p14	1073741824
cache	/dev/block/mmcblk0p15	/dev/block/mmcblk0p15	33554432
userdata	/dev/block/mmcblk0p16	/dev/block/mmcblk0p16	1610612736
preloader	/tmp/ota-decoy/preloader
preloader_real	/dev/block/mmcblk0boot0	/dev/block/mmcblk0boot0	4194304
EOF

{
	echo "WIFI_RAM_CODE_MT7668.bin	/system/vendor/firmware	/system/vendor/firmware/WIFI_RAM_CODE_MT7668.bin"
	echo "WIFI_RAM_CODE2_SDIO_MT7668.bin	/system/vendor/firmware	/system/vendor/firmware/WIFI_RAM_CODE2_SDIO_MT7668.bin"
	echo "EEPROM_MT7668.bin	/system/vendor/firmware	/system/vendor/firmware/EEPROM_MT7668.bin"
	echo "wifi.cfg	/system/vendor/firmware	/system/vendor/firmware/wifi.cfg"
	echo "TxPwrLimit_MT76x8.dat	/system/vendor/firmware	/system/vendor/firmware/TxPwrLimit_MT76x8.dat"
} > "$STUB/wifiscan.tsv"

cat > "$STUB/i2c-output.txt" <<'EOF'
## /dev/i2c-* nodes
crw-rw---- root root 89, 0 2019-01-01 01:00 i2c-0
crw-rw---- root root 89, 1 2019-01-01 01:00 i2c-1
crw-rw---- root root 89, 2 2019-01-01 01:00 i2c-2
## /sys/bus/i2c/devices (address + bound driver name)
0-0041 is31fl3236
1-0040 tas2770
1-0018 tlv320adc3101
1-0019 tlv320adc3101
2-0039 tsl2572
2-004b tmp112
## i2cdetect sweeps (only if i2c-tools is installed on the unit)
(i2cdetect not installed on the unit)
## kernel log i2c lines (is31fl3208@0x6F / tmp112@0x4B decision input)
[    1.234567] is31fl3236 0-0041: 12 RGB leds registered
[    1.567890] tsl2572 2-0039: ambient light sensor probed
EOF

cat > "$STUB/firmware-dirs.tsv" <<'EOF'
/system/vendor/firmware	/system/vendor/firmware
/vendor/firmware	missing
EOF

# --- fake adb ------------------------------------------------------------
cat > "$BIN/adb" <<'FAKE'
#!/usr/bin/env bash
# fake adb for test_stub.sh - canned donut unit answers (stub only, no network)
set -u
: "${STUB_LOG:?}"; : "${STUB_DIR:?}"
echo "adb $*" >>"$STUB_LOG"
[ "${1:-}" = "-s" ] && shift 2   # accept (and ignore) the explicit serial
cmd="${1:-}"
[ -n "$cmd" ] || exit 2
shift
case "$cmd" in
	connect) echo "connected to $*";;
	root) echo "adbd is already running as root";;
	shell)
		case "${1:-}" in
			sh)
				# device script arrives on stdin; dispatch on its first-line marker
				IFS= read -r first
				marker="${first#\# M0-DEV-SCRIPT: }"
				case "$marker" in
					byname) cat "$STUB_DIR/byname-output.txt";;
					wifiscan) awk -F'\t' -v miss="${STUB_MISSING_BLOB:-}" 'miss == "" || $1 != miss' "$STUB_DIR/wifiscan.tsv";;
					i2c) cat "$STUB_DIR/i2c-output.txt";;
					fwdirs) cat "$STUB_DIR/firmware-dirs.tsv";;
					*) echo "STUB-ADB: unknown device script: $first" >&2; exit 1;;
				esac
				cat > /dev/null   # drain the rest of the script
				;;
			*)
				case "$*" in
					"id -u") echo 0;;
					getprop) cat "$STUB_DIR/getprop.txt";;
					"cat /proc/cmdline") cat "$STUB_DIR/cmdline.txt";;
					"cat /proc/idme") cat "$STUB_DIR/idme.txt";;
					dmesg) cat "$STUB_DIR/dmesg.txt";;
					*) echo "STUB-ADB: no canned answer for shell: $*" >&2; exit 1;;
				esac
				;;
		esac
		;;
	pull)
		remote="${1:?}"; dest="${2:?}"
		case "$remote" in
			/system/vendor/firmware/*)
				mkdir -p "$(dirname "$dest")"
				printf 'STUB-BLOB %s\n' "$remote" > "$dest"
				;;
			/system/vendor/firmware)
				mkdir -p "$dest"
				for n in WIFI_RAM_CODE_MT7668.bin WIFI_RAM_CODE2_SDIO_MT7668.bin EEPROM_MT7668.bin wifi.cfg TxPwrLimit_MT76x8.dat mt7668_patch_e2_hdr.bin wlan_mt76x8_sdio.ko wlan_mt76x8_sdio_prealloc.ko btmtksdio.ko; do
					[ -n "${STUB_MISSING_BLOB:-}" ] && [ "$n" = "${STUB_MISSING_BLOB}" ] && continue
					printf 'STUB-BLOB %s/%s\n' "$remote" "$n" > "$dest/$n"
				done
				;;
			/dev/block/*)
				mkdir -p "$(dirname "$dest")"
				{
					printf 'STUB-CONTENT %s\n' "$remote"
					case "$remote" in
						*/mmcblk0p3|*/mmcblk0p5)
							echo 'STUB-LK-MARKERS kaeru amonet kamakiri swdl'
							echo 'fastboot boot recovery verify unlock oem bootimage'
							;;
						*/mmcblk0p8)
							echo 'boot update update_payload_in_clear slot_suffix=a retry_count=3'
							;;
						*/mmcblk0boot0)
							echo 'PRELOADER-STUB MTK preload bootrom'
							;;
					esac
				} > "$dest"
				;;
			*)
				echo "STUB-ADB: unexpected pull: $remote" >&2
				exit 1
				;;
		esac
		;;
	*)
		echo "STUB-ADB: unexpected subcommand: $cmd" >&2
		exit 1
		;;
esac
FAKE

# --- fake fastboot -------------------------------------------------------
cat > "$BIN/fastboot" <<'FAKE'
#!/usr/bin/env bash
# fake fastboot for test_stub.sh
set -u
: "${FBLOG:?}"
echo "fastboot $*" >>"$FBLOG"
case "${1:-}" in
	devices)
		echo "DONUTSTUB001	fastboot"
		;;
	-s)
		shift 2   # consume '-s <serial>' together
		case "${1:-}:${2:-}" in
			devices:*)
				echo "DONUTSTUB001	fastboot"
				;;
			getvar:all)
				cat <<'VARS'
(bootloader) kernel: lk
(bootloader) product: donut
(bootloader) unlocked: yes
(bootloader) secure: no
(bootloader) hw-revision: DVT
(bootloader) battery-soc-ok: yes
(bootloader) off-mode-charge: 1
(bootloader) current-slot: a
(bootloader) partition-size:boot_a: 0x1000000
(bootloader) partition-size:boot_b: 0x1000000
(bootloader) partition-size:swdl: 0x2000000
(bootloader) partition-size:lk_a: 0x100000
(bootloader) partition-size:misc: 0x100000
(bootloader) partition-type:boot_a: boot
(bootloader) partition-type:boot_b: boot
(bootloader) partition-type:swdl: ext4
OKAY
VARS
				;;
			getvar:*)
				v="${2:?}"
				case "$v" in
					unlocked) echo "(bootloader) unlocked: yes"; echo "OKAY";;
					secure) echo "(bootloader) secure: no"; echo "OKAY";;
					product) echo "(bootloader) product: donut"; echo "OKAY";;
					serialno) echo "(bootloader) serialno: DONUTSTUB001"; echo "OKAY";;
					current-slot) echo "(bootloader) current-slot: a"; echo "OKAY";;
					hw-revision) echo "(bootloader) hw-revision: DVT"; echo "OKAY";;
					battery-soc-ok) echo "(bootloader) battery-soc-ok: yes"; echo "OKAY";;
					off-mode-charge) echo "(bootloader) off-mode-charge: 1"; echo "OKAY";;
					partition-size:*) echo "(bootloader) $v: 0x1000000"; echo "OKAY";;
					partition-type:*) echo "(bootloader) $v: boot"; echo "OKAY";;
					*) echo "FAILED (remote: variable not found: $v)" >&2; exit 1;;
				esac
				;;
			*)
				echo "STUB-FASTBOOT: unexpected: $*" >&2
				exit 1
				;;
		esac
		;;
	*)
		echo "STUB-FASTBOOT: unexpected subcommand: $*" >&2
		exit 1
		;;
esac
FAKE
chmod +x "$BIN/adb" "$BIN/fastboot"

: > "$LOG"; : > "$FBLOG"
run_capture_rc() {  # run_capture_rc <logfile>... command runs with set +e
	local rc=0
	set +e
	"$@" 2>"$T/last.err"
	rc=$?
	set -e
	LAST_RC=$rc
}

# --- 1: refusal ----------------------------------------------------------
echo "test_stub: [1] refusal without a mode flag"
run_capture_rc "$M0/harvest.sh" --out "$T/out1"
[ "$LAST_RC" -ne 0 ] || fail "harvest.sh without flags must exit nonzero (got 0)"
grep -qi "refusing" "$T/last.err" || fail "refusal message missing"
[ -s "$LOG" ] && fail "refusal path issued adb commands"
[ ! -e "$T/out1" ] || fail "refusal path created the output dir"
ok "refusal: exit=$LAST_RC, zero adb invocations, nothing created"

# --- 1b: audit refusal -----------------------------------------------------
echo "test_stub: [1b] audit.sh refusal without a mode flag"
log_before=$(wc -l < "$LOG"); fblog_before=$(wc -l < "$FBLOG")
run_capture_rc "$M0/audit.sh" --out "$T/out1b"
[ "$LAST_RC" -ne 0 ] || fail "audit.sh without flags must exit nonzero (got 0)"
grep -qi "refusing" "$T/last.err" || fail "audit refusal message missing"
[ "$(wc -l < "$LOG")" = "$log_before" ] && [ "$(wc -l < "$FBLOG")" = "$fblog_before" ] ||
	fail "audit refusal path issued adb/fastboot commands"
[ ! -e "$T/out1b" ] || fail "audit refusal path created the output dir"
ok "audit refusal: exit=$LAST_RC, zero adb/fastboot invocations, nothing created"

# --- 2: dry-run ----------------------------------------------------------
echo "test_stub: [2] --dry-run prints the plan and touches nothing"
run_capture_rc "$M0/harvest.sh" --dry-run --out "$T/out2" > "$T/out2.plan"
[ "$LAST_RC" -eq 0 ] || fail "--dry-run must exit 0"
[ -s "$LOG" ] && fail "--dry-run must not invoke adb"
[ ! -e "$T/out2" ] || fail "--dry-run must not create the output dir"
grep -q "+ adb -s 192.168.10.155:5555 root" "$T/out2.plan" || fail "plan missing: adb root"
grep -q "192.168.10.155:5555" "$T/out2.plan" || fail "plan missing the default serial"
grep -q "userdata" "$T/out2.plan" || fail "plan missing: userdata pull note"
grep -q "manifest.py" "$T/out2.plan" || fail "plan missing: manifest.py"
grep -q "selector:" "$T/out2.plan" || fail "plan missing: boot-chain selector note"
grep -q "boot-chain partition" "$T/out2.plan" || fail "plan missing: boot-chain partition note"
grep -q "five MT7668 blobs" "$T/out2.plan" || fail "plan missing: five-blob pull note"
grep -q "FOUND-WHERE.txt" "$T/out2.plan" || fail "plan missing: FOUND-WHERE.txt note"
ok "dry-run: plan printed ($(( $(wc -l < "$T/out2.plan") )) lines), zero adb calls, no dir created"

# --- 3: --i-mean-it against the stub -------------------------------------
echo "test_stub: [3] --i-mean-it against the fake unit"
run_capture_rc "$M0/harvest.sh" --i-mean-it --out "$T/out3" > "$T/out3.log"
[ "$LAST_RC" -eq 0 ] || { cat "$T/last.err"; fail "harvest --i-mean-it must exit 0 (got $LAST_RC)"; }
head -1 "$LOG" | grep -q "connect" || fail "first adb call must be connect (got: $(head -1 "$LOG"))"
sed -n 2p "$LOG" | grep -q " root" || fail "second adb call must be root"
grep -q "shell id -u" "$LOG" || fail "root check (shell id -u) missing from the sequence"

for p in kb dkb lk_a tee1 lk_b tee2 expdb misc persist boot_a boot_b swdl userdata preloader mmcblk0boot0 mmcblk0boot1; do
	[ -s "$T/out3/partitions/$p.img" ] || fail "partitions/$p.img missing/empty"
done
grep -q "mmcblk0p3" "$T/out3/partitions/lk_a.img" ||
	fail "lk_a.img must be pulled from the _real resolved node (mmcblk0p3), not the decoy"
if grep -rq "ota-decoy" "$T/out3/partitions/"; then
	fail "a decoy target was pulled into partitions/"
fi
[ ! -e "$T/out3/partitions/system_a.img" ] || fail "system_a must not be harvested (not boot-chain)"
[ ! -e "$T/out3/partitions/cache.img" ] || fail "cache must not be harvested (not boot-chain)"
grep -q "lk_a" "$T/out3/evidence/decoy-map.txt" || fail "decoy map must record lk_a"
grep -q "preloader" "$T/out3/evidence/decoy-map.txt" || fail "decoy map must record preloader"
grep -q "decoy link" "$T/out3/evidence/decoy-map.txt" || fail "decoy map must label the decoy links as decoy link"
[ "$(awk -F'\t' '$1=="misc" {print $3}' "$T/out3/partitions.tsv")" = 1048576 ] ||
	fail "partitions.tsv misc size must be the stub's 1048576 (real-size evidence flow)"
[ "$(awk -F'\t' '$1=="boot_a" {print $3}' "$T/out3/partitions.tsv")" = 16777216 ] ||
	fail "partitions.tsv boot_a size wrong"
[ "$(awk -F'\t' '$1=="swdl" {print $3}' "$T/out3/partitions.tsv")" = 33554432 ] ||
	fail "partitions.tsv swdl size wrong"
for f in getprop.txt cmdline.txt idme.txt kernel-log.txt by-name-report.txt decoy-map.txt i2c-probe.txt wifi-blob-scan.tsv firmware-dirs.tsv; do
	[ -s "$T/out3/evidence/$f" ] || fail "evidence/$f missing/empty"
done
grep -q "tmp112" "$T/out3/evidence/i2c-probe.txt" || fail "i2c probe must carry the tmp112 evidence"
grep -q "is31fl3236" "$T/out3/evidence/i2c-probe.txt" || fail "i2c probe must carry the ring driver evidence"
for b in WIFI_RAM_CODE_MT7668.bin WIFI_RAM_CODE2_SDIO_MT7668.bin EEPROM_MT7668.bin wifi.cfg TxPwrLimit_MT76x8.dat; do
	[ -s "$T/out3/wifi-firmware/$b" ] || fail "wifi-firmware/$b missing"
	grep -q "$b" "$T/out3/wifi-firmware/FOUND-WHERE.txt" || fail "FOUND-WHERE.txt missing $b"
	grep -q "^$b	/system/vendor/firmware" "$T/out3/wifi-firmware/FOUND-WHERE.txt" ||
		fail "FOUND-WHERE.txt must record where $b was found"
done
[ -s "$T/out3/firmware/system-vendor-firmware/wifi.cfg" ] || fail "firmware snapshot missing wifi.cfg"
[ -s "$T/out3/firmware/system-vendor-firmware/wlan_mt76x8_sdio.ko" ] || fail "firmware snapshot missing the wlan module"
[ -s "$T/out3/manifest.txt" ] && [ -s "$T/out3/manifest.json" ] || fail "manifest.txt/.json missing"
python3 "$M0/manifest.py" --check "$T/out3" >/dev/null ||
	fail "manifest check failed on the fresh harvest"
grep -q "partitions/userdata.img" "$T/out3/manifest.json" || fail "manifest.json must list userdata.img"
ok "harvest --i-mean-it: sequence, decoy-aware pulls, five blobs + locations, GPT sizes, manifest green"

# --- 3b: missing blob -> exit 3 ------------------------------------------
echo "test_stub: [3b] missing device-only blob -> exit 3, evidence still lands"
export STUB_MISSING_BLOB=WIFI_RAM_CODE_MT7668.bin
run_capture_rc "$M0/harvest.sh" --i-mean-it --out "$T/out3b" > "$T/out3b.log" 2>&1
unset STUB_MISSING_BLOB
[ "$LAST_RC" -eq 3 ] || fail "missing blob must exit 3 (got $LAST_RC)"
grep -q "MISSING device-only blobs: WIFI_RAM_CODE_MT7668.bin" "$T/last.err" || fail "missing-blob warning missing"
[ -s "$T/out3b/wifi-firmware/wifi.cfg" ] || fail "the other four blobs must still be pulled"
[ -s "$T/out3b/manifest.txt" ] || fail "manifest must still be written on partial harvest"
[ ! -s "$T/out3b/wifi-firmware/WIFI_RAM_CODE_MT7668.bin" ] || fail "missing blob must not appear"
ok "missing-blob path: exit 3 with loud warning, partial evidence + manifest intact"

# --- 4: audit --dry-run --------------------------------------------------
echo "test_stub: [4] audit --dry-run"
log_before=$(wc -l < "$LOG"); fblog_before=$(wc -l < "$FBLOG")
run_capture_rc "$M0/audit.sh" --dry-run --out "$T/out4" > "$T/out4.plan"
[ "$LAST_RC" -eq 0 ] || fail "audit --dry-run must exit 0"
[ "$(wc -l < "$LOG")" = "$log_before" ] && [ "$(wc -l < "$FBLOG")" = "$fblog_before" ] ||
	fail "audit --dry-run must not invoke adb/fastboot"
[ ! -e "$T/out4" ] || fail "audit --dry-run must not create the output dir"
grep -q "getvar all" "$T/out4.plan" || fail "audit plan missing: getvar all"
grep -q "no reboot, no unlock" "$T/out4.plan" || fail "audit plan missing the passivity note"
grep -q "pull every lk\* by-name partition" "$T/out4.plan" || fail "audit plan missing: lk pull note"
grep -q "misc -> " "$T/out4.plan" || fail "audit plan missing: misc pull note"
ok "audit dry-run: plan printed incl. lk*/misc pulls, zero adb/fastboot calls"

# --- 5: audit --i-mean-it (fake fastboot reachable) -----------------------
echo "test_stub: [5] audit --i-mean-it with fastboot reachable"
printf 'AMZN stock LK reference strings: boot verify bootimage partition\nconsole ttyMT0\n' > "$T/stock-lk.img"
cat > "$T/unit-extra-lk.img" <<'EOF'
STUB-EXTRA-LK patched strings: kaeru kamakiri swdl fastboot oem
EOF
cat > "$T/notes.txt" <<'EOF'
usb_pads_tm4_tm5=soldered test wires on TM4/TM5
vbus_source=barrel adapter only
uart_console=TX only on test pads
case_seal=opened and resealed
evidence_photos=yes (bench log)
EOF
run_capture_rc "$M0/audit.sh" --i-mean-it --out "$T/out5" --stock "$T/stock-lk.img" --lk "$T/unit-extra-lk.img" --notes-file "$T/notes.txt" > "$T/out5.log"
[ "$LAST_RC" -eq 0 ] || { cat "$T/last.err"; fail "audit --i-mean-it must exit 0 (got $LAST_RC)"; }
grep -q "getvar all" "$FBLOG" || fail "fastboot getvar all not swept"
[ -s "$T/out5/fastboot-getvar-all.txt" ] || fail "fastboot-getvar-all.txt missing"
grep -q "unlocked: yes" "$T/out5/fastboot-getvar-all.txt" || fail "getvar all capture missing unlocked"
grep -q "## getvar unlocked" "$T/out5/fastboot-getvars.txt" || fail "targeted getvar capture missing"
[ -s "$T/out5/lk/lk_a.img" ] || fail "audit must pull lk_a"
[ -s "$T/out5/misc/misc.img" ] || fail "audit must pull misc"
grep -q "kaeru: PRESENT" "$T/out5/audit-report.md" || fail "marker table must report kaeru PRESENT"
grep -q "kamakiri: PRESENT" "$T/out5/audit-report.md" || fail "marker table must report kamakiri PRESENT"
grep -q "swdl: PRESENT" "$T/out5/audit-report.md" || fail "marker table must report swdl PRESENT"
grep -q "DIFFERENT" "$T/out5/audit-report.md" || fail "stock compare must report DIFFERENT for a patched LK"
grep -q "unit-extra-lk.img" "$T/out5/audit-report.md" || fail "--lk extra image must be analyzed"
grep -q "retry_count=3" "$T/out5/audit-report.md" || fail "misc evidence (slot tries) missing from the report"
grep -q "soldered test wires on TM4/TM5" "$T/out5/audit-report.md" || fail "manual fields missing from the report"
grep -q "usb_pads_tm4_tm5=soldered test wires on TM4/TM5" "$T/out5/manual-fields.txt" || fail "manual-fields.txt wrong"
grep -q "it does NOT judge" "$T/out5/audit-report.md" || fail "report must carry the non-judging note"
python3 "$M0/manifest.py" --check "$T/out5" >/dev/null || fail "audit manifest check failed"
ok "audit --i-mean-it: sweep captured, markers vs stock stub, misc evidence, manual fields, manifest green"

# --- 5b: real stock lk.img compare (skipped when absent) -------------------
echo "test_stub: [5b] stock compare against the real ota-donut lk.img"
if [ -f "$STOCK_DEFAULT" ]; then
	run_capture_rc "$M0/audit.sh" --harvest "$T/out3" --stock "$STOCK_DEFAULT" --out "$T/out5b" > /dev/null
	[ "$LAST_RC" -eq 0 ] || fail "offline audit vs real stock must exit 0"
	grep -q "ota_out/lk.img" "$T/out5b/audit-report.md" || fail "real stock reference not recorded"
	grep -q "DIFFERENT" "$T/out5b/audit-report.md" || fail "stub LK vs real stock must be DIFFERENT"
	ok "real stock reference used ($(wc -c < "$STOCK_DEFAULT") bytes); stub LK correctly DIFFERENT"
else
	ok "SKIPPED (stock lk.img not present at $STOCK_DEFAULT)"
fi

# --- 6: audit --harvest offline -------------------------------------------
echo "test_stub: [6] audit --harvest (offline, zero device calls)"
log_before=$(wc -l < "$LOG"); fblog_before=$(wc -l < "$FBLOG")
run_capture_rc "$M0/audit.sh" --harvest "$T/out3" --stock "$T/stock-lk.img" --out "$T/out6" > "$T/out6.log"
[ "$LAST_RC" -eq 0 ] || { cat "$T/last.err"; fail "audit --harvest must exit 0 (got $LAST_RC)"; }
[ "$(wc -l < "$LOG")" = "$log_before" ] && [ "$(wc -l < "$FBLOG")" = "$fblog_before" ] ||
	fail "offline audit must not invoke adb/fastboot"
grep -q "offline" "$T/out6/audit-report.md" || fail "offline report must say so"
grep -q "kaeru: PRESENT" "$T/out6/audit-report.md" || fail "offline marker check failed (harvest lk_a.img carries the markers)"
grep -q "retry_count=3" "$T/out6/audit-report.md" || fail "offline misc evidence missing"
grep -q "offline re-analysis: fastboot not queried" "$T/out6/audit-report.md" || fail "offline fastboot note missing"
ok "audit --harvest: report built from the harvest dir, zero device calls"

# --- 7: manifest tamper detection ------------------------------------------
echo "test_stub: [7] manifest --check catches tampering"
printf 'tampered\n' >> "$T/out3/wifi-firmware/wifi.cfg"
run_capture_rc python3 "$M0/manifest.py" --check "$T/out3"
[ "$LAST_RC" -ne 0 ] || fail "tampered file must fail --check"
grep -q "hash mismatch: wifi-firmware/wifi.cfg" "$T/last.err" || fail "tamper report wrong"
ok "manifest --check: tampered file detected (exit $LAST_RC)"

# --- 8: name-for-name blob sync --------------------------------------------
echo "test_stub: [8] five-blob list in sync with donut-firmware-links.sh"
[ -f "$FWLINKS" ] || fail "rootfs/usr/lib/firmware/donut-firmware-links.sh missing"
FIVE=(WIFI_RAM_CODE_MT7668.bin WIFI_RAM_CODE2_SDIO_MT7668.bin EEPROM_MT7668.bin wifi.cfg TxPwrLimit_MT76x8.dat)
for n in "${FIVE[@]}"; do
	grep -q -- "$n" "$M0/harvest.sh" || fail "harvest.sh lost $n"
	grep -q -- "$n" "$FWLINKS" || fail "donut-firmware-links.sh lost $n"
done
# and the harvest list has exactly those five (no drift)
for n in $(grep -A7 '^WIFI_BLOBS=(' "$M0/harvest.sh" | grep -oE '^[[:space:]]+[A-Za-z0-9_.]+\.(bin|cfg|dat)$' | sed 's/^[[:space:]]*//'); do
	case " ${FIVE[*]} " in
		*" $n "*) ;;
		*) fail "harvest.sh WIFI_BLOBS carries an out-of-sync name: $n";;
	esac
done
ok "name-for-name sync: five blobs identical in harvest.sh and donut-firmware-links.sh"

echo "test_stub: ALL GREEN"
