# donut-mainline: mainline Linux and U-Boot for the Amazon Echo Dot 3

Workspace for running mainline Linux **v7.3** and U-Boot **v2026.10** on the
Amazon Echo Dot 3rd gen (codename "donut", MediaTek MT8516). It stacks a
donut overlay layer on top of the [lidar-mainline](../lidar-mainline)
overlays (Echo Plus 2nd gen, same SoC, lidar's exact pins).

**Status: M1 software track complete** (commits `5a365da..767462f`). Kernel
+ DTS + audio card + MT76x8 WiFi module + U-Boot board + LK payload packers
(boot and swdl variants) + Alpine test image + M0 harvest/audit/restore kit
all build and pass their gates in the container — no hardware access was
used or needed. Plan: `../../recovery-notes/donut-mainline-m1-plan.md`;
per-task reports + ledger: `../../.superpowers/sdd/donut-mainline-m1-plan/`
(`task-1-report.md` … `task-10-report.md`, `progress.md`); design and
hardware truth: `../../recovery-notes/donut-mainline-design.md`.

## Layout

- `lidar-linux/`, `lidar-uboot/`, `lidar-wifi/`, `lidar.config`, and the
  `tools/lidar-*` files — verbatim copies from `../lidar-mainline` (the base
  overlay layer; lidar stays the upstream for these, do not diverge).
- `donut-linux/`, `donut-uboot/`, `donut-wifi/`, `donut.config` — the donut
  overlay layer. Applied after the lidar overlays: a file present in both
  layers ends up with the donut version, so donut copies of shared files
  (e.g. `sound/soc/mediatek/Makefile`, dts `Makefile`) carry lidar's content
  plus donut additions, never a replacement. Pinned by
  `tools/donut-selftest.sh` and the sync order in `build.sh`.
- `crumpet-linux/`, `crumpet.config` — the **crumpet** overlay layer (Echo
  Dot 3rd Gen Refresh, raw NAND; third and last layer, same
  shadow-never-replace rule — pinned by the 3-layer selftest). Contains the
  mt8167 NFC/ECC driver variant, the crumpet board DTS + audio dtsi, the
  mtd binding entries. See the crumpet section below.
- `reference/` — evidence anchors from the stock image:
  `extract-stock-dtb.py` + `donut-stock-ns6529.dts` (the retail NS6529
  `boot.img` carries **five** appended DTBs and the retail kernel boots the
  proto board — task-2-report.md).
- `rootfs/` + `ucm2/` — staged into the test image: `extlinux.conf`
  (FDT + dual console), `modprobe.d/donut.conf` (the
  `btmtksdio pre: wlan_mt76x8_sdio` softdep), the five-blob firmware link
  script `usr/lib/firmware/donut-firmware-links.sh`, and the ALSA UCM for
  card `mt8516-donut` (`Speaker` / `Headphones` verbs).
- `tools/` — `donutboot.py` (LK payload packer, boot + swdl variants,
  `test_donutboot.py`), `mk-donut-testimg.sh`, `tools/m0/` (harvest.sh,
  audit.sh, manifest.py, test_stub.sh) plus the lidar helpers.
- `docs/` — `restore.md` (M0 restore runbook) and `m0-checklist.md`
  (M0 execution order + evidence routing).
- `src/` — pristine linux v7.3 / u-boot v2026.10 checkouts plus build
  directories (`src/build/<O>`); untracked, disposable, recreated by
  `build.sh sync`.
- `out/` — build artifacts (untracked; see table below).
- `Containerfile` (with a `Dockerfile` symlink for docker's legacy builder)
  — Debian trixie cross-build container.
- `build.sh` — build pipeline (run `bash build.sh` with no target for the
  full target list; usage text is script lines 3–27).

## From clean checkout to artifacts

Everything builds inside the container; `src/` and `out/` are bind-mounted,
so the container writes there as root.

    docker build -t donut-build .
    b() { docker run --rm -v "$PWD":/work -v "$PWD/src":/src donut-build bash /work/build.sh "$@"; }

    b fetch             # clone linux v7.3 + u-boot v2026.10 (exact tags) into src/
    b linux-config donut   # config gate: must print ZERO "config: not applied" /
                        # "config: still enabled" lines (lidar.config + donut.config)
    b linux donut       # Image.gz + dtbs + modules into src/build/donut/
    b uboot donut       # U-Boot mt8516_amazon_donut_defconfig + pack out/donut-uboot.img
    b wifi donut        # out-of-tree MT76x8 driver (vendor donut.config) -> out/wifi/
    b testimg donut     # out/donut-testimg.ext4 (needs linux + wifi first)

Notes:

- Order matters: `wifi` needs `linux` first (Module.symvers guard), and
  `testimg` needs both (guards in `build.sh` / `tools/mk-donut-testimg.sh`).
- `testimg` fetches the Alpine minirootfs + packages over the network and
  pins `mt7668pr2h.bin` (linux-firmware, redistributable) by sha256
  (`465ed634…ec4cc9` in `tools/mk-donut-testimg.sh`).
- The config gate is the T4/T8 invariant: every option in `donut.config`
  must stick on v7.3 — any `not applied` / `still enabled` line is a
  failure (task-4-report.md).
- Other targets: `sync` (overlay re-sync), `linux-dir` (single-directory
  W=1 builds), `bootimg` (lidar-only kernel-for-LK pack — donut's equivalent
  is `donutboot.py`, see below), `patches` (export overlay diffs to
  `out/patches/`), and `uboot lidar` for the lidar regression build.

## Crumpet (Echo Dot 3rd Gen Refresh) — C0+C1, build-only

The crumpet delta on this same repo: a third overlay layer (`lidar → donut
→ crumpet`) plus `crumpet.config`, adding the mt8167 raw-NAND port
(`mediatek,mt8167-nfc`/`-ecc` variant inside the mainline v7.3
`mtk_nand.c`/`ecc-mtk.c`) and `mt8516-amazon-crumpet.dts` (MX30LF4G28AD
4 GiB NAND, TAS5770 + 2× AIC3101 audio, keypad, dual light sensors).
Status: **C0+C1 complete** (branch `crumpet-c0c1`, commits
`faa4fb8..0df9fdf` + T8) — everything builds and passes its static gates;
**no crumpet hardware has been touched**. Design:
`../../recovery-notes/crumpet-mainline-design.md`; plan + ledger:
`../../recovery-notes/crumpet-mainline-c0c1-plan.md`,
`../../.superpowers/sdd/crumpet-mainline-c0c1-plan/progress.md`.

    b sync                      # applies all three overlay layers
    b linux-config crumpet      # config gate (lidar+donut+crumpet fragments)
    b linux crumpet             # Image.gz + dtbs + modules -> src/build/crumpet/
    python3 tools/crumpet-nand-consistency.py   # caps-vs-register-map lock

Gates (all green at T7/T8): the config gate prints zero `not applied` /
`still enabled` lines; `tools/donut-selftest.sh` proves the 3-layer
shadowing contract; the consistency script pins the mt8167 caps tables to
`reference/crumpet-nfi-register-map.md`; the built DTB contains **zero**
`nand-on-flash-bbt` (first boot uses an in-RAM BBT — no silent writes).
Key artifacts in `src/build/crumpet/` (2026-11-23): `Image.gz` 10,783,596 B
(load ends < 0x43000000), `mt8516-amazon-crumpet.dtb` 18,938 B, vermagic
`7.3.0-dirty` (see `reference/crumpet-build-notes.md`). The three-authority
static register review (C1 sign-off, open items routed to C2/C3):
`reference/crumpet-nand-static-review.md`.

What C2 needs (not in this repo's power): the operator's chip-off session —
the dump that finalizes the gate-A2 spare/ECC model
(`reference/crumpet-nand-spare-ecc-draft.md`, `tools/chipoff` keeps
refusing to bless until then), the flash-slot decision per
`recovery-notes/chipoff-crumpet-spec.md`, and the MAC baked into the DTB at
flash time. Stretch tasks T9 (TAS5770/AIC3101 codec merges) and T10
(crumpet machine driver) did not run — the DTS sound node carries
`amazon,crumpet-sound` awaiting the T10 driver (shape mirrors the stock
`mt8516-soc-card` node).

## Artifacts (current build, 2026-11-21 — `out/`, untracked)

| Artifact | Size | What it is |
|---|---|---|
| `out/donut-linux-boot.img` | 10,795,008 B | Kernel LK payload (Android boot v0 + MTK `0x58881688` header + appended DTBs), boot variant. sha256 `ff77d1b2209090afa4b5bcc20971038e3b645154d84d5de4d027ef9622b49eb3` (pinned in task-5-report.md, re-verified). |
| `out/donut-uboot.img` | 327,680 B | U-Boot in the same LK payload wrapper, boot variant (`build.sh uboot donut` packs via `tools/donutboot.py`). |
| `out/donut-u-boot.bin` / `out/donut-u-boot.dtb` | 655,672 / 8,688 B | Raw U-Boot + its DTB — inputs for the swdl repack below. |
| `out/lidar-uboot.img` | 325,632 B | lidar regression artifact (`build.sh uboot lidar`) — proves the donut work did not move lidar. |
| `out/wifi/wlan_mt76x8_sdio.ko` | 7,735,592 B | MT76x8 WLAN module, vendor `donut.config`; vermagic `7.3.0-dirty SMP preempt mod_unload aarch64`, `depends: cfg80211`. Stripped to 2,274,568 B inside the test image (task-9-report.md). |
| `out/donut-testimg.ext4` | 268,435,456 B | 256 MiB ext4, label `donut-test`: kernel + DTB + Alpine initramfs (UCM, softdep, fw_setenv bootcount, USB ACM/NCM gadget) on userdata. |

Build-level invariants worth remembering (all in the task reports):

- Image header `image_size` 0x1980000 → payload loaded at 0x40080000 ends
  **0x41A00000 < 0x43000000** (BL31 bound) — task-4-report.md.
- Donut DTB **18,931 B < 0x80000** (LK copy bound at 0x4e000000).
- ALSA card `mt8516-donut` with front ends `DL1` (playback, 96 kHz S16/S24),
  `UL1`, `DL1_AWB` (echo reference) + the conditional `HP` jack BE
  (integrated codec @ 48 kHz S24) — `donut-linux/sound/soc/mediatek/mt8516/`
  and task-3/task-8/task-9 reports.

## M2 handoff — deploying to the unit

No flashing has happened and none is authorized from this repo alone; M0
comes first. Everything below is anchored in task-9/task-10 reports and the
in-repo scripts/runbooks.

### Step 1 — M0: audit + harvest (software kit ready: `tools/m0/`)

1. `tools/m0/audit.sh --dry-run` — review; then `--i-mean-it` (or pass
   `--notes-file`). Produces `m0-audit-<ts>/audit-report.md`: unlock-state
   record (LK markers vs the stock reference, slot map, misc evidence, pad
   fields) → decides the deploy path (root-shell `dd` vs kamakiri-donut
   kit first). No mode flag = refusal, zero device calls.
2. `tools/m0/harvest.sh --dry-run` — review; then `--i-mean-it --out
   /evidence/disk/m0-harvest-<date>` (~2 GB raw evidence; keep harvest dirs
   out of the repo). Exit 3 = a device-only blob is missing. Decoy-aware:
   pulls prefer `_real` resolved nodes.
3. Verify: `python3 tools/m0/manifest.py --check <dir>`; keep a second copy
   of `userdata.img` (WHA-oracle state).
4. Route evidence per `docs/m0-checklist.md` §4. This fills the open
   **TODO(M0)** blanks, which are exactly:
   - `partitions.tsv` misc row → `MISC_SIZE` in `tools/mk-donut-testimg.sh`
     (current default is lidar's 512 KiB placeholder; it sizes
     `fw_env.config`);
   - `boot_a`/`boot_b` sizes → `tools/donutboot.py` boot-slot `max_size`
     (16 MiB TODO(M0) placeholder); confirm the swdl 32 MB bound;
   - five MT7668 blobs (`WIFI_RAM_CODE_MT7668.bin`,
     `WIFI_RAM_CODE2_SDIO_MT7668.bin`, `EEPROM_MT7668.bin`, `wifi.cfg`,
     `TxPwrLimit_MT76x8.dat`) → `rootfs/usr/lib/firmware/` name-for-name
     via `donut-firmware-links.sh`;
   - `evidence/i2c-probe.txt` → IS31FL3208 (i2c0 0x6F) population decision
     (design §9; deliberately no DTS node and no driver today);
   - task-8's accdet concern: verify the effective EINT debounce on
     hardware (units question, task-8-report.md §NEXT STEPS).
5. Work `docs/restore.md`'s 8-point rehearse-before-first-flash checklist
   (all read-only) and sign it off **before any flash**.

### Step 2 — pack + flash (per task-9-report.md; swdl is the design §5.2 default)

1. **swdl route (default)** — boots from the 32 MB `swdl` partition while
   `boot_a`/`boot_b` stay stock (strongest reversibility; ramdisk load addr
   0x43478000):
   `python3 tools/donutboot.py pack --variant swdl --kernel src/build/u-boot/u-boot.bin --dtb src/build/u-boot/u-boot.dtb -o out/donut-uboot-swdl.img`
   **boot_a alternative** — only if M0 shows this unit's LK accepts the
   boot-slot payload: use `out/donut-uboot.img` (already packed).
2. Arm the bootcount fallback (from the running system or U-Boot):
   `fw_setenv upgrade_available 1; fw_setenv bootcount 0` — with it armed,
   a kernel that never reaches userspace trips bootlimit (3) and U-Boot
   disarms + stops in USB fastboot.
3. Flash: `fastboot flash userdata out/donut-testimg.ext4` (userdata = GPT
   p16, 1.5 GB) plus the chosen U-Boot image to `swdl` (or `boot_a`).
4. A good boot **self-disarms** (initramfs runs `fw_setenv upgrade_available
   0; fw_setenv bootcount 0`, logged to kmsg). Consoles: `ttyS0` (test
   pads) and `ttyGS0` (USB CDC ACM gadget, 169.254.8.2/16 NCM). Vol+ at
   power-on = fastboot, Vol− = ACM console.
5. Any restore: `docs/restore.md` is the runbook (resolved nodes only,
   `_real` is truth, binary-safe `adb exec-out` readbacks, stock reference
   inventory with sha256).

### Parked from M1 (hardware validation, not software gaps)

- T8 codec/ACCDET port is vendor-faithful but hardware-unvalidated:
  DC-offset calibration (AUXADC ch7/8 units), APLL, CON04 window (+0x710),
  accdet debounce units — M4/M6 checklist in
  `../../.superpowers/sdd/donut-mainline-m1-plan/task-8-report.md`
  §NEXT STEPS.
- Jack use case must re-open DL1 at 48 kHz (speaker runs 96 kHz; the codec
  maxes at 48 kHz) — the UCM owns the re-open + amp-mute semantics.
- Config-wise the tree stays at v7.3 / v2026.10; rebase only after M4
  (design §5.1).
