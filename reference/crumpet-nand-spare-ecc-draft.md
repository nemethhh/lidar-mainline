# Gate A2 draft — crumpet NAND spare/ECC model

**Status: DRAFT — finalized against our own chip-off dump at C2** (design §7
C0 / builder-check #5). This document records both derivations of the ECC
model (spec §4.3), the empirical-fit tooling built for C2, and what remains
dump-gated. The companion register map is
[crumpet-nfi-register-map.md](crumpet-nfi-register-map.md).

## 1. Constants and where each comes from

Three independent sources agree; each row cites them.

| constant | value | preloader disasm (pl2021.i64) | gilderchuck/mtk-nand-utils (real MT8516B dump) | emu (`kamakiri/emu/nfi_model.py`) |
|---|---|---|---|---|
| ECC algorithm | binary BCH, narrow-sense over GF(2^14) | `14 * t` parity-bit math in DECCNFG (`mtk_ecc_config` @0x211A7C) | m=14 (prim_poly 0x4443 = degree-14) | — (parity not modeled) |
| strength table | {4,6,8,10,12,14,16,18,20,22,24,28,32,36,40,44,48,52,56,60,68,72,80} (23 entries) | the full if-ladder of `mtk_ecc_config` maps each t → hw index 0..22 | t=32 used | — |
| parity bits | t × 14 (t=32 → 448 bits = 56 B) | same DECCNFG expression | ecclen 56 | — |
| sector size | 1024 B data | chip-table math in `sub_2137B0`: sectors = page/1024 | chunks of 1024 | CON unit counting "1024-B units" |
| FDM | 8 B/sector, **inside the ECC codeword** | `a1[59] = 8` in `sub_213B14` | chunk = data+8 encoded together | FDM in units |
| spare/sector | 64 (256/4 sectors) | `sub_2137B0`: spare >> log2(sectors) | 4×(64) | H1 sector map |
| page | 4352 = 4×1088 interleaved units | (72×CON=0x4100+2×0x2100 pattern, S1 note) | 4 chunks × 1088 | 1088-B FIFO units |
| bit order | data LSB-first, parity LSB-first (bchlib `swap_bits=True` equiv.) | — | swap_bits=True | — |
| operating point | **t=32, 4×(1024+8+56)** | (strength ladder supports it) | the tested configuration | — |

Disasm spot-checks backing the register map (all in `tools/crumpet2019/pl2021.i64`):
- `mtk_ecc_wait_idle` @0x211920 polls **ECC+0x0C** (encode) / **ECC+0x10C** (decode), bit0 — mainline ENCIDLE/DECIDLE.
- `mtk_ecc_hw_init` @0x2119DC: writes ENCCON=0 (u16 @ECC+0x00), DECCON=0 (@+0x100), clears bit0 of ECC+0x20C.
- `mtk_ecc_config` @0x211A7C: ENCCNFG (@+0x04) = `mode<<5 | strength_idx` (+`sector_sel<<19`); DECCNFG (@+0x104) = `0x80000000 | (bytes_per_chunk)<<16 | sector_cfg<<12 | strength_idx`; DMA addr @+0x08/+0x108; enable u16=1 @+0x00/+0x100; IRQ enable u16=1 @+0x80 (enc) / +0x200 (dec).
- `mtk_ecc_get_stats` @0x211C7C: reads **ECC+0x114** per-sector corrected-bit counts as **byte-packed 7-bit fields** (`>> 8*(s&3) & 0x7F`) → err_shift=8, err_mask=GENMASK(6,0).
- `mtk_ecc_wait_decode_fsm_idle` @0x211D90 polls ECC+0x208 (DECFSM state == idle constant).
- NFI probe `sub_215664`: **NFI+0x0C = 0x10804222** (ACCCON initial timing — settles the T1 0x0C question: it IS ACCCON on mt8167), **NFI+0x44 = 0xF1** (CNRNB), u16 **NFI+0x04 PAGEFMT=3** (raw boot-read config), NFI+0x220 bit15 cleared (DEBUG_CON1), chip-select via 0x90 region, ONFI param-page probe.

## 2. Layout model (H2 — supersedes the H1 contiguous-spare guess)

On-flash page (raw programmer view, columns 0..4351) = 4 interleaved
codewords: unit k at `[k*1088, (k+1)*1088)` = `1024 data + 8 FDM + 56
parity`. The nominal 4096+256 "main/spare" split is just a view — parity of
units 0..2 lives **inside the nominal main plane**. Consequences:

- `tools/chipoff` `spare.py`'s H1 sector map (spare at page bytes
  4096+64k) is **wrong** under H2; the BBM-byte-0 convention needs the C2
  dump to re-anchor (is the factory BBM in unit-0 FDM byte 0 = page byte
  1032, or page byte 0?). Until then `census`/`overlay` keep the existing
  (H1) convention and both are marked C2-reconfirm.
- Host-side decode = gilderchuck's pipeline shape: XOR-descramble
  (mask 1088-periodic, page%64) then per-chunk BCH correct. Dump-side
  correction stays with `mtk-nand-utils`/bchlib at C2.

## 3. Tooling built (all synthetic-proven; C2 finalizes)

`tools/chipoff/ecc.py` (new, 55-test suite green incl. 6 new):
- `bch_parity(payload, params)` — pure-python GF(2^m) narrow-sense BCH
  remainder, MTK bit order; **pinned byte-exact against
  `bchlib.BCH(t=32, prim_poly=17475, swap_bits=True)` over 10 random
  vectors** (bchlib installed in `tools/emu-venv`).
- `BchPageModel` — H2 full-page synthesis (`synthesize_page`);
  `build_unlock_image(..., spare_model=BchPageModel())` uses it, report
  stamped `spare_model: bch-draft, blessed: false`.
- `validate_pages(pairs, params)` / `pick_candidate(pairs)` — the C2
  empirical fit: feed (plain_main, raw_page) pairs from dump + donors; the
  candidate that re-derives factory parity on 100% of content pages wins
  (Gate A2's empirical half; spec §4.3 derivation 1).
- `run.py build --spare-model bch-draft` wired; **refusal invariant**: every
  report carries `blessed: false` + "DRAFT — Gate A2 not closed; do not
  flash real silicon" — flipping it requires a dump-validated model that
  does not exist yet (guarded by test).

## 4. Scramble (Gate A1) status

- `tools/chipoff/scramble.py`: XOR masks 1088-periodic ×4, selected by
  page % 64 — synthetic-proven derive/validate (49-test suite predates
  this task).
- **New analytic prior**: gilderchuck's scrambler generator (PRBS-15
  LFSR, MTK-custom seed[22]=0x484F, 64 hardcoded stream offsets, mask
  repeated across the 4 units) produces the same family of masks. Not yet
  cross-checked against our derived-mask code path — cheap C2-prep
  follow-up: embed the LFSR, assert derived masks == analytic masks
  (ledger: deferred, out of T2 scope).

## 5. Page-geometry prior — CLOSED at draft level

4352 B = 4×1088 confirmed three ways (arithmetic; real-dump decode;
preloader chip-table math). The community "4532 = 3×1088" figure is a
transcription slip (design §5.1). C2 reconfirms on our own dump (cheap).

## 6. What only C2 can decide

1. Gate A2 empirical half: `validate_pages` on real dump pages ≥1000
   across ≥3 partitions (expect fraction 1.0 for t=32/1024 B everywhere;
   any partition that rejects ⇒ boot-region vs userdata geometry variance
   — the preloader's mixed 0x4100/0x2100 CON pattern is the hint to watch).
2. BBM byte position under H2 (unit-0 FDM vs page byte 0).
3. FDM content conventions on factory pages (what the 8 bytes carry).
4. Whether `tee1`/`tee2`/`lk`/`brhgptpl` write-set pages validate under
   the same params as `boot` (Gate A1's per-partition applicability
   question folds into this).
5. Scramble mask table == analytic PRBS-15 table (§4 follow-up).
