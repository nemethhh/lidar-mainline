# Crumpet codec deltas — TAS5770 & TLV320AIC3101 vs mainline v7.3

**Status: C0 scoping (plan Task 3).** Consumed by stretch Task 9 (code) and
Task 6 (DTS codec nodes); C4 validates on hardware. Evidence rule: both sides
cited `file:line`; `V = src-analysis/dot3-6.5.7.0/platform/kernel/mediatek/
mt8167/4.4/sound/soc/codecs`, `M = tools/donut-mainline/src/linux/sound/soc/codecs`.

Board truth (crumpet-mp-prq.dts): 2× `ti,tlv320aic3101` @ i2c0 0x18 (enable-gpio
GPIO2) / 0x1a (none); `ti,tas5770` @ i2c2 0x34 (`ti,reset-gpio` GPIO10,
`ti,irq-gpio` GPIO9, `ti,asi-format=0`, `ti,ppg=0`); sound card
`mediatek,mt8516-soc-card` with `playback-codec-name = "tas2770.2-0034"`.

## 1. TAS5770 → mainline tas2770

**Finding: the 5770 "variant" is a compatible string, nothing more.** The
vendor driver is the direct ancestor of the mainlined tas2770 driver; the
only 5770-specific line in the entire vendor tree is the of_match entry
`{ .compatible = "ti,tas5770" }` (`V/tas2770-regmap.c:574`) — no register
map, DAI, or power branch keys on it. Amazon shipped the TAS5770 under the
tas2770 driver wholesale, and the stock DTB's own `playback-codec-name =
"tas2770.2-0034"` corroborates (design §5.2).

Merge items (all of Task 9's TAS side):

| # | item | vendor | mainline v7.3 | change |
|---|------|--------|---------------|--------|
| T1 | of_match `ti,tas5770` | `V/tas2770-regmap.c:574` | absent (`M/tas2770.c:968` has only `ti,tas2770`) | add entry (same data) |
| T2 | TDM_CFG_REG2 default | `0x3e` (`V/tas2770-regmap.c:48`) | `0x0a` (`M/tas2770.c:787`) | keep mainline default (machine driver sets TDM config anyway — vendor default is board flow residue); note in commit msg |
| T3 | ClockConfiguration volatile | marked volatile (`V/tas2770-regmap.c:72`) | not volatile (`M/tas2770.c` volatile list) | add to volatile list (vendor treats the clock-detect reg as self-updating; harmless for cache correctness) |
| T4 | regmap shape | 8/8 bits, RBTREE cache, max 128, writeable_reg table (`V/tas2770-regmap.c:110-121`) | same shape (`M/tas2770.c:850-859`) | none |
| T5 | 96 kHz support | hw_params SAMPRATE case 96000 (`V/tas2770-codec.c:288-306`) | present in mainline hw_params | none |

DTS node (Task 6/9): mainline binding props — tas2770 uses `#sound-dai-cells`,
`ti,asi-format` is vendor-only → drop (mainline sets format via DAI fmt from
the machine driver); reset GPIO via mainline's own property (`reset-gpios`,
active-low per schematic polarity — verify at C4); `ti,irq-gpio` dropped
(mainline uses IRQ via `interrupts-extended`; wire only if the amp IRQ is
consumed — defer).

Risk list: (a) undocumented 5770 silicon behavior the vendor driver got away
with (IRQ regs, brown-out) — C4 live; (b) S32-silent quirk is machine-driver
territory (donut handles it in the card, §same approach); (c) `ti,ppg` is a
vendor power-limit prop with no mainline equivalent — speaker-protection
tuning may need a sysfs/ucontrol at C4 if output is capped.

## 2. TLV320AIC3101 → mainline tlv320aic31xx

**Finding: same driver lineage — the vendor file IS an aic31xx driver** (it
literally uses `aic31xx_*` function names, `V/tlv320aic3101.c:93-100,1208-…`),
customized with per-instance arrays (`NUM_ADC3101` = 2 instances,
`V/tlv320aic3101.c:63-76`) for crumpet's two mic codecs on one card.

Mainline `tlv320aic31xx.c:1471-1479` already matches: `ti,tlv320aic3100/3110/
3111/3120` + `ti,tlv320dac3100/dac3101` + family wildcards `ti,tlv320aic310x/
311x` — **but not `ti,tlv320aic3101`** (design §5.2 verified).

Merge items (Task 9's AIC side):

| # | item | vendor | mainline v7.3 | change |
|---|------|--------|---------------|--------|
| A1 | compatible `ti,tlv320aic3101` | `V/tlv320aic3101.c:2444` (of_match `aic31xx_of_id`), i2c id `.name "tlv320aic3101"` `:2451` | absent from `M/tlv320aic31xx.c:1471-1479` | add `{ .compatible = "ti,tlv320aic3101" }` (+ i2c id entry) with `driver_data = AIC3100` (`M/tlv320aic31xx.h:28` enum; same mono-DAC/stereo-ADC class as 3100/3120, `M/tlv320aic31xx.c:647`) |
| A2 | TDM slot offsets | per-instance `ch1_offset`/`ch2_offset` byte controls (`V/tlv320aic3101.c:75-76,93-100,244-249`) writing the data-offset reg | `AIC31XX_DATA_OFFSET` written from the BCLK ratio path (`M/tlv320aic31xx.c:1150-1151`), default 0 (`:57`) | verify at T9: mainline derives offset from the DAI format/ratio only — if per-slot manual offsets are needed for the 2-codec TDM, add the two per-DAI controls like vendor (small, upstreamable) |
| A3 | per-instance PGA input selects | `pga*_input_sel_*` arrays (`V:63-66`) | DAPM routes exist (aic31xx has mic PGA inputs) | none expected; C4 checks capture routing |
| A4 | register defaults | vendor defaults table | `M/tlv320aic31xx.c:44-61` | spot-diff at Task 9 implementation; family defaults expected to hold (vendor file shares the PLL/NDAC/MDAC skeleton) |
| A5 | enable-gpio | DTB: codec@0x18 `enable-gpio GPIO2` | aic31xx has GPIO handling (`M/tlv320aic31xx.c` gpiod in probe) | DTS-side; map to mainline prop name at Task 6 |

Fallback (if A2/A4 diverge at implementation time): vendor port of
`tlv320aic3101.c` as a separate codec module (design §5.2 allows; only if
the ID-entry path fails the spot-diff).

Risk list: (a) 3101 register deltas vs 3100 hidden in the 2.4k-line vendor
file — mitigated by A4 spot-diff + C4; (b) both codecs share one card — DAPM
widget name collisions (mainline derives names per-component, fine); (c) the
`NUM_ADC3101` per-instance state must map to per-DAI devdata, not globals —
Task 9 implements via the two DT nodes each probing their own component.
