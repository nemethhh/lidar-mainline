# MT8167/MT8516 NFI + ECC register map & variant data (crumpet C0)

Status: **C0 reference — DRAFT until C2 dump finalization.** Authority for the
mainline `mtk_nand.c`/`ecc-mtk.c` variant (plan Task 5) and the C1 static review
(Task 8). Hardware: MT8516B (crumpet), NAND Macronix MX30LF4G28AD (4 GiB,
4096+256 B/page, 64 pages/block, 2048 blocks, 131 072 pages).

## 0. Sources and authority ranking

| # | Source | Nature | Where |
|---|--------|--------|-------|
| S1 | `tools/kamakiri/emu/nfi_model.py` + `emu/nfi_regs.json` | Register model **validated by executing the real BROM + 2021 preloader bytes** (Unicorn replay; addresses cited as `rom 0x…`/`pl …` in the model) | kamakiri emu |
| S2 | `tools/mtk-nand-utils/` (gilderchuck) | **Real-dump-derived** ECC (BCH t=32, m=14) + scramble (PRBS-15) constants, "tested on a flash dump of an MT8516B only" | cloned 2026-11 |
| S3 | Stock boot-chain disasm targets: BROM (`no-alexa/brom_…bin`), preloader `extracted/pl2021.bin` (functions: `mtk_ecc_config@0x211A7C` etc., from S1's funcs table) | Ground-truth register programming | Task 2 mines these |
| S4 | Mainline v7.3 `drivers/mtd/nand/raw/mtk_nand.c`, `drivers/mtd/nand/ecc-mtk.c` | The vocabulary/variant framework the port rides | donut-mainline src |
| S5 | `probe-out/crumpet-dtb/crumpet-mp-prq.dts:1252-1290` | The vendor node shape our DT must satisfy | crumpet DTB |
| S6 | `tools/vendor-mt8163/` (amazon-oss mt8163 tree) | **RULED OUT** — see ruling below | — |
| S7 | `tools/chipoff/` (49-test suite) + `recovery-notes/chipoff-crumpet-spec.md` | Layout/scramble model + gates A1/A2 | chipoff |

**Ruling (T1, 2026-11-22):** the design's assumption that
`amazon-oss/android_kernel_amazon_mt8163` carries a vendor NFI v2 driver
(**design §6 gap #2**) is **false**: its `drivers/mtd/nand/mtk_nand.c` is a
byte-for-byte ancestor of the *mainline mt2701 driver* (2016, Xiaolei Li /
Jorge Ramirez-Oriz, sole compatible `mediatek,mt2701-nfc` at
`tools/vendor-mt8163/drivers/mtd/nand/mtk_nand.c:1507`); Amazon shipped the
tree without any MT8167 NFI vendor code. Replacement authorities: S1+S2+S3
(real-silicon evidence) with S4 as framework — strictly stronger than the
mt8163-era code the design hoped for. No public mt8516/mt8167 NAND mainline
work exists to collide with (kernel.org MTK matrix: NAND "Wip"; searched
2026-11-22) — our variant remains genuinely new and upstreamable.

## 1. Geometry & on-flash layout (the model everything else serves)

From S2 (real MT8516B dump decode) with S1/S7 corroboration:

- Raw page = **4352 B = 4 chunks × 1088 B**; chunk = **1024 B data + 8 B FDM
  (spare-in-ECC) + 56 B BCH parity**.
  (`tools/mtk-nand-utils/mt8167_correct_ecc.py`: pagesize 4096, oob 256,
  chunks 4, `cooked=1024`, `+8` spare inside the codeword, ecclen = 56.)
- **BCH: t=32, prim_poly=0x4443 (17475, degree 14 → m=14, n=16383),
  `swap_bits=True`** (MTK bit order). t·m = 448 bits = 56 parity bytes ✓ —
  arithmetic closure.
- This *confirms the design's page-geometry prior* (4352 = 4×1088) and refutes
  the community "4532 = 3×1088" transcription slip, independently of our own
  reasoning. Final confirmation stays a C2 dump gate.
- Sector counter semantics: S1 — the boot chain counts **1024-B units**
  (`CON = units<<12 | 0x100`, "counting 1024-B units", preloader load does
  72× CON=0x4100 + 2× 0x2100 for its 4352-B pages) — i.e. **4 sectors/page,
  sector size 1024**, matching S2's 4 chunks.
- Spare classification (chipoff `spare.py` H1 sector map 4×(1024+64)) aligns:
  64 B spare per sector = 8 FDM (ECC-covered) + 56 parity, BBM byte 0
  (pages 0/1 of block) lives in FDM.

## 2. NFI block (0x11001000) — register map

"v7.3" = mainline driver offset (`drivers/mtd/nand/raw/mtk_nand.c` regs);
"S1" = emu/real-boot-chain evidence (`nfi_regs.json`, `nfi_model.py` vaddr
citations). Verdict **SAME** = driver offset usable on mt8167 as-is.

| offset | v7.3 name | S1 evidence | verdict / notes |
|--------|-----------|-------------|-----------------|
| 0x00 | NFI_CNFG | `NFI_CNFG` @0x11001000 | SAME. CNFG_HW_ECC_EN/CNFG_AUTO_FMT_EN path is the mainline ECC flow; the BROM additionally uses CNFG_OP_CUST for raw boot reads |
| 0x04 | NFI_PAGEFMT | `NFI_PAGEFMT` @0x11001004; S1 lists PAGEFMT among regs the boot chain programs | SAME. Sector select: PAGEFMT_SEC_SEL_512 bit2 — **mt8167 uses 1024-B sectors ⇒ bit clear** (S1 unit counting). Spare-per-sector encoding = spare_size table index `PAGEFMT_SPARE_SHIFT 4` (mt2701-style; mt2712 uses 16 — mt8167 is mt2701-generation, see §4) |
| 0x08 | NFI_CON (CON_BRD/CON_BWR, sec count <<12) | `NFI_CON` @0x11001008; S1: `units<<12\|0x100` = exactly v7.3 `CON_SDL(<n>)` sector-count semantics | SAME |
| 0x0C | NFI_ACCCON (timings) | (boot chain uses defaults; not in S1 json) | SAME layout assumed; timing *values* per MX30LF4G28AD datasheet math in driver's `mtk_nfc_compute_timings` — C3 validation |
| 0x10/0x14 | NFI_INTR_EN / NFI_INTR_STA | `NFI_INTR_EN` @0x11001040? → see note | **CHECK**: S1 names 0x40 `NFI_REG_50`… no — S1's json maps `NFI_INTR_EN = 0x11001040` **conflicts** with v7.3 `NFI_DATAW=0x50/NFI_INTR_EN=0x10`. See ruling below |
| 0x20 | NFI_CMD | `NFI_CMD` @0x11001020 | SAME |
| 0x30/0x34/0x38 | NFI_ADDRNOB / NFI_COLADDR / NFI_ROWADDR | S1: `NFI_ADDRNOB=0x11001038`, `NFI_ADDR=0x11001034` | **NAME/ORDER DIFFERS**: mt8167 real name at 0x34 is `NFI_ADDR` (S1), v7.3 calls 0x34 `NFI_COLADDR` + 0x38 `NFI_ROWADDR`; S1 puts ADDRNOB at 0x38. Offsets 0x34/0x38 both exist in both; the driver writes 0x34/0x38 in one flow — treat **0x34 = column+row stream (MTK "ADDR"), 0x38 = ADDRNOB**, and rule the driver's split write wrong-for-mt8167 unless disasm (T2) shows two separate stores. **OPEN→T2** |
| 0x44 | NFI_CNRNB | (ONFI R/B timing; driver-only) | assumed SAME (mt2701-era) |
| 0x50/0x54/0x58 | NFI_DATAW / NFI_DATAR / NFI_PIO_DIRDY | S1: `NFI_REG_50`(FIFO write port)=0x50, `NFI_DATAR`=0x54, `NFI_DATAR_STAT`=0x58 | SAME (names differ only) |
| 0x60 | NFI_STA | S1: `NFI_STA`=0x60; STA busy bit8, EMP_PAGE bit12; S1 adds bit5 = ECC-decode-ready (`rom 0x232c`), bit9 = BROM DMA done (0x200) | SAME; bits 5/9 are BROM-flow bits the mainline driver doesn't use (it gets ECC-done via ECC block IRQ) — no conflict |
| 0x70 | NFI_ADDRCNTR (CNTR sector count bits 16:12) | S1: `NFI_REG_70`=0x70 "completed sectors bits 16:12" (`rom 0x1d96` drain waits on it) | **SAME register, SAME bit field** (S1 name is just pre-disasm label). Driver's `sectors(&addrcntr)` works |
| 0x80 | NFI_STRADDR (DMA start address) | S1: `NFI_STRADDR`=0x11001080, DMA source for AHB reads | SAME |
| 0x84 | NFI_BYTELEN (v7.3) | S1 labels 0x84 `NFI_ADDRCNTR` | **NAME CLASH, OFFSET SAME**: 0x84 exists in both; v7.3 uses it as BYTELEN only on mt2712+ paths — driver use is caps-gated, harmless. Note in code comment |
| 0x90 | NFI_CSEL (v7.3) | S1: `NFI_REG_90`=0x11001090 (programmed in boot flow) | SAME offset; mt8167 meaning = chip-select (CSEL) per v7.3 — treat SAME, verify value written (T2) |
| 0xA0/0xA4 | NFI_FDML(x)/NFI_FDMM(x), FDM size 1..8 | S2: **8 FDM bytes per sector inside the ECC codeword**; FDM_MAX 8 matches mt2701 family | SAME — FDM=8 for crumpet (S2) |
| 0x220 | NFI_DEBUG_CON1 | (not in S1) | mt2712-era reg; driver touches only under `pageformat_spare_shift==16` caps — our caps avoid it |
| 0x224 | NFI_MASTER_STA (v7.3) | S1: `NFI_REG_224`=0x224 hw_reset poll target | SAME offset, meaning compatible (master busy states used in reset poll) |
| 0x238 | — (v7.3: NFI_EMPTY_THRESH @0x23C) | S1: `DMA_CTL`=0x238 bit16 = BROM DMA arm/disarm (`rom 0x10f0/0x10f8`), 0x264..0x278 zeroed when arming | **MT8167-specific DMA arm reg** — v7.3 driver arms DMA via CNFG_AHB+STRADDR+INTR_AHB_DONE and never touches 0x238; BROM flow is its own protocol. No driver change needed; **OPEN→T2** to confirm mainline flow works (C3 live check) |
| 0x240/0x244/0x248 | — | S1 header comment: "the BROM drives the HW ECC decoder through NFI+0x240/0x244/0x248 and the 0x11002000 ECC register block" | **NFI-side ECC steering regs, absent from v7.3 driver** (it drives ECC purely via the ECC block + `mtk_ecc_*` API). Needed only for raw/BROM-style decode. Driver: untouched; **OPEN→T2** (decode their exact function; may matter for C3 raw-mode reads) |

**Ruling (0x10/0x40 clash):** S1's `nfi_regs.json` maps `NFI_INTR_EN=0x11001040`
while v7.3 defines `NFI_DATAW=0x50, NFI_INTR_EN=0x10`. These cannot both hold
for a register set whose 0x50 is the FIFO port (S1 names 0x50 "NFI_REG_50
(FIFO write port)" itself). Most likely the json label was assigned by
pre-disasm pattern naming and 0x40 is really the v7.3 `NFI_CNRNB`-adjacent…
no — v7.3 CNRNB is 0x44. Resolution: **trust offsets over names** (both sources
agree 0x50/0x54/0x58/0x60/0x70/0x80/0x90); the 0x10/0x40 disagreement affects
only INTR_EN/INTR_STA placement. v7.3 mt2701 (same generation, S4) places
INTR_EN@0x10 — adopted; **OPEN→T2** to confirm from `pl2021` disasm (look for
the store of `INTR_AHB_DONE_EN(BIT6)` which v7.3 writes at 0x10).

## 3. ECC block (0x11002000) — register map

v7.3 `ecc-mtk.c` (mt2701/mt2712 column); S3 functions (`mtk_ecc_hw_init`,
`mtk_ecc_config`, `mtk_ecc_wait_done`, …) live in the 2021 preloader — T2
reads them out for byte-level confirmation.

| offset | v7.3 name | notes |
|--------|-----------|-------|
| 0x00 | ECC_ENCCON | encoder enable/idle |
| 0x04 | ECC_ENCCNFG | mode(BCH/NAND)<<ecc_mode_shift, sector size, strength idx |
| 0x08/0x0C | ECC_ENCDIADDR / ECC_ENCIDLE | |
| 0x100/0x104 | ECC_DECCON / ECC_DECCNFG | DEC_EMPTY_EN bit31, DEC_CNFG_CORRECT 0x3<<12, sector/strength |
| 0x10C | ECC_DECIDLE | |
| 0x114 | ECC_DECENUM0 | corrected-bit-count per sector, err_shift right |
| + ecc_regs[] caps | ENCIRQ/DECIRQ/DECDONE/ENCPAR00… | per-variant offset arrays |

## 4. Variant data (the tables Task 5 codes) — MACHINE-PARSEABLE

`tools/donut-mainline/tools/crumpet-nand-consistency.py` (T5) parses the block
below between the markers; keep the format when editing.

<!-- caps-block
nfc.compatible = mediatek,mt8167-nfc
nfc.spare_size = 16 26 27 28 32 36 40 44 48 49 50 51 52 62 63 64
nfc.num_spare_size = 16
nfc.pageformat_spare_shift = 4
nfc.nfi_clk_div = 1
nfc.max_sector = 4
nfc.max_sector_size = 1024
ecc.compatible = mediatek,mt8167-ecc
ecc.err_mask = GENMASK(5, 0)
ecc.err_shift = 8
ecc.ecc_strength = 4 6 8 10 12 14 16 18 20 22 24 28 32 36 40 44 48 52 56 60
ecc.num_ecc_strength = 20
ecc.ecc_mode_shift = 5
ecc.parity_bits = 14
ecc.pg_irq_sel = 0
ecc.ecc_regs = mt2701_ecc_regs
-->

Derivation per field:
- `spare_size` / `pageformat_spare_shift=4`: mt2701-generation PAGEFMT (S4
  `mtk_nand.c` spare tables; S1 PAGEFMT@0x04 mt2701-style, §2); crumpet uses
  index for **64** (4 sectors × 64 = 256 OOB ✓). `nfi_clk_div=1` like mt2701
  (same 1x pad clock generation; **OPEN→T2** from preloader clock setup).
- `max_sector=4, max_sector_size=1024`: S1 (1024-B unit counting) + S2 (4
  chunks) — page 4096/1024.
- ECC caps = **mt2701 set verbatim**: parity_bits 14 (S2's m=14 ✓), strengths
  list incl. t=32 (S2 ✓), err_mask GENMASK(5,0) (t≤60 fits 6 bits), err_shift
  8, mode_shift 5, pg_irq_sel 0, register offsets = mt2701 array. mt2701 ECC
  is the same 2016-generation block (S4); the 2025 mt7986 caps use mt2712
  regs + pg_irq_sel 1 — that is a *newer* generation, not ours.
- **Crumpet operating point: sector 1024, strength 32, spare 64, FDM 8** (S2).
  The 8-chunk/512-B variant in `mt8167_correct_ecc.py` is untested on-dump —
  we do **not** encode it (UNRESOLVED→C2 whether the boot region uses a
  different geometry than userdata — the BROM boot-read 72×CON=0x4100+2×0x2100
  pattern (S1) = 74 1024-B units... see T2; treat as 1024/4 until dump).

## 5. Clock map

Modern node (S5, `crumpet-mp-prq.dts`): `nfc@11001000` clocks
`nfi_clk`,`pad_clk`; `ecc@11002000` clock `nfiecc_clk`. Mainline
`clk-mt8516.c` objects (S4):

| vendor (legacy 18-clock list, dts:1256-1257) | modern | mainline object |
|---|---|---|
| nfi_bclk / nfi_hclk (AHB) | (implicit bus) | infra AHB — not gated separately in mainline tree |
| nfi_1xclk, nfi_1xpad_sel | `pad_clk` | `nfi1x_pad_sel` mux → parent `nfi1x_ck` (FACTOR of `nfi2x_pad_sel`/2) |
| nfi_2xclk, nfi_2xpad_sel | — | `nfi2x_pad_sel` mux (parents: univpll/... see `clk-mt8516.c:143`) |
| nfi_clk (functional) | `nfi_clk` | `CLK_TOP_NFI` gate `nfi` (`clk-mt8516.c:559`, parent `nfi1x_pad_sel`) |
| nfiecc_bclk, nfiecc_sel, nfiecc_csw_sel | `nfiecc_clk` | `CLK_TOP_NFIECC` gate `nfiecc` (`:560`, parent `rg_nfiecc` ← `nfiecc_sel` mux ← `nfi2x_pad_sel`/`csw_nfiecc_sel`) |
| clk_26m, main_dN divisors | — | mux parents, not gates the driver requests |

DTS wiring (T6): `nfc: clocks = <&topckgen CLK_TOP_NFI>, <&topckgen CLK_TOP_NFI1X_PAD_SEL>; clock-names = "nfi_clk","pad_clk"` (driver
`devm_clk_bulk_get` order per names), `ecc: clocks = <&topckgen CLK_TOP_NFIECC>; clock-names="nfiecc_clk"`. Follow the exact pattern of mainline
`mt2701-evb.dts`/`mt7622-bananapi` nodes (check what clock provider phandle
mt8516 exposes — topckgen vs infra; **OPEN→T6** reads `mt8516.dtsi`).

## 6. FIFO/DMA constraints

- v7.3 driver: AHB DMA via CNFG_AHB, burst CNFG_DMA_BURST_EN, STRADDR buffer,
  `INTR_AHB_DONE_EN`; max sectors per transfer bounded by caps/max_sector —
  4×1024 B here. S1 shows the BROM's DMA engine advancing `0x1000` per page
  (4352 B raw) with its own 0x238 arm protocol — a BROM flow, not required for
  the mainline driver. No mt8167-specific FIFO depth evidence available
  pre-dump; use the driver's default thresholds (mt2701-family behavior,
  S4). **OPEN→C2/C3** (live DMA tolerance).

## 7. Open items routed forward

| item | route |
|---|---|
| INTR_EN@0x10 vs json 0x40 label | T2 disasm |
| 0x240/0x244/0x248 ECC steering function; 0x238 DMA_CTL | T2 disasm (document for C3 raw reads) |
| ADDR/COLADDR/ROWADDR split at 0x34/0x38 | T2 disasm |
| nfi_clk_div (1 assumed), ACCCON timing values | T2 disasm (clock init), C3 live |
| ECC caps byte-confirmation (t list, err_shift, irq regs) | T2 disasm of `mtk_ecc_config@0x211A7C` |
| Boot-region vs userdata geometry variance | C2 dump |
| Anything scramble | stays host-side (design §5.1); see gate-A2 draft doc (T2) |
