# wpa_supplicant for donut (recipe)

The test image ships Alpine's stock `wpa_supplicant`.  Do not use it to join
WPA2 networks on this device: with Amazon's `wlan_mt76x8_sdio` driver the
association never completes.  The driver puts its own RSNE (RSN Capabilities
0) into the association request, while wpa_supplicant 2.11 advertises 16
PTKSA replay counters in the RSNE of EAPOL-Key message 2/4 whenever the
association uses WMM; the access point sees two different RSNEs and drops
the station after message 2/4 (reason 2).  wpa_supplicant 2.10 and Fire OS's
2.6 send 0 in both and join.  Bench-tested 2026-10-23 (techo5,
`docs/hardware-donut.md`).

## Build

The recipe is techo5's `tools/linux/build-wpa-supplicant.sh`
(`/home/am/Work/SRE-Device/tools/techo5-dot`, "wpa_supplicant 2.11 built the
way Alpine v3.24's `wpa_supplicant-2.11-r4` is") with the donut deltas:

1. Keep the same wpa_supplicant 2.11 source pin, checksum, Alpine -dev
   sysroot packages and `alpine-config`, and this tree's
   `../wpa-patches/0100-donut-no-wmm-replay-counters.patch` (byte-identical
   to techo5's, md5 `7b614b6e8a13d039fbf62169b35f9a49`).
2. Cross-compile for **aarch64 musl**, not armv7 (donut runs an arm64
   kernel and Alpine aarch64 userspace):
   - cross toolchain: `aarch64-linux-musl` from musl.cc (or the
     `aarch64-linux-gnu-gcc --sysroot` in the donut-build container, which
     links glibc - only acceptable for a test image),
   - Alpine sysroot arch path `…/main/aarch64` instead of `…/main/armv7`,
   - `ARCH_FLAGS="-march=armv8-a"` replacing the armv7 arch flags.
3. Verify before installing: `grep -c "RSN_NUM_REPLAY_COUNTERS_16 << 2"
   src/rsn_supp/wpa_ie.c` must print `0`, and the binary's `readelf -h`
   Machine must be `AArch64`.

Install the stripped `wpa_supplicant` into the running test system at
`/usr/sbin/wpa_supplicant` (over the Alpine package's; it is
command-line-compatible - no config change needed).

## Why not patch it into the image build

The image build (`tools/mk-donut-testimg.sh`) installs the Alpine package:
the patch exists to fix a *driver-vs-supplicant* mismatch that only matters
once WiFi firmware is harvested from the device (M0) and the network is
actually joined (M4).  Rebuild and install the patched binary at that point;
until then the recipe is dormant.
