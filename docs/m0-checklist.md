# M0 execution checklist — donut unit audit + harvest (no flashing)

Unit: rooted Amazon Echo Dot 3 (`donut`, NS6529-era) at **192.168.10.155**
(root adb) — the **production WHA oracle**, the only unit. Everything here is
read-only on the device; the first flash happens after M0's done-when is met
and `docs/restore.md`'s rehearsal is green. M0 done-when (design §7):
*unlock state determined; all firmware files + IDME + slot images pulled and
hashed; restore path written + rehearsed (no flash yet).*

Kit: `tools/m0/` — `harvest.sh`, `audit.sh`, `manifest.py` (guarded: nothing
runs against the unit without `--i-mean-it`; `--dry-run` prints the plan;
`test_stub.sh` is the stub regression, no device involved).

## 0. Preconditions (host)

- [ ] adb + fastboot (platform-tools) on PATH; `adb devices` shows
      `192.168.10.155:5555 device` (or `adb connect 192.168.10.155:5555`).
- [ ] `python3` on PATH (manifest.py is stdlib-only); `strings` (binutils)
      for the audit marker check; GNU coreutils; ~2 GB free disk.
- [ ] Stock reference present: `/home/am/Work/SRE-Device/ota-donut/2019/ota_out/lk.img`
      (audit's compare default; hashes in `docs/restore.md`).
- [ ] Repo fresh on the M1 commit; work dir for evidence chosen (outside the
      repo — harvest dirs are ~2 GB and are raw device evidence, never
      committed).

## 1. Audit (unlock state, passive — never reboots/unlocks the unit)

```bash
tools/m0/audit.sh --dry-run          # review the plan
tools/m0/audit.sh --i-mean-it        # full audit (interactive manual fields)
```

Produces `m0-audit-<ts>/audit-report.md` (+ fastboot sweep when reachable,
LK marker table vs the stock reference, slot map, misc evidence,
`manual-fields.txt`, manifest).

Answer the manual fields (or pass `--notes-file` with `key=value` lines;
keys: `usb_pads_tm4_tm5`, `vbus_source`, `uart_console`, `case_seal`,
`evidence_photos`).

**Decisions this feeds:**
- patched-LK markers present (kaeru/kamakiri/swdl unit-only strings) →
  deploy via root-shell `dd` is viable (design §8); absent → plan for the
  kamakiri-donut kit first.
- fastboot `getvar` sweep (only if the unit is already in the bootloader) →
  unlock/slot facts; not reachable is the normal Fire OS state, not a
  verdict.
- USB-pad fields → kamakiri BROM-entry feasibility (Method 2 fallback).

## 2. Harvest (the evidence pass)

```bash
tools/m0/harvest.sh --dry-run                  # review the plan
tools/m0/harvest.sh --i-mean-it --out /path/to/m0-harvest-<date>
```

Pulls: the **five device-only MT7668 files** (`WIFI_RAM_CODE_MT7668.bin`,
`WIFI_RAM_CODE2_SDIO_MT7668.bin`, `EEPROM_MT7668.bin`, `wifi.cfg`,
`TxPwrLimit_MT76x8.dat` — searched in `/system/vendor/firmware` and
`/vendor/firmware`, every hit's location recorded in
`wifi-firmware/FOUND-WHERE.txt`), the full `/system/vendor/firmware`
snapshot, IDME, getprop, cmdline, kernel log, the **GPT evidence** (raw
by-name listings, per-name resolved-node + `blockdev --getsize64` sizes in
`partitions.tsv`, `/proc/partitions`, major:minor listings, decoy map),
**every boot-chain partition image** (both slots + swdl + the LK partitions
+ misc + full 1.5 GB userdata + the rest of the selector, pulled from
resolved nodes with decoy-aware `_real` preference) and the **i2c probe**
(best-effort). Files hash into `manifest.txt` + `manifest.json`.

A missing device-only blob makes harvest exit **3** with a loud warning —
M0 is not done until all five are in hand and hashed. Expected blob home:
`/system/vendor/firmware` (vendor layout; techo5 docs).

## 3. Verify the harvest

```bash
python3 tools/m0/manifest.py --check /path/to/m0-harvest-<date>
```

- [ ] manifest check green; `wifi-firmware/` holds all five blobs;
      `FOUND-WHERE.txt` records where each was found.
- [ ] `partitions/` holds both boot slots, swdl, the LK partitions (incl.
      any `_real`-resolved copies), misc, userdata, tee1/tee2, expdb,
      persist, kb/dkb, `mmcblk0boot0` (preloader).
- [ ] `evidence/decoy-map.txt` reviewed (amonet-TWRP decoy by-name links;
      `<name>_real` is authoritative).
- [ ] i2c probe captured (i2c-detect via sysfs names + kernel log lines).

## 4. Evidence routing — what lands where

| Evidence | Feeds | How |
|---|---|---|
| `partitions.tsv` misc size (bytes) | **T9's `MISC_SIZE` TODO** in `tools/mk-donut-testimg.sh` (fw_env.config = two 64 KiB U-Boot env copies at the top of misc) | set `MISC_SIZE=<size>` / fix the default; sanity-check the env offsets fit inside the real misc |
| `partitions.tsv` boot_a/boot_b sizes | **`tools/donutboot.py` `TODO(M0)`** boot-slot `max_size` (16 MiB placeholder) | replace the placeholder from the real GPT sizes before packing any boot image |
| `partitions.tsv` swdl size | swdl variant bound (32 MB expected, p12) | confirm the packer bound |
| `wifi-firmware/*.bin/.cfg/.dat` + `FOUND-WHERE.txt` | **T9 firmware staging**: `rootfs/usr/lib/firmware/donut-firmware-links.sh` expects them **name-for-name** next to itself | copy the five files into `rootfs/usr/lib/firmware/` (keep the exact names); the links script stops warning; rebuild the testimg |
| `evidence/i2c-probe.txt` | **IS31FL3208 decision (design §9)**: LED ctrl 2 @ i2c0 0x6F gets a node + port **only if populated** on this board rev; TMP112 @ 0x4B included iff populated | read the sysfs device names / kernel-log lines; record the verdict in the M0 report (expected: techo5-family units show is31fl3236 only) |
| audit report + `manual-fields.txt` | **deploy-path decision** (dd vs kamakiri-first) and restore rehearsal feasibility | recorded in the M0 report; see `docs/restore.md` |
| `evidence/decoy-map.txt` + `partitions.tsv` | restore write targets | `docs/restore.md` rules: write to resolved nodes only |
| `evidence/getprop.txt` (ro.build.version) | NS6529-era confirmation | note the exact Fire OS version in the M0 report |
| `partitions/userdata.img` | WHA-oracle provisioned state (restore-with-state) | keep two copies; operator question (provisioned vs factory-level restore bar) stays open |

## 5. Done-when gate (M0 → M1 handoff)

- [ ] Unlock state determined (audit report + recorded decision).
- [ ] All firmware files + IDME + slot images pulled and hashed (manifest
      green; five blobs present; userdata image on two disks).
- [ ] Restore path written (`docs/restore.md`) **and rehearsed** — its
      8-point checklist fully signed off, no flash performed.
- [ ] M0 report written: evidence locations, the §4 routing results
      (MISC_SIZE number, boot-slot sizes, i2c verdict), deploy-path
      decision, manual-field answers, operator questions still open.
