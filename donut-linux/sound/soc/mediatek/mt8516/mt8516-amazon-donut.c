// SPDX-License-Identifier: GPL-2.0
/*
 * Amazon Echo Dot (3rd generation, "donut") ALSA SoC machine driver
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Derived from the MediaTek/Amazon mt8167 donut driver in Amazon's GPL
 * release (sound/soc/mediatek/mt8167/donut.c) and modeled on the machine
 * driver of the Echo Plus 2 (mt8516-amazon-lidar.c) built on the same AFE.
 *
 * Vendor link-table cross-check (Amazon 4.4 donut.c vs techo5 docs)
 * ------------------------------------------------------------------
 * The vendor card is a DPCM card with more front ends than the donut
 * hardware uses.  The donut-relevant subset, and where it lives here:
 *
 *   vendor FE "DL1 Playback" (stream DL1_Playback, cpu DL1, codec
 *     tas2770.2-0044, fmt I2S|NB_NF|CBS_CFS, dpcm_playback)
 *     -> FE "DL1" on the afe DL1 memory interface; the TAS2770 moves
 *        to the "I2S_BE" back end like on lidar.  The TAS2770 plays
 *     S16_LE and S24_LE at 96 kHz and stays silent on S32_LE
 *     (techo5 docs/hardware-donut.md), so the FE constrains the rate
 *     to 96 kHz and the formats to S16_LE/S24_LE.  DL1 needs a routed
 *     back end before it opens (techo5), which DPCM provides through
 *     I2S_BE.
 *
 *   vendor FE "TDM Capture" (stream TDM_Capture, cpu TDM_IN, codec
 *     tlv320aic3101.0-0018, fmt DSP_B|NB_NF|CBS_CFS, TDM slots =
 *     stream channels with 32 bit slots; codec PLL fed from the TDM
 *     bit clock, vendor divs entry 2048000 -> 16000: P=1 R=1 J=40
 *     D=0, NADC=10 MADC=4 AOSR=128)
 *     -> FE "UL1" on the afe TDM_IN memory interface.  Four analog
 *        microphones sit on two TLV320ADC3101 stereo ADCs, each codec
 *        filling the next two 32 bit slots of one TDM frame (techo5:
 *        "TDM capture, four channels", S32 with 24 significant bits).
 *        The back end is "TDM_IN_BE".  The afe accepts DSP_B and
 *        LEFT_J identically for the TDM input; the vendor BE used
 *        LEFT_J, this driver uses DSP_B as lidar does with the same
 *        AFE.  On the codec side the mainline tlv320adc3xxx driver
 *        expresses the DSP-mode data launch edge as IB (the vendor
 *        4.4 driver called it NB); the silicon is the same and the
 *        combination below is the one proven on lidar.
 *
 *   vendor FE "Ref In Capture" (stream DL1_AWB_Record, cpu AWB,
 *     dpcm_capture) -> FE "DL1_AWB" on the afe AWB memory interface,
 *     the playback loopback that techo5 uses as the echo reference
 *     ("card 0, device 7"); it follows whichever format DL1 runs in.
 *
 *   vendor FEs HDMI, MultiMedia1 (VUL), DL2 Playback, DAI Capture,
 *     BTCVSD RX/TX and BEs HDMI BE, 2ND EXT Codec (BT SCO over I2S0
 *     BCK/LRCK/DI + I2S3 DO, pins 36/37/38/20), HW Gain1 BE: HDMI,
 *     VUL/DL2/DAI memifs, the BT-SCO I2S link and the HW gain path
 *     have no donut use that mainline covers; the jack is the vendor
 *     "MTK Codec" BE: cpu INT ADDA (the AFE ADDA back end, ported
 *     with the codec), codec mt8167-codec-dai of the integrated
 *     codec, dpcm playback and capture, no dai_fmt (not a serial
 *     link).  On this card it is the "HP" link, present only when
 *     the device tree carries the jack codec (jack-dai-link),
 *     exactly the vendor shape.  With the jack present DL1 also
 *     opens at 48 kHz S24 for it (the speaker keeps 96 kHz; ALSA
 *     re-opens DL1 between uses, both cannot run at once because
 *     the codec tops out at 48 kHz).  The jack's headset-mic
 *     capture has no front end on the donut (the mic array owns
 *     UL1); the vendor card did not route it either.
 *
 * The vendor FE codec nodes (tas2770.2-0044 on DL1, tlv320aic3101.0-0018
 * on TDM Capture) are a v4.4-ism; on v7.3 the codecs hang off the back
 * ends through the audio dtsi (speaker-dai-link/mic-dai-link), exactly
 * as lidar does it.
 *
 * DAI link and card names are the contract of the donut UCM:
 * card "mt8516-donut", front ends "DL1", "UL1", "DL1_AWB" and the
 * jack link "HP" (stream names equal to the link names).  The back
 * ends keep lidar's names for the same AFE links: "I2S_BE",
 * "TDM_IN_BE", "DL_BE".
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/soc.h>

/* Every microphone ADC fills two 32 bit slots of the TDM frame. */
#define DONUT_MIC_SLOT_WIDTH	32
#define DONUT_MIC_MAX_CODECS	2

enum {
	/* FE */
	DAI_LINK_DL1_PLAYBACK,
	DAI_LINK_MIC_CAPTURE,
	DAI_LINK_AWB_CAPTURE,
	/* BE */
	DAI_LINK_SPEAKER,
	DAI_LINK_MIC,
	DAI_LINK_DL_LOOPBACK,
	/* jack back end; kept last — it is dropped when absent */
	DAI_LINK_HP,
};

/*
 * The speaker path runs at 96 kHz: the TAS2770 renders the high
 * frequencies of voices and music as static at 48 kHz (techo5 #20).
 * It plays S16_LE and S24_LE; on S32_LE it stays silent, so that
 * format is not offered.  The headphone jack on the integrated
 * codec runs DL1 at 48 kHz S24_LE: when the jack codec is wired up
 * (jack-dai-link), the rate constraint widens to the pair, and the
 * stream is re-opened per use case (spec §3).
 */
static bool donut_has_jack;

static const unsigned int donut_playback_rates[] = { 48000, 96000 };

static int donut_playback_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	if (donut_has_jack) {
		static const struct snd_pcm_hw_constraint_list rates = {
			.count = ARRAY_SIZE(donut_playback_rates),
			.list = donut_playback_rates,
		};

		ret = snd_pcm_hw_constraint_list(runtime, 0,
						 SNDRV_PCM_HW_PARAM_RATE,
						 &rates);
	} else {
		ret = snd_pcm_hw_constraint_single(runtime,
						   SNDRV_PCM_HW_PARAM_RATE,
						   96000);
	}
	if (ret < 0)
		return ret;

	return snd_pcm_hw_constraint_mask64(runtime,
					    SNDRV_PCM_HW_PARAM_FORMAT,
					    SNDRV_PCM_FMTBIT_S16_LE |
					    SNDRV_PCM_FMTBIT_S24_LE);
}

static const struct snd_soc_ops donut_playback_ops = {
	.startup = donut_playback_startup,
};

/*
 * The TDM frame layout is fixed by the ADCs: 2 * num_codecs slots of
 * 32 bits, carrying 24 significant bits per sample.  There is no
 * conversion, so the stream format and channel count are pinned.
 */
static int donut_mic_fe_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai_link *mic_link = &rtd->card->dai_link[DAI_LINK_MIC];
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	ret = snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_CHANNELS,
					   2 * mic_link->num_codecs);
	if (ret < 0)
		return ret;

	return snd_pcm_hw_constraint_mask64(runtime,
					    SNDRV_PCM_HW_PARAM_FORMAT,
					    SNDRV_PCM_FMTBIT_S32_LE);
}

static const struct snd_soc_ops donut_mic_fe_ops = {
	.startup = donut_mic_fe_startup,
};

/*
 * The ADCs share the data line of one TDM frame, each of them sending
 * its stereo pair in the next two slots.  They are clocked from the bit
 * clock and launch data on its rising edge, which is the inverted bit
 * clock polarity in ASoC terms, while the TDM input of the SoC samples
 * with its default polarity (as on lidar; the vendor 4.4 codec driver
 * named the same edge NB).
 */
static int donut_mic_init(struct snd_soc_pcm_runtime *rtd)
{
	unsigned int slots = 2 * rtd->dai_link->num_codecs;
	struct snd_soc_dai *codec_dai;
	int i, ret;

	ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_cpu(rtd, 0),
				  SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_BP_FP);
	if (ret)
		return ret;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		unsigned int mask = 0x3 << (2 * i);

		ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_B |
					  SND_SOC_DAIFMT_IB_NF |
					  SND_SOC_DAIFMT_BC_FC);
		if (ret)
			return ret;

		ret = snd_soc_dai_set_tdm_slot(codec_dai, mask, mask, slots,
					       DONUT_MIC_SLOT_WIDTH);
		if (ret)
			return ret;
	}

	return 0;
}

SND_SOC_DAILINK_DEFS(playback,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(mic_capture,
		     DAILINK_COMP_ARRAY(COMP_CPU("TDM_IN")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(awb_capture,
		     DAILINK_COMP_ARRAY(COMP_CPU("AWB")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(speaker,
		     DAILINK_COMP_ARRAY(COMP_CPU("I2S")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(mic,
		     DAILINK_COMP_ARRAY(COMP_CPU("TDM_IN_IO")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(dl_loopback,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL Input")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(hp,
		     DAILINK_COMP_ARRAY(COMP_CPU("INT ADDA")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link donut_dai_links[] = {
	/* Front End DAI links */
	[DAI_LINK_DL1_PLAYBACK] = {
		.name = "DL1",
		.stream_name = "DL1",
		.id = DAI_LINK_DL1_PLAYBACK,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.playback_only = 1,
		.dpcm_merged_format = 1,
		.dpcm_merged_rate = 1,
		.ops = &donut_playback_ops,
		SND_SOC_DAILINK_REG(playback),
	},
	[DAI_LINK_MIC_CAPTURE] = {
		.name = "UL1",
		.stream_name = "UL1",
		.id = DAI_LINK_MIC_CAPTURE,
		/* Start the memory interface before the TDM input feeds it. */
		.trigger = {
			SND_SOC_DPCM_TRIGGER_PRE,
			SND_SOC_DPCM_TRIGGER_PRE
		},
		.dynamic = 1,
		.capture_only = 1,
		.dpcm_merged_format = 1,
		.dpcm_merged_rate = 1,
		.ops = &donut_mic_fe_ops,
		SND_SOC_DAILINK_REG(mic_capture),
	},
	[DAI_LINK_AWB_CAPTURE] = {
		.name = "DL1_AWB",
		.stream_name = "DL1_AWB",
		.id = DAI_LINK_AWB_CAPTURE,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(awb_capture),
	},
	/* Back End DAI links */
	[DAI_LINK_SPEAKER] = {
		.name = "I2S_BE",
		.id = DAI_LINK_SPEAKER,
		.no_pcm = 1,
		.playback_only = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		SND_SOC_DAILINK_REG(speaker),
	},
	[DAI_LINK_MIC] = {
		.name = "TDM_IN_BE",
		.id = DAI_LINK_MIC,
		.no_pcm = 1,
		.capture_only = 1,
		.init = donut_mic_init,
		SND_SOC_DAILINK_REG(mic),
	},
	[DAI_LINK_DL_LOOPBACK] = {
		.name = "DL_BE",
		.id = DAI_LINK_DL_LOOPBACK,
		.no_pcm = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(dl_loopback),
	},
	/* the headphone jack on the integrated codec (vendor "MTK Codec").
	 * The vendor link set dpcm_playback and dpcm_capture; a back end
	 * without direction flags covers both directions here.
	 */
	[DAI_LINK_HP] = {
		.name = "HP",
		.stream_name = "HP",
		.id = DAI_LINK_HP,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(hp),
	},
};

static const struct snd_soc_dapm_widget donut_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
};

static const struct snd_soc_dapm_route donut_routes[] = {
	{"Speaker", NULL, "OUT"},
	/* codec jack outputs (mt8167-codec widgets) */
	{"Headphone", NULL, "AU_HPL"},
	{"Headphone", NULL, "AU_HPR"},
};

static struct snd_soc_card donut_card = {
	.name = "mt8516-donut",
	.owner = THIS_MODULE,
	.dai_link = donut_dai_links,
	.num_links = ARRAY_SIZE(donut_dai_links),
	.dapm_widgets = donut_widgets,
	.num_dapm_widgets = ARRAY_SIZE(donut_widgets),
	.dapm_routes = donut_routes,
	.num_dapm_routes = ARRAY_SIZE(donut_routes),
};

static void donut_put_node(void *data)
{
	of_node_put(data);
}

static void donut_put_codecs(void *data)
{
	struct snd_soc_card *card = data;

	snd_soc_of_put_dai_link_codecs(&card->dai_link[DAI_LINK_SPEAKER]);
	snd_soc_of_put_dai_link_codecs(&card->dai_link[DAI_LINK_MIC]);
	if (donut_has_jack)
		snd_soc_of_put_dai_link_codecs(&card->dai_link[DAI_LINK_HP]);
}

static int donut_parse_codecs(struct device *dev, const char *name,
			      struct snd_soc_dai_link *link)
{
	struct device_node *link_node __free(device_node) =
		of_get_child_by_name(dev->of_node, name);
	struct device_node *codec_node __free(device_node) =
		of_get_child_by_name(link_node, "codec");
	int ret;

	if (!codec_node)
		return dev_err_probe(dev, -EINVAL, "missing %s codec node\n",
				     name);

	ret = snd_soc_of_get_dai_link_codecs(dev, codec_node, link);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get %s codecs\n",
				     name);

	return 0;
}

static int donut_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &donut_card;
	struct device *dev = &pdev->dev;
	struct device_node *platform_node;
	struct device_node *link_node;
	struct snd_soc_dai_link *link;
	int i, ret;

	platform_node = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	if (!platform_node)
		return dev_err_probe(dev, -EINVAL,
				     "missing mediatek,platform property\n");

	/* The DAI links refer to the node for as long as the card exists. */
	ret = devm_add_action_or_reset(dev, donut_put_node, platform_node);
	if (ret)
		return ret;

	card->dev = dev;

	for_each_card_prelinks(card, i, link) {
		link->cpus->of_node = platform_node;
		link->platforms->of_node = platform_node;
	}

	ret = devm_add_action(dev, donut_put_codecs, card);
	if (ret)
		return ret;

	ret = donut_parse_codecs(dev, "speaker-dai-link",
				 &card->dai_link[DAI_LINK_SPEAKER]);
	if (ret)
		return ret;

	link = &card->dai_link[DAI_LINK_MIC];
	ret = donut_parse_codecs(dev, "mic-dai-link", link);
	if (ret)
		return ret;

	if (link->num_codecs > DONUT_MIC_MAX_CODECS)
		return dev_err_probe(dev, -EINVAL, "too many microphone ADCs\n");

	/*
	 * The jack back end exists only when the device tree wires the
	 * integrated codec; without it the card keeps the lidar shape
	 * (DL1 stays 96 kHz only).
	 */
	link_node = of_get_child_by_name(dev->of_node, "jack-dai-link");
	if (link_node) {
		of_node_put(link_node);

		ret = donut_parse_codecs(dev, "jack-dai-link",
					 &card->dai_link[DAI_LINK_HP]);
		if (ret)
			return ret;

		donut_has_jack = true;
	} else {
		card->num_links--;
	}

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id donut_dt_match[] = {
	{ .compatible = "amazon,donut-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, donut_dt_match);

static struct platform_driver donut_driver = {
	.driver = {
		.name = "mt8516-amazon-donut",
		.of_match_table = donut_dt_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = donut_probe,
};
module_platform_driver(donut_driver);

MODULE_DESCRIPTION("Amazon Echo Dot (3rd generation) ALSA SoC machine driver");
MODULE_LICENSE("GPL");
