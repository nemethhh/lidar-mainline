# Crumpet kernel build notes (C1 build gate)

Status: **C1 evidence record** for plan Task 7 (`recovery-notes/crumpet-mainline-c0c1-plan.md`).
All numbers below are read from the real build artifacts of the full green build
(2026-11-22); every claim cites the artifact it came from. Re-derive with the
commands in §4 after touching `crumpet.config` or the overlay trees.

## 1. Build inputs

- Tree: pristine linux **v7.3** (`src/linux`) + 3-layer overlay sync
  lidar-linux → donut-linux → crumpet-linux (`build.sh sync`; stacking contract
  asserted by `tools/donut-selftest.sh`).
- Config: arm64 defconfig + `lidar.config` + `donut.config` + `crumpet.config`
  stacked by `build.sh linux-config crumpet` (`build.sh:84-88` — crumpet.config
  rides on top, mirroring the overlay sync; explicit `is not set` overrides drop
  baseline lines, e.g. `SND_SOC_MT8516_AMAZON_DONUT`).
- Commands (container `donut-build`, 32 jobs):
  `build.sh sync` → `build.sh linux-config crumpet` → `build.sh linux crumpet`
  (default make args `Image.gz dtbs modules`, `build.sh:175`).

## 2. Gate results

- All-sticks gate: `build.sh linux-config crumpet` exits 0 printing **zero**
  `config: not applied` / `config: still enabled` lines (checker: `build.sh:112-133`).
- Selftest: `tools/donut-selftest.sh` — PASS (crumpet shadows donut shadows
  lidar; restore paths + lidar-overlay-still-applied all green).
- Full build: `make Image.gz dtbs modules` exit 0, zero errors.
- Key config outcomes (`src/build/crumpet/.config`):
  `MTD=y`, `MTD_NAND_ECC_MEDIATEK=y`, `MTD_NAND_MTK=y` (NAND pair **builtin** —
  `modules.builtin` lists `kernel/drivers/mtd/nand/ecc-mtk.ko` and
  `kernel/drivers/mtd/nand/raw/mtk_nand.ko`; C3 boot gate wants root storage
  builtin-capable), `OPT3001=m`, `TSL2772=m`,
  `# CONFIG_SND_SOC_MT8516_AMAZON_DONUT is not set`, `SND_SOC_TAS2770=m` +
  `SND_SOC_TLV320AIC31XX=m` (baseline-kept, bind nothing until T9), `SND_SOC_MT8167_CODEC=m`,
  `EXTCON_MT8167_ACCDET=m`, `# CONFIG_REGULATOR_RT5739 is not set`,
  `# CONFIG_TSL2540 is not set`.

## 3. Artifact record (C1 numbers)

| Item | Value | Source artifact |
|------|-------|-----------------|
| kernel.release | `7.3.0-dirty` | `src/build/crumpet/include/config/kernel.release` |
| vermagic | `7.3.0-dirty SMP preempt mod_unload aarch64` | `.ko` modinfo, e.g. `sound/soc/mediatek/mt8516/mt8516-amazon-lidar.ko` |
| Image.gz | 10 783 596 B | `src/build/crumpet/arch/arm64/boot/Image.gz` |
| Image (uncompressed) | 26 008 064 B | `src/build/crumpet/arch/arm64/boot/Image` |
| arm64 header `image_size` | `0x1980000` (26 738 688 B) | Image bytes @0x10 (LE64); `text_offset` = `0x0` @0x08; MZ magic @0x00 |
| Load / end | load `0x40080000` + `0x1980000` = end **`0x41A00000`** | load addr = `KERNEL_ADDR` family constant (`tools/donutboot.py:52`, `tools/lidarboot.py:28`) |
| End < 0x43000000 | **PASS** (headroom 0x1600000 = 22 MiB) | computed |
| System.map `_text`/`_end` | `ffff800080000000` / `ffff800081980000` (span 0x1980000 == header `image_size`) | `src/build/crumpet/System.map` |
| DTB | `mt8516-amazon-crumpet.dtb` 18 938 B < 0x80000 (524 288) **PASS** | `src/build/crumpet/arch/arm64/boot/dts/mediatek/` |
| vmlinux | 120 569 824 B | `src/build/crumpet/vmlinux` |
| Module count | **805** `.ko` files | `find src/build/crumpet -name '*.ko' \| wc -l` |

- vermagic `-dirty` suffix: family norm — donut/lidar builds report the same
  (`src/build/donut/` + `src/build/lidar/` `kernel.release` both `7.3.0-dirty`);
  the overlay rsync leaves `src/linux` permanently dirty relative to its pristine
  git state. C4 WiFi module builds must use this vermagic/Module.symvers pair.
- NAND/ECC linked into vmlinux: 80 `mtk_nfc`/`mtk_ecc` symbols in `System.map`.
- Notable modules built: `sound/soc/mediatek/mt8516/{snd-soc-mt8516-afe.ko,
  mt8516-amazon-lidar.ko}`, `sound/soc/codecs/snd-soc-mt8167-codec.ko`,
  `drivers/extcon/extcon-mt8167-accdet.ko`, `drivers/iio/light/{opt3001.ko,
  tsl2772.ko}`, `drivers/net/bluetooth+wireless` set (BT_MTKSDIO etc.).
  No donut/crumpet machine module (`AMAZON_DONUT` off; `AMAZON_CRUMPET` = T10).

## 4. Re-derive commands (host, artifacts are in src/build/crumpet)

```sh
cat src/build/crumpet/include/config/kernel.release
strings src/build/crumpet/sound/soc/mediatek/mt8516/mt8516-amazon-lidar.ko | grep -m1 vermagic
stat -c '%s %n' src/build/crumpet/arch/arm64/boot/{Image.gz,Image} \
  src/build/crumpet/arch/arm64/boot/dts/mediatek/mt8516-amazon-crumpet.dtb
python3 -c "import struct;d=open('src/build/crumpet/arch/arm64/boot/Image','rb').read(32);\
s=struct.unpack_from('<Q',d,16)[0];print(hex(s),hex(0x40080000+s),0x40080000+s<0x43000000)"
find src/build/crumpet -name '*.ko' | wc -l
```
