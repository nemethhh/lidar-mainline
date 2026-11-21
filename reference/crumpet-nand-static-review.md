# Crumpet NAND static review — C1 gate (plan Task 8)

Status: **C1 static review — PASSED** (this document; per-constant verdicts
below). Like every C0/C1 artifact it stays **DRAFT against our own chip-off
dump at C2** — the review proves authority agreement, not silicon truth.
Companion docs: [crumpet-nfi-register-map.md](crumpet-nfi-register-map.md)
(A1), [crumpet-nand-spare-ecc-draft.md](crumpet-nand-spare-ecc-draft.md) (A3).

## 0. Scope, authorities, conflict rule

**Code under review** (the T5/T6 overlay delta — everything else in the two
driver files is verbatim v7.3, verified purely-additive at T5 landing):

- `crumpet-linux/drivers/mtd/nand/raw/mtk_nand.c` — `spare_size_mt8167[]`
  (:188-190), `mtk_nfc_caps_mt8167` (:1512-1519), of_match entry (:1531-1534),
  the 0x84 name-clash comment (:75-79).
- `crumpet-linux/drivers/mtd/nand/ecc-mtk.c` — `ecc_strength_mt8167[]`
  (:95-98), `mtk_ecc_caps_mt8167` (:536-545), of_match entry (:561-562).
- `crumpet-linux/Documentation/devicetree/bindings/mtd/mediatek,mtk-nfc.yaml`
  (:18, :103-112) + `mediatek,nand-ecc-engine.yaml` (:22).
- DTS operating point `crumpet-linux/arch/arm64/boot/dts/mediatek/
  mt8516-amazon-crumpet.dts` (:361-403 NFC node, :376-388 nand@0, :404-410 ECC).

Plus a per-register pass over every NFI/ECC offset the variant's
probe/read/write/ECC paths actually drive (the v7.3 code inherited by the
overlay). Review target == built artifact: the overlay files are byte-equal
to the synced `src/linux/` copies (`diff -q` clean, 2026-11-23) that produced
the T7 `src/build/crumpet` build.

**Three authorities** (task-specified):

- **A1** = [crumpet-nfi-register-map.md](crumpet-nfi-register-map.md), incl.
  §4a shipped-kernel caps (NS6570 vmlinux extraction — MTK's own compiled
  mt8167 variant, **production-proven on this exact silicon**, §4a:133-156).
- **A2** = `tools/kamakiri/emu/nfi_model.py` + `emu/nfi_regs.json` (read-only;
  register model validated by executing the real BROM + 2021 preloader bytes;
  json `_meta.source: pl2021.i64`, nfi_regs.json:8667-8669).
- **A3** = the pl2021 preloader disasm results recorded in
  [crumpet-nand-spare-ecc-draft.md](crumpet-nand-spare-ecc-draft.md)
  (`mtk_ecc_config@0x211A7C` etc., draft §1 :15-31).

**Conflict rule outcome:** every A2-vs-v7.3 disagreement found is a
*name-label* disagreement at an agreed offset, each already adjudicated by a
recorded authority (A1 §2 rulings + §4a shipped-kernel table + A3 disasm) —
see §6. No unresolved authority conflict exists, so **no new IDA passes were
run** (task step 1 conditions them on conflict).

## 1. NFC caps constants — verdicts

Driver: `mtk_nfc_caps_mt8167` @ mtk_nand.c:1512-1519; A1 caps block
@ register-map §4:114-131.

| constant | coded | A1 (register-map) | A2 (emu) | A3 (pl2021 disasm) | verdict |
|---|---|---|---|---|---|
| compatible | `mediatek,mt8167-nfc` (mtk_nand.c:1532) | §4:115; vendor DTB string, §4a:137 (`@file 0x7b6aa0`) | — | — | **MATCH** |
| `spare_size` table | `{16,26,27,28,32,36,40,44,48,49,50,51,52,62,63,64}` (mtk_nand.c:188-190) | §4:116 (identical list); mt2701-generation table (§4 derivation :164-166) | — | spare/sector 64 for MX30LF4G28AD (draft:20, `sub_2137B0`) — entry 64 present | **MATCH** (C2/C3 residual: PAGEFMT index semantics, row below) |
| `num_spare_size` | 16 (mtk_nand.c:1514) | §4:117 | — | — | **MATCH** |
| `pageformat_spare_shift` | 4 (mtk_nand.c:1515) | §4:118; §2:63 mt2701-style, mt2712=16 not ours | PAGEFMT@0x04 programmed by boot chain (nfi_regs.json:4; nfi_model.py:74) | PAGEFMT written u16 (draft:31) | **MATCH** — kernel-side scalar uninterpreted (§6.4), C3 confirms index width live |
| `nfi_clk_div` | 1 (mtk_nand.c:1516) | §4:119 ("like mt2701, same 1x pad clock generation; OPEN→T2") | not modeled (BROM clock init outside NFI window) | no clock-init decode in T2 scope | **MATCH (2-of-3; OPEN→C3)** — consequence: wrong div mis-times pad clock; first C3 read fails loud (timeout), no write risk |
| `max_sector` | 4 (mtk_nand.c:1517) | §4:120 | 1024-B unit counting: CON = `units<<12 \| 0x100` (nfi_model.py:185-186), 4 units/page for 4352-B page (§1 note, model:107-111) | chip-table math 4 sectors × 1024 (draft:18, `sub_2137B0`) | **MATCH** (kernel scalar A=8 uninterpreted, §6.4) |
| `max_sector_size` | 1024 (mtk_nand.c:1518) | §4:121 | 1024-B units (A2 above) | sectors = page/1024 (draft:18) | **MATCH** |

**Spare-index nuance (review finding, no code change):** A1 §4:166 says
"crumpet uses index for **64**". The v7.3 code halves the spare value before
table lookup for 1024-B sectors (mtk_nand.c:373-379 `spare >>= 1` in
`mtk_nfc_hw_runtime_config`; :1219/:1235 in `mtk_nfc_set_spare_per_sector`):
`spare_per_sector` = 64 (256 OOB / 4 steps, = A3 draft:20), PAGEFMT index =
table position of **32** (4<<4). Both 32 and 64 are table members, so probe
cannot catch a wrong encoding; the shipped kernel runs the same
v7.3-lineage halving math at step 1024 on this silicon (§4a + shipped DTB
`nand@0` step 0x400/strength 0x20, crumpet-mp-prq-stock.dts), so the
semantics is production-proven — C2/C3 re-confirms on the dump/live part
(wrong index = wrong spare placement = raw-page compare fails, not silent).

## 2. ECC caps constants — verdicts

Driver: `mtk_ecc_caps_mt8167` @ ecc-mtk.c:536-545; A1 caps block
@ register-map §4:122-130.

| constant | coded | A1 | A2 | A3 | verdict |
|---|---|---|---|---|---|
| compatible | `mediatek,mt8167-ecc` (ecc-mtk.c:561) | §4:122; §4a:137 | — | — | **MATCH** |
| `err_mask` | `ECC_ERRMASK_MT2712` = GENMASK(6,0) (ecc-mtk.c:45,537) | §4:123; §4a:149 `err_mask=0x7f` | — | `& 0x7F` byte-packed count extraction (draft:29, `mtk_ecc_get_stats@0x211C7C`) | **MATCH** (3/3) |
| `err_shift` | 8 (ecc-mtk.c:538) | §4:124 | — | `>> 8*(s&3)` (draft:29) | **MATCH** |
| `ecc_strength` ladder | 23 entries {4…60,68,72,80} (ecc-mtk.c:95-98) | §4:125; §4a:155-156 "preloader ladder stays the authority" | — | full if-ladder of `mtk_ecc_config@0x211A7C` (draft:16) | **MATCH** |
| `num_ecc_strength` | 23 (ecc-mtk.c:541) | §4:126 | — | — | **MATCH** |
| `ecc_mode_shift` | 5 (ecc-mtk.c:542) | §4:127; §4a:149 `mode_shift=5` | — | ENCCNFG = `mode<<5 \| strength_idx` (draft:28) | **MATCH** (3/3) |
| `parity_bits` | 14 (ecc-mtk.c:543) | §4:128; §4a:149 `parity=14` | — | `14 * t` DECCNFG math (draft:15,:17); driver mirror: `strength * caps->parity_bits` @ ecc-mtk.c:217 | **MATCH** (3/3) |
| `pg_irq_sel` | 0 (ecc-mtk.c:544) | §4:129; §4a:149 `pg_irq_sel=0` **vs mt2712's 1**; §4a:158-159 correction note | — | — | **MATCH** — the deliberate mt2712 divergence, kernel-pinned; driver effect = one IRQ per ECC step (ecc-mtk.c:349-354) like mt2701 |
| `ecc_regs` | `mt2712_ecc_regs` {ENCPAR00=0x300, ENCIRQ_EN=0x80, ENCIRQ_STA=0x84, DECDONE=0x124, DECIRQ_EN=0x200, DECIRQ_STA=0x204} (ecc-mtk.c:118-125,540) | §4:130; §4a:152-153 kernel table = mt2701 superset incl. 0x300..0x31C parity bank + 0x80 | — | IRQ enable u16=1 @+0x80 (enc) / +0x200 (dec) (draft:28); mt2712 reg set confirmed (register-map §4 note :157-162, DECIRQ@0x124/0x200) | **MATCH** (0x80/0x200/0x124/0x300 direct; 0x84/0x204 set-level via mt2712 lineage + kernel table) |

## 3. NFI block (0x11001000) — per-register checklist

Every offset the variant's probe/read/write paths touch; bit-field rows
where the driver's encoding must match silicon. A2 citations:
`nfi_model.py` class table (:73-92) / `nfi_regs.json` regs (:3-20).

| reg | driver use (mtk_nand.c) | A1 verdict (§2 row) | A2 emu evidence | A3 disasm | verdict |
|---|---|---|---|---|---|
| 0x00 CNFG (:22) | RMW bits AHB/READ/DMA_BURST/HW_ECC/AUTO_FMT (read_subpage :986-993, write_fdm :804-805), OP_CUST 6<<12 (exec_op :535), BYTE_RW (read/write_byte :458/:422-424) | :62 SAME | `CNFG=0x00` (nfi_model.py:73; nfi_regs.json:3); BROM uses OP_CUST raw reads (A1 :62) | — | **MATCH** |
| 0x04 PAGEFMT (:30) | hw_runtime_config :395 (`fmt = page-sel \| spare_idx<<4 \| fdm<<8 \| fdm_ecc<<12`), hw_init :1110 | :63 SAME, SEC_SEL_512 bit2 clear for 1024-B sectors | `PAGEFMT=0x04` (nfi_model.py:74; nfi_regs.json:4) | u16 PAGEFMT=3 written in raw boot-read config (`sub_215664`, draft:31) | **MATCH** (index semantics → C2/C3, §1 nuance) |
| 0x08 CON (:39) + bits | hw_reset :279/:290 (FIFO_FLUSH\|NFI_RST), read: `sectors<<12 \| CON_BRD(bit8)` :1016-1018, write: `sectors<<12` then `\| CON_BWR(bit9)` :807/:813 | :64 SAME (`units<<12\|0x100` = CON_SDL semantics) | **bit-exact**: BROM drain reprograms CON `units<<12 \| 0x100` = `CON_SDL(n) \| CON_BRD` (nfi_model.py:185-186); "NFI_CON bits 25:12 stay the SECTOR count in both protocols" (:228-236) | 72× CON=0x4100 + 2× 0x2100 for 4352-B pages (draft:21; A1 §1) | **MATCH** — offset, sec shift 12, BRD bit8 all executed-by-BROM |
| 0x0C ACCCON (:46) | setup_interface :650 `ACCTIMING(tpoecs<<28\|tprecs<<22\|tc2r<<16\|tw2r<<12\|twh<<8\|twst<<4\|trlt)` (bit doc :633-643) | :65 SAME layout assumed; timing values C3 | not in A2 json (boot chain keeps defaults) | **sub_215664 writes NFI+0x0C = 0x10804222** (draft:31) — decodes under the driver's own packing to {tpoecs=1, tprecs=2, tc2r=0, tw2r=4, twh=2, twst=2, trlt=2}: sane fields ⇒ packing plausible | **MATCH (offset)**; computed *values* **OPEN→C3** |
| 0x10 INTR_EN (:47, INTR_AHB_DONE_EN bit6 :48) | read :1012, write :809, clear :820, hw_init disable :1115 | :66 SAME (emu 0x40 label retired); §2 ruling :82-91 | json labels 0x10 `NFI_DATA_CON` (nfi_regs.json:6) — old-generation name, offset agrees | preloader polls only, no INTR store found (A1 :66, §7 table) | **MATCH** (production-kernel authority §4a:143-146; residual C3 — misuse fails as DMA-done timeout, loud) |
| 0x14 INTR_STA (:49) | hw_init clear :1114, irq ack :1123-1129 | :66 SAME | — (not in json) | — | **MATCH** (same §4a authority; same fail-loud shape) |
| 0x20 CMD (:50) | send_command :300 | :67 SAME | `CMD=0x20` (nfi_model.py:77; nfi_regs.json:7); full opcode set executed (model :95-112) | opcode sites (nfi_regs.json opcode_sites, `_meta`) | **MATCH** |
| 0x30 ADDRNOB (:51) | send_address :320 (writew 1) | :68 **SAME (mainline)** — resolved by shipped-kernel reg table | json old-layout name `NFI_CSN_CON@0x30` (nfi_regs.json:8) — name retired by A1 ruling "trust offsets over names" (:82-91) | — | **MATCH** (production kernel runs mainline layout on this silicon, §4a:143-146; C3 residual, fail-loud addressing errors) |
| 0x34 COLADDR (:52) | send_address :318 (32-bit column) | :68 SAME (mainline) | json `NFI_ADDR@0x34` (nfi_regs.json:9); BROM writes COLUMN to 0x34 (nfi_model.py:228-236) — same function, old name | — | **MATCH** — column port function pinned by executed BROM bytes |
| 0x38 ROWADDR (:53) | send_address :319 (32-bit row) | :68 SAME (mainline) | json `NFI_ADDRNOB@0x38` (nfi_regs.json:10) — old name; BROM writes ROW value to 0x38 (rom 0x19a4, nfi_model.py:228-236) | — | **MATCH** — row port function pinned by executed BROM bytes |
| 0x40 STRDATA (:54, STAR_EN=1 :55) | read/write_byte :434/:464, write :815, read :1018 | (not an A1 §2 row — reviewed here) | json `NFI_INTR_EN@0x40` (nfi_regs.json:11) — old-layout name; the driver's STAR_EN write is the start-transfer trigger in the mt2701 lineage (S4) | — | **MATCH** (production-kernel authority: the shipped mt8167 variant descends from the same mt2701-lineage driver and drives STRDATA on every page op, §4a; C3 residual, fail-loud hang if wrong) |
| 0x44 CNRNB (:57) | hw_init :1109 writes **0xf1** | :69 "assumed SAME (mt2701-era)" | — | **sub_215664 writes NFI+0x44 = 0xF1** (draft:31) | **MATCH — value-level**: stock boot chain and mainline hw_init program the identical constant at the identical offset |
| 0x50 DATAW (:58) | write_byte :468 (PIO feed) | :70 SAME | `DATAW=0x50` FIFO write port (nfi_model.py:82; nfi_regs.json:12); model implements byte/word feed (:296-315) | — | **MATCH** |
| 0x54 DATAR (:59) | read_byte :439 (PIO drain) | :70 SAME | `DATAR=0x54` (nfi_model.py:83; nfi_regs.json:13); word/byte drain implemented (:138-152) | — | **MATCH** |
| 0x58 PIO_DIRDY (:60, bit0 :61) | wait_ioready :408 poll bit0 | :70 SAME | `DATAR_STAT=0x58` bit0 data/FIFO ready (nfi_model.py:84; nfi_regs.json:14; read() :154-160) | — | **MATCH** |
| 0x60 STA (:62) + bits | send_command :302 (CMD/ADDR busy clear), send_address :322, exec_instr :503 (STA_BUSY bit8), FSM mask :420/:455, EMP_PAGE bit12 (:65) | :71 SAME; BROM-flow bits 5/9 not used by driver — no conflict | `STA=0x60` (nfi_model.py:85; nfi_regs.json:15); model returns bit5/9 semantics + "never busy" for bits 0/1/8 (read() :161-166) | bit5 = ECC-decode ready (`rom 0x232c`) | **MATCH** |
| 0x70 ADDRCNTR (:69) + CNTR bits 16:12 (:73-76) | do_write_page :825 poll `ADDRCNTR_SEC >= sectors` (write phase) | :72 SAME register, SAME bit field | `REG_70=0x70` "write-phase completed sectors, bits 16:12" (nfi_model.py:86; model :174-175) | BROM drain waits on it (`rom 0x1d96`, A1 :72) | **MATCH** — phase (write) and bit field agree |
| 0x80 STRADDR (:74) | write :808, read :1013 (DMA buffer) | :73 SAME | `STRADDR=0x80` DMA source (nfi_model.py:87; nfi_regs.json:17); model lands DMA there (:186-200) | — | **MATCH** |
| 0x84 BYTELEN (:80, comment :75-79) | read_subpage :1022-1024 poll `ADDRCNTR_SEC(reg) >= sectors` (read phase) — no caps gate | :74 NAME CLASH, OFFSET SAME | json `NFI_ADDRCNTR@0x84` (nfi_regs.json:18) — "completed sectors (read phase)" bits 16:12 (nfi_model.py:88; model :163-168) | — | **MATCH** — both namings describe the register the read-phase completion poll needs, same offset, same bit field; overlay comment (:75-79) accurate |
| 0x90 CSEL (:81) | select_target :520 (`sels[cs]`) | :75 SAME offset; verify value (T2) | `REG_90=0x90` "programmed in boot flow" (nfi_model.py:89; nfi_regs.json:19) | "chip-select via 0x90 region" (draft:31, sub_215664) | **MATCH** |
| 0xA0/0xA4 FDML(x)/FDMM(x) (:82-83), FDM_MAX 8 (:84) | read_fdm :754-755, write_fdm :783-784; set_fdm clamps to 8 | :76 SAME — FDM=8 for crumpet (S2) | FDM carried in FIFO units (nfi_model.py interleave :430-447) | `a1[59] = 8` (draft:19, `sub_213B14`) — **FDM 8 pinned by preloader** | **MATCH** |
| 0x220 DEBUG_CON1 (:86) + strobe bits 4:3 (:87-88) | setup_interface :631-634 RMW strobe bits **unconditionally** | :77 — row **corrected this task** (see §6.3): register exists on mt8167; earlier "caps-gated" claim was wrong | not in A2 | **sub_215664 clears bit15 of NFI+0x220** (draft:31) | **MATCH (register exists)**; strobe *fields* **OPEN→C3** — RMW preserves other bits; only tREA-sampling delay, bounded by MAX_STROBE_DLY 3 |
| 0x224 MASTER_STA (:90, mask 0xFFF :91) | hw_reset :282/:287 poll | :78 SAME offset, meaning compatible | `REG_224=0x224` hw_reset poll target (nfi_model.py:90; nfi_regs.json:20); model returns idle (0) | — | **MATCH** |
| 0x23C EMPTY_THRESH (:92) | **defined, never used** in v7.3 mtk_nand.c — zero traffic on mt8167 | :79 row flags 0x238 (BROM DMA_CTL) as BROM-only; 0x23C is the v7.3-era neighbor | `DMA_CTL=0x238` BROM arm reg (nfi_model.py:91; not driver traffic) | — | **N/A (no driver traffic; by design untouched)** |
| 0x238 + 0x240/0x244/0x248 (BROM DMA arm + ECC steering) | driver **never touches** (arms DMA via CNFG_AHB+STRADDR+INTR_AHB_DONE; ECC via ECC block) | :79/:80 — BROM protocol, no driver change needed | model implements the BROM protocol (nfi_model.py:91-92,:185-200) | BROM drives HW ECC decoder through 0x240/0x244/0x248 (nfi_model.py docstring :46-48) | **N/A (no driver traffic; C3 raw-read watch item per A1 §7)** |

## 4. ECC block (0x11002000) — per-register checklist

Driver defines @ ecc-mtk.c:26-45; use sites as cited.

| reg | driver use (ecc-mtk.c) | A1 (§3 row / §4a) | A3 disasm (pl2021) | verdict |
|---|---|---|---|---|
| 0x00 ENCCON (:26) | hw_init writew disable :270; config enable via ECC_CTL_REG :40 | §3:101 | `mtk_ecc_hw_init@0x2119DC` writes ENCCON=0 u16 @+0x00; enable u16=1 @+0x00 (draft:27-28) | **MATCH** |
| 0x04 ENCCNFG (:27) | config :208-209 `ecc_bit \| mode<<5 \| enc_sz<<16` | §3:102 | ENCCNFG = `mode<<5 \| strength_idx` (+`sector_sel<<19`) @0x211A7C (draft:28) — mode_shift 5 and index encoding identical | **MATCH** |
| 0x08 ENCDIADDR (:29) | config :212-213 (non-NFI mode DMA addr) | §3:103 | "DMA addr @+0x08" (draft:28) | **MATCH** (+0x108 decode-side write is preloader-only; v7.3 decode data flows through NFI DMA — no driver counterpart needed, C3 note) |
| 0x0C ENCIDLE (:30) | wait_idle via ECC_IDLE_REG :40, bit0 ECC_IDLE_MASK :20 | §3:103 | `mtk_ecc_wait_idle@0x211920` polls ECC+0x0C bit0 (draft:26) | **MATCH — bit-level** |
| 0x100 DECCON (:31) | hw_init writel disable :273 | §3:104 | DECCON=0 @+0x100 (draft:27) | **MATCH** |
| 0x104 DECCNFG (:32) | config :216-222 `ecc_bit \| mode<<5 \| dec_sz<<16 \| DEC_CNFG_CORRECT(0x3<<12) \| DEC_EMPTY_EN(BIT31)`; `dec_sz = len<<3 + strength*14` (:217) | §3:104 | DECCNFG = `0x80000000 \| bytes_per_chunk<<16 \| sector_cfg<<12 \| strength_idx` @0x211A7C (draft:28); `14*t` parity math = coded `strength * parity_bits` | **MATCH — field-level** |
| 0x10C DECIDLE (:35) | wait_idle :40 | §3:105 | wait_idle polls ECC+0x10C bit0 (draft:26) | **MATCH — bit-level** |
| 0x114 DECENUM0 (:36) | get_stats :243 readl + err_shift/err_mask decode | §3:106 | `mtk_ecc_get_stats@0x211C7C`: byte-packed 7-bit per-sector counts `>> 8*(s&3) & 0x7F` (draft:29) | **MATCH — encoding-level** |
| ecc_regs[ENCPAR00]=0x300 (:119) | encode parity readout :446 | §4a:153 (kernel table 0x300..0x31C parity bank) | mt2712 set (A1 :157-162) | **MATCH** |
| ecc_regs[ENCIRQ_EN]=0x80 (:120) | enable :357 writew ECC_IRQ_EN(bit0) (+pg_irq_sel bit1 logic :349-354 — off for mt8167) | §4a:153 (0x80 in kernel table) | IRQ enable u16=1 @+0x80 enc (draft:28) | **MATCH** |
| ecc_regs[ENCIRQ_STA]=0x84 (:121) | irq handler :174 | mt2712 set (A1 :157-162) | set-level (mt2712 layout; preloader polls) | **MATCH (set-level)** |
| ecc_regs[DECDONE]=0x124 (:122) | irq handler :161 | §4a:153; mt2712 set | DECIRQ@0x124 (A1 :157-162) | **MATCH** |
| ecc_regs[DECIRQ_EN]=0x200 (:123) | enable :360 | §4a:153 | IRQ enable u16=1 @+0x200 dec (draft:28) | **MATCH** |
| ecc_regs[DECIRQ_STA]=0x204 (:124) | irq handler :158 | mt2712 set | set-level | **MATCH (set-level)** |
| 0x208 DECFSM / 0x20C (preloader-only) | **no driver traffic** (v7.3 waits on IRQ completion, not FSM) | — | preloader polls 0x208 idle, clears 0x20C bit0 (draft:27,:30) | **N/A (no driver traffic; C3 watch: if the decode IRQ never fires, the preloader's 0x20C clear may be a required unlock — live item)** |

## 5. Bindings + DTS operating point

| item | coded | authority | verdict |
|---|---|---|---|
| mtk-nfc.yaml enum + if-block (:18, :103-112): step enum `[512,1024]`, strength enum = 23-entry ladder (:111-112) | matches caps | A1 §4 tables (:118-126); T5 ruling [512,1024] mt2701-family pattern | **MATCH** (512 untested on-dump — narrow at C2 per T5 ruling, ledger) |
| nand-ecc-engine.yaml enum entry (:22) | `mediatek,mt8167-ecc` | A1 §4:122 | **MATCH** |
| DTS `nand@0` operating point: step 1024, strength 32, mode "hw" (dts:376-388) | crumpet point | A3/S2 operating point t=32, 4×(1024+8+56) (draft:23); A1 §4:184 | **MATCH** |
| DTS: **zero** `nand-on-flash-bbt` (dts:390-401 comment; gate re-run T6 — source grep 0, decompiled DTB 0, strings 0) | first-boot in-RAM BBT | design §5.1 builder-check #4; plan global constraint | **PASS (gate)** |
| DTS clocks: nfc `<&topckgen CLK_TOP_NFI>, <&topckgen CLK_TOP_NFI1X_PAD_SEL>` names `nfi_clk,pad_clk` (dts:365-366); ecc `CLK_TOP_NFIECC` name `nfiecc_clk` (dts:408-409); `ecc-engine = <&bch>` (dts:367) | wiring | A1 §5 clock map (register-map:190-208): CLK_TOP_NFI / CLK_TOP_NFI1X_PAD_SEL / CLK_TOP_NFIECC rows :201-202, DTS wiring :205-206; T6 gate re-verified IDs in mt8516-clk.h :83/:84/:170 | **MATCH** |

## 6. Authority conflicts encountered + resolutions (why no new IDA passes)

1. **0x10/0x14 vs 0x40 (INTR_EN placement).** A2 json names 0x10
   `NFI_DATA_CON` and 0x40 `NFI_INTR_EN` (old-generation labels,
   nfi_regs.json:6,:11); v7.3 uses INTR_EN@0x10/INTR_STA@0x14/STRDATA@0x40.
   Resolved *before* this review by A1 §2 ruling "trust offsets over names"
   (register-map:82-91) + **§4a**: the shipped production kernel's mt8167
   register-offset table contains 0x10/0x14 in the mainline slots
   (register-map:143-146). A3 found no INTR store in the preloader
   (polling-only). Production silicon evidence > label archaeology; no
   conflict remains.
2. **0x30/0x34/0x38 names.** A2 json old names CSN_CON/ADDR/ADDRNOB
   (nfi_regs.json:8-10); the *function* at 0x34/0x38 is pinned by executed
   BROM bytes (column@0x34, row@0x38 — nfi_model.py:228-236) = mainline
   COLADDR/ROWADDR; 0x30's function rests on the §4a production-kernel
   table. Resolved; C3 residual (fail-loud).
3. **NFI_DEBUG_CON1@0x220.** A1 §2:77 claimed the driver touches it "only
   under `pageformat_spare_shift==16` caps — our caps avoid it"; the v7.3
   code RMWs it **unconditionally** in `mtk_nfc_setup_interface`
   (mtk_nand.c:631-634; T5 ledger already flagged this). **Register-map row
   corrected in this task** — the register does exist on mt8167 (A3:
   `sub_215664` clears bit15, draft:31) and the write is a strobe-bits-only
   RMW. No code change required.
4. **Shipped-kernel NFC caps scalars {8, 14, 16, 16} (+ ECC X=19)**
   (§4a:145-146,:156). Uninterpreted (vendor struct ≠ mainline's) — this is
   *silence*, not conflict: the scalar set cannot be rearranged into our
   {16, 4, 1, 4, 1024} caps, so the kernel entry neither confirms nor
   contradicts them. Decoding the vendor struct layout in a new IDA pass
   would still not bind a scalar to a mainline field without a live check,
   so the task's conflict rule does not trigger. NFC caps therefore rest on
   A1§4 + A2 (max_sector/max_sector_size) + A3/S2 (spare 64, 4×1024) —
   routed C3 (§7).

## 7. Open items routed forward (the C2/C3 list this review produces)

| item | route | consequence if wrong |
|---|---|---|
| PAGEFMT spare-index encoding (halving semantics, §1 nuance) | C2 dump + C3 raw reads | wrong spare placement → raw-page compare fails loudly; no write risk (writes gated at C3+) |
| INTR_EN@0x10 / STRDATA@0x40 / ADDRNOB@0x30 functional truth | C3 live | all three fail loud (DMA-done timeout / PIO hang / misaddress), zero silent-corruption paths; driver issues no writes during C3 |
| ACCCON computed timing values (offset+packing matched; values datasheet-derived) | C3 live | slow/timing-marginal reads; preloader's own 0x10804222 is the fallback constant |
| nfi_clk_div = 1 | C3 live | pad-clock mis-rate → attach/read failure, loud |
| DEBUG_CON1 strobe fields (bits 4:3) on mt8167 | C3 live | tREA sampling delay only; RMW-bounded |
| 0x208/0x20C decode-IRQ unlock (preloader clears 0x20C bit0) | C3 live | decode-complete IRQ never fires → read timeout, loud |
| Gate A2 empirical half (spare/ECC layout on real dump, BBM position, boot-vs-userdata geometry) | **C2 dump** (draft §6:86-97) | chipoff `build` keeps refusing to bless (`blessed:false` invariant, T2 tests) |
| binding step-size 512 (untested on-dump) | C2 | narrow enum to [1024] |
| kernel caps scalars X=19/A=8 + NFC sub {8,14,16,16} | C3 curiosity only | none — informational |

## 8. Verdict — C1 gate

- **Caps constants**: 16/16 MATCH (2 with explicit OPEN residuals routed
  C2/C3; 0 DIVERGE; 0 FAIL — no T5 fix pass required).
- **NFI register offsets**: 24 driver-driven rows — 22 MATCH, 2 N/A
  (defined-unused / BROM-only), 0 DIVERGE.
- **ECC register offsets**: 14 rows — 12 MATCH (2 bit/encoding-level exact
  vs disasm), 2 N/A/notes, 0 DIVERGE.
- **Bindings + DTS**: 5 MATCH/PASS incl. the zero-`nand-on-flash-bbt` gate.
- Machine gate re-run at review time: `tools/crumpet-nand-consistency.py`
  → `all caps checks passed` (exit 0); overlay files byte-equal to the
  synced tree that produced the T7 build (Image.gz 10,783,596 B; DTB
  18,938 B, `src/build/crumpet/`).
- Every remaining unknown fails **loud** (timeout/garbage caught by the C3
  per-page raw compare) and none can silently issue a write during the
  read-only gate — the C1 safety argument.

**Static register-map review: PASS. C1 done-when satisfied.** What C2/C3
must still decide is enumerated in §7; nothing in it blocks the C2 chip-off
session or reopens C0/C1 scope.
