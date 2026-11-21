# Crumpet audio forensics — stock HAL configs + machine-driver RE

**Status: C0 (plan Task 4) — RE complete at the dai-link-table level; links
0-6 detail below, link[6] dump pending (see §4).** Sources: shipped NS6570
kernel `extracted/kernel/vmlinux.raw` (arm32 4.4.22+, 11.78 MB; IDA db
`extracted/kernel/vmlinux_arm.i64`, link base **VA = 0xC0008000 + file
offset**), HAL configs from `extracted/system/rootfs/system/vendor/etc/`
(2025) + `extracted-6548/rootfs/system/vendor/etc/` (2021).

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

Crumpet card struct @ VA **0xc0b28ccc** (= .name field; card name string
@0xc09a9ca2, long name `mt-snd-card` @+0x40=0xc0b28d0c):
- controls/widgets/routes: ptr 0xc07fcad8 (×1), 0xc07fcb8 (×5),
  0xc07fcb08 (×0xb) @ 0xc0b28dac..cc.
- **dai_link table: 17 links (0x11) @ 0xc0b28e74**, entries are 0x60-byte
  4.4-era `snd_soc_dai_link` {name, stream_name, …, cpu_dai_name@+0x10,
  codec_name@+0x14, codec_dai_name@+0x1C, platform_name@+0x2C}.

| # | name | stream | cpu_dai | codec_name | codec_dai |
|---|------|--------|---------|------------|-----------|
| 0 | HDMI | HMDI_PLayback (sic) | HDMI | snd-soc-dummy | snd-soc-dummy-dai |
| 1 | TDM Capture | TDM_Capture | TDM_IN | **tlv320aic3101.0-0018** | **tlv320aic3101-codec** |
| 2 | MultiMedia1 Capture | MultiMedia1_Capture | VUL | snd-soc-dummy | snd-soc-dummy-dai |
| 3 | AWB Capture | AWB_Record | AWB | snd-soc-dummy | snd-soc-dummy-dai |
| 4 | BTCVSD_RX | BTCVSD_Capture | snd-soc-dummy-dai | snd-soc-dummy | platform mt-soc-btcvsd-rx-pcm |
| 5 | BTCVSD_TX | BTCVSD_Playback | snd-soc-dummy-dai | snd-soc-dummy | platform mt-soc-btcvsd-tx-pcm |
| 6 | **NOT YET DUMPED** (0xc0b290b4 — likely DL1 Playback w/ tas2770) | | | | |
| 7 | Ref In Capture | **DL1_AWB_Record** | AWB | snd-soc-dummy | snd-soc-dummy-dai |
| 8 | DL2 Playback | MultiMedia2_PLayback | DL2 | snd-soc-dummy | snd-soc-dummy-dai |
| 9 | DAI Capture | VOIP_Call_BT_Capture | DAI | snd-soc-dummy | snd-soc-dummy-dai |
| 10 | HDMI BE | — | HDMIO | snd-soc-dummy | snd-soc-dummy-dai |
| 11 | 2ND EXT Codec | — | 2ND I2S | linux_bt_sco_codec | bt-sco-pcm-wb |
| 12 | MTK Codec | — | INT ADDA | **mt8167-codec** | **mt8167-codec-dai** |
| 13 | TDM IN BE | — | TDM_IN_IO | snd-soc-dummy | snd-soc-dummy-dai |
| 14 | I2S BE | — | I2S | snd-soc-dummy | snd-soc-dummy-dai |
| 15 | DL BE | — | DL Input | snd-soc-dummy | snd-soc-dummy-dai |
| 16 | HW Gain1 BE | — | HW_GAIN1 | snd-soc-dummy | snd-soc-dummy-dai |

**Cross-checks holding:** mic TDM binds `tlv320aic3101.0-0018` (i2c0 0x18,
first codec; second @0x1a presumably via codec driver multi-instance);
echo-reference stream = `DL1_AWB_Record` (donut's name — the family truth);
integrated codec = `mt8167-codec` (donut's T8 port); TAS5770 front-ended as
`tas2770.2-0034` per the stock DTB (link 6 will confirm); FE/BE split uses
MTK's usual AFE names (DL1/DL2/VUL/AWB/TDM_IN/I2S/HDMI/DAI) — these match
lidar's mainline `mt8516-afe` link vocabulary for the FE side.

## 3. Implications for T10 (machine driver skeleton)

- Card name `mt8516-crumpet` per plan; dai links to mirror (mainline
  naming, but keep stream names `DL1_AWB_Record`/`TDM_Capture` etc. so
  UCM + userspace recipes port).
- FE links on lidar's afe: DL1 (speaker → TAS5770), TDM_IN (mics), AWB
  (DL1_AWB_Record echo ref); dummy-codec for AWB/VUL.
- Jack: int-codec `mt8167-codec` port from donut overlay (link 12 pattern).

## 4. Remaining (cheap, next session)

1. Dump link[6] @ 0xc0b290b4 (0x60 bytes, same decoder) — expect the
   DL1/tas2770 speaker link. Also dump links 0-6's +0x2C platform names.
2. Widgets/routes arrays (0xc07fcad8/0xc07fcb8/0xc07fcb08) + card ops ptrs
   (0xc07fc950, 0xc07e3d10) — DAPM route table for the UCM.
3. `Ext_Speaker_Amp_Switch` control + `__pascal_ext_speaker_amp_set`
   (strings @0x9a1xxx region) — the ext-amp control name for crumpet?
   (pascal-specific; check binding).
4. Live parameter discovery at C4 (rates/formats/TDM slots are NOT in
   static data — machine driver reads them from DT/audio_param).
