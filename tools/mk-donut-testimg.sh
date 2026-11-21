#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build a bring-up test filesystem for donut: kernel + DTB + a small Alpine
# initramfs with a root shell on the debug UART (ttyS0, test pads) and on a
# USB CDC ACM gadget serial (ttyGS0), packed as an ext4 image that U-Boot
# boots from the userdata partition via extlinux.
#
# Runs inside the donut-build container after `build.sh linux donut` and
# `build.sh wifi donut`:
#   build.sh testimg donut        -> /work/out/donut-testimg.ext4
#
# MISC_SIZE (bytes, decimal or 0xhex) sizes the misc partition for
# fw_env.config (two 64 KiB copies at its top, as U-Boot's
# ENV_MMC_USE_SW_PARTITION places the environment).  TODO(M0): donut's real
# misc size is unknown until the GPT is read from the device; the default is
# lidar's 512 KiB, a placeholder - override or fix it when M0 knows.
#
# Flash with U-Boot fastboot (hold Volume Up at power-on), after arming the
# bootcount fallback:
#   fw_setenv upgrade_available 1; fw_setenv bootcount 0
#   fastboot flash userdata out/donut-testimg.ext4
# The initramfs of a good boot disarms it again
# (fw_setenv upgrade_available 0; fw_setenv bootcount 0).
set -euo pipefail
B=${1:-/src/build/donut}
REL=https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64
OUT=/work/out
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

[ -f "$OUT/wifi/wlan_mt76x8_sdio.ko" ] ||
	{ echo "run: build.sh wifi donut first (need out/wifi/wlan_mt76x8_sdio.ko)" >&2; exit 1; }

mkdir -p "$T/initramfs" "$T/fs/boot/extlinux"
ROOTFS=$(curl -fsSL "$REL/latest-releases.yaml" |
	sed -n 's/^ *file: \(alpine-minirootfs-.*-aarch64\.tar\.gz\)$/\1/p' | head -n1)
[ -n "$ROOTFS" ] || { echo "could not resolve Alpine minirootfs" >&2; exit 1; }
curl -fsSL "$REL/$ROOTFS" | tar -xz -C "$T/initramfs"
python3 /work/tools/alpine-fetch.py "$T/initramfs" alsa-utils i2c-tools evtest iw wpa_supplicant \
	wireless-regdb busybox-extras uboot-tools libgpiod alsa-ucm-conf

# fw_printenv/fw_setenv: U-Boot keeps its environment in two 64 KiB copies
# at the top of misc (ENV_MMC_USE_SW_PARTITION).  TODO(M0): replace the
# MISC_SIZE default with donut's real misc partition size.
MISC_SIZE=${MISC_SIZE:-524288}
MISC_SIZE=$(printf '%d' "$MISC_SIZE")
[ "$MISC_SIZE" = 524288 ] &&
	echo "testimg: MISC_SIZE defaults to lidar's 512 KiB - TODO(M0): donut's real misc size" >&2
printf '/dev/disk/by-partlabel/misc 0x%x 0x10000\n/dev/disk/by-partlabel/misc 0x%x 0x10000\n' \
	$((MISC_SIZE - 0x10000)) $((MISC_SIZE - 0x20000)) \
	> "$T/initramfs/etc/fw_env.config"

# MT7668 ROM patch from linux-firmware (redistributable), for Bluetooth and
# Wi-Fi alike (see donut-firmware-links.sh for the names each side asks for)
LF=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
mkdir -p "$T/initramfs/usr/lib/firmware/mediatek"
curl -fsSL "$LF/mediatek/mt7668pr2h.bin?h=20261019" -o "$T/initramfs/usr/lib/firmware/mediatek/mt7668pr2h.bin"
echo "465ed634c61b46b34bb21240736f33d3d347fefec7cbfbd1cb6e63334aec4cc9  $T/initramfs/usr/lib/firmware/mediatek/mt7668pr2h.bin" |
	sha256sum -c --quiet
# the MT7668 firmware links (and the M0 harvest file list)
install -D -m755 /work/rootfs/usr/lib/firmware/donut-firmware-links.sh \
	"$T/initramfs/usr/lib/firmware/donut-firmware-links.sh"
"$T/initramfs/usr/lib/firmware/donut-firmware-links.sh" "$T/initramfs/usr/lib/firmware" 2>"$T/fwmissing" || true
grep -q "WIFI_RAM_CODE_MT7668.bin missing" "$T/fwmissing" &&
	echo "testimg: WLAN RAM firmware absent (expected until the M0 harvest)" >&2
# the softdep that orders the combo chip's drivers
install -D -m644 /work/rootfs/etc/modprobe.d/donut.conf "$T/initramfs/etc/modprobe.d/donut.conf"

make -C /src/linux O="$B" -s INSTALL_MOD_PATH="$T/initramfs" INSTALL_MOD_STRIP=1 modules_install
KREL=$(cat "$B/include/config/kernel.release")
# Out-of-tree MT7668 WLAN driver from build.sh wifi, without its debug info:
# 7.7 MB -> ~2 MB, recorded because the initramfs is unpacked into RAM
install -D -m644 "$OUT/wifi/wlan_mt76x8_sdio.ko" "$T/initramfs/lib/modules/$KREL/extra/wlan_mt76x8_sdio.ko"
echo "testimg: wlan_mt76x8_sdio.ko $(stat -c%s "$OUT/wifi/wlan_mt76x8_sdio.ko") bytes" \
	"-> $(aarch64-linux-gnu-strip --strip-debug -o "$T/wifi-stripped.ko" "$OUT/wifi/wlan_mt76x8_sdio.ko" && stat -c%s "$T/wifi-stripped.ko") bytes (strip-debug)" >&2
mv "$T/wifi-stripped.ko" "$T/initramfs/lib/modules/$KREL/extra/wlan_mt76x8_sdio.ko"
depmod -b "$T/initramfs" "$KREL"
# Keep only the modules this board uses (tools/donut-initramfs-modules.txt)
# and what they depend on; then index again.
python3 - "$T/initramfs/lib/modules/$KREL" /work/tools/donut-initramfs-modules.txt <<'EOF'
import os, re, sys
mdir, listfile = sys.argv[1], sys.argv[2]
norm = lambda n: os.path.basename(n).split(".ko")[0].replace("-", "_")
deps, path = {}, {}
for line in open(os.path.join(mdir, "modules.dep")):
    mod, rest = line.split(":", 1)
    path[norm(mod)] = mod
    deps[norm(mod)] = [norm(d) for d in rest.split()]
for line in open(os.path.join(mdir, "modules.softdep")):
    m = re.match(r"softdep (\S+) (.*)", line)
    if m:
        deps.setdefault(norm(m[1]), []).extend(
            norm(w) for w in m[2].split() if not w.endswith(":"))
want = [norm(l.strip()) for l in open(listfile) if l.strip() and not l.strip().startswith("#")]
missing = [w for w in want if w not in path]
if missing:
    sys.exit("donut-initramfs-modules.txt: not built: " + " ".join(missing))
whole = ("kernel/net/", "kernel/crypto/", "kernel/lib/", "kernel/arch/", "extra/")
# the whole-kept modules need their dependencies too
keep, todo = set(), list(want) + [m for m, p in path.items() if p.startswith(whole)]
while todo:
    m = todo.pop()
    if m in path and m not in keep:
        keep.add(m)
        todo += deps.get(m, [])
removed = 0
for m, p in path.items():
    if m not in keep and not p.startswith(whole):
        os.remove(os.path.join(mdir, p))
        removed += 1
for root, dirs, files in os.walk(mdir, topdown=False):
    if root != mdir and not os.listdir(root):
        os.rmdir(root)
print(f"modules: kept {len(path) - removed} of {len(path)}")
EOF
depmod -b "$T/initramfs" "$KREL"

# The card's UCM profile (alsa-ucm-conf layout)
U="$T/initramfs/usr/share/alsa/ucm2"
install -D -m644 -t "$U/mt8516-donut" /work/ucm2/mt8516-donut/*.conf
mkdir -p "$U/conf.d/mt8516-donut"
ln -sf ../../mt8516-donut/mt8516-donut.conf "$U/conf.d/mt8516-donut/mt8516-donut.conf"

# Login shell on the debug UART, respawned by BusyBox init, which also makes
# the tty the shell's controlling terminal.  The ttyGS0 shell is appended by
# init once the USB gadget is up.
cat > "$T/initramfs/etc/inittab" <<'EOF'
ttyS0::respawn:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
EOF

cat > "$T/initramfs/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /sys/kernel/config && mount -t devpts devpts /dev/pts
mount -t configfs configfs /sys/kernel/config 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
# Previous boot's console/oops log, if it was a warm reboot
mount -t pstore pstore /sys/fs/pstore 2>/dev/null
echo "donut test initramfs: $(uname -r)"
# No udev: name the GPT partitions ourselves (fw_env.config uses misc)
mkdir -p /dev/disk/by-partlabel /run
for i in $(seq 1 25); do
	for u in /sys/class/block/*/uevent; do
		n=$(sed -n 's/^PARTNAME=//p' "$u")
		[ -n "$n" ] && ln -sf "/dev/$(basename "$(dirname "$u")")" "/dev/disk/by-partlabel/$n"
	done
	[ -b /dev/disk/by-partlabel/misc ] && break
	sleep 0.2
done
# Disarm the bootcount fallback: this userspace came up (T6 model - a kernel
# that never reaches here trips bootlimit and U-Boot stops in fastboot)
if fw_setenv upgrade_available 0 2>/dev/null && fw_setenv bootcount 0 2>/dev/null; then
	echo "donut: bootcount disarmed (good boot)" > /dev/kmsg
else
	echo "donut: fw_setenv failed - check fw_env.config against the real misc size" > /dev/kmsg
fi
# Firmware links for the MT7668 combo (idempotent; lists what M0 must
# still harvest)
/usr/lib/firmware/donut-firmware-links.sh 2>/dev/kmsg
# Sound card, no udev: codecs and the accdet before the machine driver
for m in snd-soc-mt8516-afe snd-soc-tas2770 snd-soc-tlv320adc3xxx \
	snd-soc-mt8167-codec extcon-mt8167-accdet snd-soc-mt8516-amazon-donut; do
	modprobe $m || echo "donut: modprobe $m failed" > /dev/kmsg
done
# Base audio setup from the card's UCM (BootSequence: amps safe, mics up)
for i in $(seq 1 25); do
	grep -q mt8516donut /proc/asound/cards 2>/dev/null && break
	sleep 0.2
done
if alsaucm -c hw:mt8516donut set _boot '' 2>/dev/null; then
	echo "donut: UCM boot sequence applied; verbs: Speaker (96k), Headphones (48k)" > /dev/kmsg
else
	echo "donut: alsaucm _boot failed - check /proc/asound/cards" > /dev/kmsg
fi
# Wi-Fi/BT is manual: the WLAN RAM firmware is not on the image (M0
# harvest).  With it staged: modprobe wlan_mt76x8_sdio, then
# modprobe btmtksdio (the softdep enforces the order).
# USB gadget on the test-pad port: ACM console + NCM network (169.254.8.2)
g=/sys/kernel/config/usb_gadget/donut
if [ -d /sys/kernel/config/usb_gadget ] && [ -n "$(ls /sys/class/udc 2>/dev/null)" ]; then
	mkdir -p $g/strings/0x409 $g/configs/c.1 $g/functions/acm.usb0 $g/functions/ncm.usb0
	echo 0x1d6b > $g/idVendor; echo 0x0104 > $g/idProduct
	echo donut-mainline > $g/strings/0x409/manufacturer
	echo "Echo Dot 3 (donut) test" > $g/strings/0x409/product
	ln -s $g/functions/acm.usb0 $g/configs/c.1/
	ln -s $g/functions/ncm.usb0 $g/configs/c.1/
	ls /sys/class/udc | head -n1 > $g/UDC
	ip link set lo up
	ip addr add 169.254.8.2/16 dev usb0 2>/dev/null && ip link set usb0 up
	# The USB serial console exists now: let init run a shell on it too
	echo 'ttyGS0::respawn:-/bin/sh' >> /etc/inittab
fi
# BusyBox init becomes PID 1: it runs the shells from /etc/inittab and reaps
# orphaned processes (a plain shell as PID 1 leaves them as zombies).
exec /bin/busybox init
EOF
chmod +x "$T/initramfs/init"

(cd "$T/initramfs" && find . | cpio -o -H newc --quiet | gzip -9) > "$T/fs/boot/initramfs.gz"
cp "$B/arch/arm64/boot/Image.gz" "$T/fs/boot/Image.gz"
install -D -m644 "$B/arch/arm64/boot/dts/mediatek/mt8516-amazon-donut.dtb" \
	"$T/fs/boot/mediatek/mt8516-amazon-donut.dtb"
install -D -m644 /work/rootfs/boot/extlinux/extlinux.conf "$T/fs/boot/extlinux/extlinux.conf"
# The overlay's non-boot files also live on the ext4 itself (the durable
# on-disk copy; runtime uses the initramfs copies)
install -D -m755 /work/rootfs/usr/lib/firmware/donut-firmware-links.sh \
	"$T/fs/usr/lib/firmware/donut-firmware-links.sh"
install -D -m644 "$T/initramfs/usr/lib/firmware/mediatek/mt7668pr2h.bin" \
	"$T/fs/usr/lib/firmware/mediatek/mt7668pr2h.bin"
ln -snf mediatek/mt7668pr2h.bin "$T/fs/usr/lib/firmware/mt7668_patch_e2_hdr.bin"
install -D -m644 /work/rootfs/etc/modprobe.d/donut.conf "$T/fs/etc/modprobe.d/donut.conf"

rm -f "$OUT/donut-testimg.ext4"
mke2fs -q -t ext4 -L donut-test -d "$T/fs" "$OUT/donut-testimg.ext4" 256M
ls -la "$OUT/donut-testimg.ext4"
