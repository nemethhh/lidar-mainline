# Crumpet audio forensics — stock HAL configs + machine-driver RE

**Status: C0 (plan Task 4) — complete.** Sources: shipped NS6570
kernel `extracted/kernel/vmlinux.raw` (arm32 4.4.22+, 11.78 MB; link
**VA = 0xC0008000 + file offset**), HAL configs from
`extracted/system/rootfs/system/vendor/etc/` (2025) +
`extracted-6548/rootfs/system/vendor/etc/` (2021), and the vendor GPL
drop machine driver `D = src-analysis/dot3-6.5.7.0/platform/kernel/
mediatek/mt8167/4.4/sound/soc/mediatek/mt8167/donut.c` (plus the drop's
4.4 `include/sound/{soc.h,soc-dapm.h,control.h,soc-dai.h}` for struct
layouts — binary decodes match source field-for-field, see §3).

## 1. HAL configs (both vintages byte-identical)

`mixer_paths.xml` (119 lines) — the stock HAL's routing truth:
- MTK AFE crossbar switches (names = mainline mtk-afe port ids):
  `O03 I05`/`O04 I06` (on), `O05 I05`/`O06 I06` (on), `O09 I03`/`O10 I04`
  (on), `O11 I02` (off), BT paths `O00 I07`/`O01 I08`, capture `O13 I00`/
  `O14 I01`/`O11 I10`.
- `INT ADDA O03_O04 Switch`, `I2S O03_O04 Switch`, `MRG BT O02`,
  `PCM0 O02` — integrated-codec + I2S + merged-BT routing.
- Speaker path: `LINEOUT Mux = VOICE_AMP` + `I2S O03_O04 = 1` +
  `PCM Playback Volume`; headphone path: `HPOUT Mux = AUDIO_AMP` —
  the TAS5770 hangs off the I2S path; VOICE_AMP = int-codec speaker route.
- Mic codecs: `ADC_A`/`ADC_B` (the two AIC3101) — `ADC_x DIF1_L/R` input
  selects + gains, `ADC_x MICPGA Volume = 24`.
`audio_policy.conf` is generic boilerplate (goldfish header) — not board
truth. MTK parameter files live in `audio_device.xml`/`audio_param/` (C4
territory).

## 2. Machine driver RE (shipped kernel, statically recovered)

The family machine driver (crumpet+pascal+croissant) registers cards:
`mt8516-soc-card` / `mt8516-soc-pascal-card` / `mt8516-soc-croissant-card`
(compatibles at file 0x7f4990/0x7f50d9/0x7f58e1 in inline-compatible
0xC4-byte entry tables — no of_device_id pointers; driver strcmp-matches).
Crumpet's card is `donut_card` (`.name = "mt-snd-card"`, `D/donut.c:419-431`).

Crumpet card struct @ VA **0xc0b28ccc** (= .name field; card name string
@0xc09a9ca2, long name `mt-snd-card` @+0x40=0xc0b28d0c):
- controls/widgets/routes (ptr,count) pairs @ **+0xE4/+0xEC/+0xF4** =
  **0xc07fcad8 ×1**, **0xc07fcbb8 ×5**, **0xc07fcb08 ×0xb**
  (words @0xc0b28db0..c4; matches `.controls/.dapm_widgets/.dapm_routes`
  in `D/donut.c:424-429`).
- **dai_link table: 17 links @ 0xc0b28e74**, entries are 0x60-byte 4.4
  `snd_soc_dai_link` (GPL-drop `include/sound/soc.h:952-1040`):
  {name@+0x00, stream_name@+0x04, cpu_name@+0x08, cpu_of_node@+0x0C,
  cpu_dai_name@+0x10, codec_name@+0x14, codec_of_node@+0x18,
  codec_dai_name@+0x1C, codecs@+0x20, num_codecs@+0x24,
  **platform_name@+0x28**, platform_of_node@+0x2C, be_id@+0x30,
  params@+0x34, num_params@+0x38, dai_fmt@+0x3C, trigger[2]@+0x40,
  init@+0x48, fixup@+0x4C, ops@+0x50, compr_ops@+0x54,
  bool/flag bytes@+0x58}.

| # | name | stream | cpu_dai | codec_name | codec_dai | platform (+0x28) |
|---|------|--------|---------|------------|-----------|-------------------|
| 0 | HDMI | HMDI_PLayback (sic) | HDMI | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 1 | TDM Capture | TDM_Capture | TDM_IN | **tlv320aic3101.0-0018** | **tlv320aic3101-codec** | — (DT) |
| 2 | MultiMedia1 Capture | MultiMedia1_Capture | VUL | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 3 | AWB Capture | AWB_Record | AWB | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 4 | BTCVSD_RX | BTCVSD_Capture | snd-soc-dummy-dai | snd-soc-dummy | snd-soc-dummy-dai | **mt-soc-btcvsd-rx-pcm** |
| 5 | BTCVSD_TX | BTCVSD_Playback | snd-soc-dummy-dai | snd-soc-dummy | snd-soc-dummy-dai | **mt-soc-btcvsd-tx-pcm** |
| 6 | **DL1 Playback** | DL1_Playback | DL1 | **tas2770.2-0044** (static; runtime tas2770.2-0034, see below) | **tas2770 ASI1** | — (DT) |
| 7 | Ref In Capture | **DL1_AWB_Record** | AWB | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 8 | DL2 Playback | MultiMedia2_PLayback | DL2 | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 9 | DAI Capture | VOIP_Call_BT_Capture | DAI | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 10 | HDMI BE | — | HDMIO | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 11 | 2ND EXT Codec | — | 2ND I2S | linux_bt_sco_codec | bt-sco-pcm-wb | — (DT) |
| 12 | MTK Codec | — | INT ADDA | **mt8167-codec** | **mt8167-codec-dai** | — (DT) |
| 13 | TDM IN BE | — | TDM_IN_IO | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 14 | I2S BE | — | I2S | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 15 | DL BE | — | DL Input | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |
| 16 | HW Gain1 BE | — | HW_GAIN1 | snd-soc-dummy | snd-soc-dummy-dai | — (DT) |

**Platform binding (correction of the earlier "+0x2C platform_name" read):
platform_name lives at +0x28.** Only links 4/5 carry a platform string:
`mt-soc-btcvsd-rx-pcm` @0xc09a8072 (file off 0x9a0072, link[4]
@0xc0b28ff4+0x28) and `mt-soc-btcvsd-tx-pcm` @0xc09a8385 (file off
0x9a0385) — `D/donut.c:218,226`. All other 15 links have platform_name
AND platform_of_node (+0x2C) NULL statically: `D/donut.c:495-499` fills
`platform_of_node` at probe from the card's audio platform phandle — so
the AFE platform for links 0-3/6-16 binds via DT of_node at runtime, not
via a name in the static table. (The earlier "platform_name NULL for all
17" observation was the +0x2C of_node slot, not the name slot.)

**tas2770 address reconciliation (link[6]):** the static table carries
`tas2770.2-0044` = `#define TAS2770_CODEC_NAME` (`D/donut.c:30`), the
family default. At probe the driver reads DT `playback-codec-name` and
replaces codec_name on every link where it equals that default
(`D/donut.c:512-526`). Stock crumpet DTBs both say
`playback-codec-name = "tas2770.2-0034"` (probe-out/crumpet-dtb/
crumpet-mp-prq.dts:2451 and extracted/kernel/crumpet.dts:2451, identical),
matching the i2c node `tas5770@34 { compatible = "ti,tas5770" }` (both
DTS :1465-1466). **Runtime codec = tas2770.2-0034 = i2c bus 2, addr
0x34; the mainline DTS keeps i2c2 0x34** (as `reference/crumpet-codec-
deltas.md` already records). 0x44 is only the compiled-in default for
boards without the DT property.

**Numeric tail (validated against `D/donut.c` + `soc-dai.h`):**
dai_fmt@+0x3C uses this fork's encodings (I2S=1, LEFT_J=3, DSP_B=5;
CBS_CFS=4<<12=0x4000; soc-dai.h:29,31,33,86): link[1] TDM Capture =
0x4005 = CBS_CFS|DSP_B (`D/donut.c:177`), link[6] DL1 = 0x4001 =
CBS_CFS|I2S (`D/donut.c:242`), link[13] TDM IN BE = 0x4003 =
CBS_CFS|LEFT_J (`D/donut.c:327`). trigger[2]
@+0x40/+0x44 = {1,1} = {SND_SOC_DPCM_TRIGGER_POST, …} (soc-dpcm.h:61-65,
`D/donut.c:244-247`) on every FE link. Flag byte @+0x5B: bit0
no_pcm, bit1 dynamic, bit2 dpcm_capture, bit3 dpcm_playback —
e.g. link[6]=0x0a (dynamic+dpcm_playback, `D/donut.c:248-249`), capture
FEs (2/3/7/9)=0x06, BEs carry no_pcm (`D/donut.c:294,304,315,324,334,
344,352`). link[1].ops@+0x50 =
0xc0b294d4 = `tlv3101_machine_ops` (`D/donut.c:183`).

**Cross-checks holding:** mic TDM binds `tlv320aic3101.0-0018` (i2c0 0x18,
first codec; second @0x1a presumably via codec driver multi-instance);
echo-reference stream = `DL1_AWB_Record` (donut's name — the family
truth); integrated codec = `mt8167-codec` (donut's T8 port); TAS5770
front-ended as tas2770 under the tas2770 driver at i2c2 0x34 (§ above);
FE/BE split uses MTK's usual AFE names (DL1/DL2/VUL/AWB/TDM_IN/I2S/HDMI/
DAI) — these match lidar's mainline `mt8516-afe` link vocabulary for the
FE side.

## 3. Card DAPM tables (controls/widgets/routes)

The three card arrays pack back-to-back in .data:
controls 0xc07fcad8 (0x30) → routes 0xc07fcb08 (0xb0) → widgets
0xc07fcbb8 (5×0xb8) → the two DAPM switch kcontrols (0xc07fcf50,
0xc07fcf80) → pascal-card data begins at 0xc07fcfb0 (`pascal_ext_spk_amp…`).
All decode field-for-field against `D/donut.c:33-34,60-61,359-416`.

**Card control ×1 @0xc07fcad8** (0x30-byte 4.4 `snd_kcontrol_new`,
control.h:44-60): iface=MIXER, name=`LineIn ADC`@0xc09a9cbe,
info=0xc05766a8 (snd_ctl_info_boolean_value), get=0xc05d0608
(`donut_linein_adc_get`), put=0xc05d0680 (`donut_linein_adc_set`) —
`SOC_SINGLE_BOOL_EXT("LineIn ADC", …)`, `D/donut.c:412-416`.

**Routes ×0xb @0xc07fcb08** — 16-byte 4.4 `snd_soc_dapm_route`
{sink, control, source, connected} (soc-dapm.h:505-513; connected=0 on
all — 11×16=0xb0 spans exactly to the widgets array, which is what pins
the 16-byte entry size). Identical to `donut_audio_map`, `D/donut.c:371-392`,
including the duplicated HP Spk Amp route:

| # | sink | control | source |
|---|------|---------|--------|
| 0 | External Line Out | — | I2S Playback |
| 1 | LINEOUT Ext Amp | Switch | AU_LOL |
| 2 | Ext Spk Amp | — | LINEOUT Ext Amp |
| 3 | HP Ext Amp | Switch | AU_HPL |
| 4 | HP Ext Amp | Switch | AU_HPR |
| 5 | HP Spk Amp | — | HP Ext Amp |
| 6 | HP Spk Amp | — | HP Ext Amp (dup, source-line identical) |
| 7 | AIF TX | — | AFE_CLK |
| 8 | AIF TX | — | AD_CLK |
| 9 | AIF RX | — | AFE_CLK |
| 10 | AIF RX | — | DA_CLK |

**Widgets ×5 @0xc07fcbb8** — 0xb8-byte 4.4 `snd_soc_dapm_widget`
(id@+0x00, name@+0x04, sname@+0x08, reg@+0x2C, event_flags@+0x4C,
event@+0x50, num_kcontrols@+0x54, kcontrol_news@+0x58; soc-dapm.h field
order — num_kcontrols precedes kcontrol_news). Identical to
`donut_dapm_widgets`, `D/donut.c:359-369`:

| VA | id | name | type | notable fields |
|----|----|------|------|----------------|
| 0xc07fcbb8 | 1 | External Line Out | DAPM_OUTPUT | — |
| 0xc07fcc70 | 13 | HP Spk Amp | DAPM_SPK | event=0xc05d0650 `donut_hp_spk_amp_event` (D:37-58), flags 6 = POST_PMU\|PRE_PMD |
| 0xc07fcd28 | 15 | HP Ext Amp | DAPM_SWITCH | reg=SND_SOC_NOPM(-1), kcontrol_news=0xc07fcf80 ×1 |
| 0xc07fcde0 | 13 | Ext Spk Amp | DAPM_SPK | event=0xc05d0620 `donut_ext_spk_amp_wevent` (D:64-89), flags 6 |
| 0xc07fce98 | 15 | LINEOUT Ext Amp | DAPM_SWITCH | reg=SND_SOC_NOPM, kcontrol_news=0xc07fcf50 ×1 |

The two SWITCH widgets each carry a `SOC_DAPM_SINGLE_VIRT("Switch", 1)`
kcontrol (`D/donut.c:33-34,60-61`): @0xc07fcf80 (HP Ext Amp's) and
0xc07fcf50 (LINEOUT Ext Amp's), iface=MIXER, name `Switch`@0xc09a88c7,
get=0xc05b58e8 / put=0xc05b674c (`snd_soc_dapm_get/put_volsw`).

**Pascal note (closes old remaining-item 3):** the `Ext_Speaker_Amp_Switch`
string @0xc09aa065 and `__pascal_ext_speaker_amp_set` @0xc07fd03c live in
the pascal-card region that follows crumpet's arrays — pascal-machine-
driver items, NOT on crumpet's `mt-snd-card`; crumpet's ext-amp control
surface is the DAPM switch widgets above (plus the `LineIn ADC` bool).

## 4. Implications for T10 (machine driver skeleton)

- Card name `mt8516-crumpet` per plan; dai links to mirror (mainline
  naming, but keep stream names `DL1_AWB_Record`/`TDM_Capture` etc. so
  UCM + userspace recipes port).
- FE links on lidar's afe: DL1 (speaker → TAS5770), TDM_IN (mics), AWB
  (DL1_AWB_Record echo ref); dummy-codec for AWB/VUL. FE/BE wiring
  follows the flag byte: dynamic FE + dpcm_playback/capture BE pairs
  (§2 tail table).
- DAPM graph vocabulary for the UCM: speaker path I2S Playback →
  External Line Out; ext-amp switch widgets `LINEOUT Ext Amp`/`HP Ext
  Amp` (kcontrol `Switch`) feeding pins AU_LOL/AU_HPL/AU_HPR via the
  SPK widgets `Ext Spk Amp`/`HP Spk Amp` — reuse names verbatim so the
  stock mixer_paths.xml semantics port.
- Jack: int-codec `mt8167-codec` port from donut overlay (link 12 pattern).
- BTCVSD rx/tx links: standalone (no dynamic/dpcm flags), platform-bound
  by name — on mainline these map to the merged BT paths, not a separate
  platform (C4 validates).

## 5. Remaining (C4 live discovery only)

1. Rates/formats/TDM slots are NOT in static data — machine driver reads
   them from DT/audio_param at runtime (`audio_device.xml`/`audio_param/`,
   §1). C4 territory per plan.
2. AIC3101 second instance (i2c0 0x1a) wiring — codec-driver
   multi-instance assumption (§2 cross-check) to confirm live.
