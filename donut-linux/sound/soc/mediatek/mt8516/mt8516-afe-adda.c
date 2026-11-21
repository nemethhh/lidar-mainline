// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8516 integrated ADDA (INT ADDA) back end DAI
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Port of the "INT ADDA" DAI of the MediaTek/Amazon mt8167 driver in
 * Amazon's 4.4 GPL release (dot3-6.5.7.0, sound/soc/mediatek/mt8167/
 * mt8167-afe-pcm.c) into the lidar MT8516 AFE framework, as its own
 * sub DAI like upstream does for the mt8183 ADDA.  The register
 * sequences are the vendor ones: the downlink path resamples the DL1
 * memory interface data through the DL SRC2 to the analog part of the
 * integrated codec, and the "i2s out" register block (AFE_I2S_CON1)
 * carries the serial link to it.
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../common/mtk-base-afe.h"
#include "mt8516-afe-adda.h"
#include "mt8516-afe-clk.h"
#include "mt8516-afe-common.h"
#include "mt8516-reg.h"

/*
 * ADDA registers of the MT8516 audio front end.  AFE_ADDA_TOP_CON0
 * and AFE_I2S_CON1 (with its field macros) already live in
 * mt8516-reg.h.
 */
#define AFE_ADDA_DL_SRC2_CON0		0x0108
#define AFE_ADDA_DL_SRC2_CON1		0x010c
#define AFE_ADDA_UL_SRC_CON0		0x0114
#define AFE_ADDA_UL_DL_CON0		0x0124
#define AFE_ADDA_NEWIF_CFG1		0x013c
#define AFE_ADDA_PREDIS_CON0		0x0260
#define AFE_ADDA_PREDIS_CON1		0x0264

/* AFE_ADDA_DL_SRC2_CON0 */
#define AFE_ADDA_DL_8X_UPSAMPLE		(BIT(25) | BIT(24))
#define AFE_ADDA_DL_MUTE_OFF		(BIT(12) | BIT(11))
#define AFE_ADDA_DL_VOICE_DATA		BIT(5)
#define AFE_ADDA_DL_DEGRADE_GAIN	BIT(1)
#define AFE_ADDA_DL_EN			BIT(0)

/* AFE_ADDA_UL_SRC_CON0 */
#define AFE_ADDA_UL_RATE_MASK		GENMASK(20, 17)
#define AFE_ADDA_UL_EN			BIT(0)

/* AFE_ADDA_UL_DL_CON0 */
#define AFE_ADDA_ON			BIT(0)

/* AFE_ADDA_NEWIF_CFG1 */
#define AFE_ADDA_NEWIF_CFG1_RATE_MASK	GENMASK(11, 10)

static const struct mt8516_afe_rate {
	unsigned int rate;
	unsigned int regvalue;
} mt8516_afe_adda_dl_rates[] = {
	{ .rate = 8000,  .regvalue = 0 },
	{ .rate = 11025, .regvalue = 1 },
	{ .rate = 12000, .regvalue = 2 },
	{ .rate = 16000, .regvalue = 3 },
	{ .rate = 22050, .regvalue = 4 },
	{ .rate = 24000, .regvalue = 5 },
	{ .rate = 32000, .regvalue = 6 },
	{ .rate = 44100, .regvalue = 7 },
	{ .rate = 48000, .regvalue = 8 },
};

static int mt8516_afe_adda_dl_rate_to_val(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt8516_afe_adda_dl_rates); i++)
		if (mt8516_afe_adda_dl_rates[i].rate == rate)
			return mt8516_afe_adda_dl_rates[i].regvalue;

	return -EINVAL;
}

/* the I2S rate field of the vendor mt8167_afe_i2s_rates[] table */
static const struct mt8516_afe_rate mt8516_afe_adda_i2s_rates[] = {
	{ .rate = 8000,  .regvalue = 0 },
	{ .rate = 11025, .regvalue = 1 },
	{ .rate = 12000, .regvalue = 2 },
	{ .rate = 16000, .regvalue = 4 },
	{ .rate = 22050, .regvalue = 5 },
	{ .rate = 24000, .regvalue = 6 },
	{ .rate = 32000, .regvalue = 8 },
	{ .rate = 44100, .regvalue = 9 },
	{ .rate = 48000, .regvalue = 10 },
};

static int mt8516_afe_adda_i2s_fs(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt8516_afe_adda_i2s_rates); i++)
		if (mt8516_afe_adda_i2s_rates[i].rate == rate)
			return mt8516_afe_adda_i2s_rates[i].regvalue;

	return -EINVAL;
}

/* vendor mt8167_afe_set_adda_out() */
static int mt8516_afe_set_adda_out(struct mtk_base_afe *afe, unsigned int rate)
{
	unsigned int val = 0;
	int ret = mt8516_afe_adda_dl_rate_to_val(rate);

	if (ret < 0)
		return ret;

	switch (rate) {
	case 8000:
	case 16000:
		/* voice band needs the voice data path */
		val |= AFE_ADDA_DL_VOICE_DATA;
		break;
	}

	val |= (ret << 28);
	val |= AFE_ADDA_DL_8X_UPSAMPLE |
		AFE_ADDA_DL_MUTE_OFF |
		AFE_ADDA_DL_DEGRADE_GAIN;

	regmap_update_bits(afe->regmap, AFE_ADDA_PREDIS_CON0, 0xffffffff, 0);
	regmap_update_bits(afe->regmap, AFE_ADDA_PREDIS_CON1, 0xffffffff, 0);
	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
			   0xffffffff, val);
	/* the down sample ratio of the SRC is fixed (vendor value) */
	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON1,
			   0xffffffff, 0xf74f0000);

	return 0;
}

/* vendor mt8167_afe_set_i2s_out() */
static int mt8516_afe_set_i2s_out(struct mtk_base_afe *afe,
				   unsigned int rate, int bit_width)
{
	unsigned int val;
	int fs = mt8516_afe_adda_i2s_fs(rate);

	if (fs < 0)
		return fs;

	/*
	 * The donut DT doubles the bit width of this bus
	 * (mediatek,bclk-factors <0 1 ...>, vendor property): a 24 bit
	 * sample always runs as a 32 bit word here.  WLEN_32BIT for
	 * everything wider than 8 bit covers that.
	 */
	val = AFE_I2S_CON1_I2S2_TO_PAD |
		AFE_I2S_CON1_LOW_JITTER_CLK |
		FIELD_PREP(AFE_I2S_CON1_RATE_MASK, fs) |
		AFE_I2S_CON1_FORMAT_I2S;
	if (bit_width > 8)
		val |= AFE_I2S_CON1_WLEN_32BIT;

	regmap_update_bits(afe->regmap, AFE_I2S_CON1,
			   ~(u32)AFE_I2S_CON1_EN, val);

	return 0;
}

/* vendor mt8167_afe_set_adda_in() */
static int mt8516_afe_set_adda_in(struct mtk_base_afe *afe, unsigned int rate)
{
	unsigned int val = 0;
	unsigned int val2 = 0;

	switch (rate) {
	case 8000:
		val2 = FIELD_PREP(AFE_ADDA_NEWIF_CFG1_RATE_MASK, 1);
		break;
	case 16000:
		val |= (1 << 17) | (1 << 19);
		val2 = FIELD_PREP(AFE_ADDA_NEWIF_CFG1_RATE_MASK, 1);
		break;
	case 32000:
		val |= (2 << 17) | (2 << 19);
		val2 = FIELD_PREP(AFE_ADDA_NEWIF_CFG1_RATE_MASK, 1);
		break;
	case 48000:
		val |= (3 << 17) | (3 << 19);
		val2 = FIELD_PREP(AFE_ADDA_NEWIF_CFG1_RATE_MASK, 3);
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
			   AFE_ADDA_UL_RATE_MASK, val);
	regmap_update_bits(afe->regmap, AFE_ADDA_NEWIF_CFG1,
			   AFE_ADDA_NEWIF_CFG1_RATE_MASK, val2);
	/* internal ADC of the ADDA (not the external one) */
	regmap_update_bits(afe->regmap, AFE_ADDA_TOP_CON0, 0x1, 0x0);

	return 0;
}

/* vendor mt8167_afe_enable_adda_on()/disable_adda_on() */
static void mt8516_afe_adda_on(struct mtk_base_afe *afe, bool enable)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	guard(mutex)(&afe_priv->clk_lock);

	if (enable) {
		if (afe_priv->adda_on_ref_cnt++)
			return;
	} else {
		if (--afe_priv->adda_on_ref_cnt)
			return;
	}

	regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
			   AFE_ADDA_ON, enable ? AFE_ADDA_ON : 0);
}

static void mt8516_afe_set_adda_out_enable(struct mtk_base_afe *afe,
					   bool enable)
{
	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
			   AFE_ADDA_DL_EN, enable ? AFE_ADDA_DL_EN : 0);

	mt8516_afe_adda_on(afe, enable);
}

static void mt8516_afe_set_i2s_out_enable(struct mtk_base_afe *afe,
					  bool enable)
{
	regmap_update_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_EN,
			   enable ? AFE_I2S_CON1_EN : 0);
}

static void mt8516_afe_set_adda_in_enable(struct mtk_base_afe *afe,
					  bool enable)
{
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
			   AFE_ADDA_UL_EN, enable ? AFE_ADDA_UL_EN : 0);

	mt8516_afe_adda_on(afe, enable);
}

/* vendor mt8167_afe_int_adda_startup() */
static int mt8516_afe_int_adda_startup(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	int ret;

	ret = mt8516_afe_enable_main_clk(afe);
	if (ret)
		return ret;

	mt8516_afe_enable_afe_on(afe);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		mt8516_afe_enable_top_cg(afe, MT8516_AFE_CG_DAC);
		mt8516_afe_enable_top_cg(afe, MT8516_AFE_CG_DAC_PREDIS);
	} else {
		mt8516_afe_enable_top_cg(afe, MT8516_AFE_CG_ADC);
	}
	udelay(100);

	return 0;
}

/* vendor mt8167_afe_int_adda_shutdown() */
static void mt8516_afe_int_adda_shutdown(struct snd_pcm_substream *substream,
					 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_adda_data *adda = &afe_priv->adda;
	const unsigned int stream = substream->stream;

	if (adda->prepared[stream]) {
		if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
			mt8516_afe_set_adda_out_enable(afe, false);
			mt8516_afe_set_i2s_out_enable(afe, false);
		} else {
			mt8516_afe_set_adda_in_enable(afe, false);
		}

		adda->prepared[stream] = false;
	}

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		mt8516_afe_disable_top_cg(afe, MT8516_AFE_CG_DAC);
		mt8516_afe_disable_top_cg(afe, MT8516_AFE_CG_DAC_PREDIS);
	} else {
		mt8516_afe_disable_top_cg(afe, MT8516_AFE_CG_ADC);
	}
	mt8516_afe_disable_afe_on(afe);
	mt8516_afe_disable_main_clk(afe);
}

/* vendor mt8167_afe_int_adda_hw_params() */
static int mt8516_afe_int_adda_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params,
					 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int width_val = params_width(params) > 16 ?
		(AFE_CONN_24BIT_O03 | AFE_CONN_24BIT_O04) : 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		regmap_update_bits(afe->regmap, AFE_CONN_24BIT,
			AFE_CONN_24BIT_O03 | AFE_CONN_24BIT_O04, width_val);

	return 0;
}

/* vendor mt8167_afe_int_adda_prepare() */
static int mt8516_afe_int_adda_prepare(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_adda_data *adda = &afe_priv->adda;
	const unsigned int rate = substream->runtime->rate;
	const unsigned int stream = substream->stream;
	const int bit_width = snd_pcm_format_width(substream->runtime->format);
	int ret;

	if (adda->prepared[stream]) {
		dev_dbg(afe->dev, "%s '%s' prepared already\n",
			__func__, snd_pcm_stream_str(substream));
		return 0;
	}

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = mt8516_afe_set_adda_out(afe, rate);
		if (ret)
			return ret;

		ret = mt8516_afe_set_i2s_out(afe, rate, bit_width);
		if (ret)
			return ret;

		mt8516_afe_set_adda_out_enable(afe, true);
		mt8516_afe_set_i2s_out_enable(afe, true);
	} else {
		ret = mt8516_afe_set_adda_in(afe, rate);
		if (ret)
			return ret;

		mt8516_afe_set_adda_in_enable(afe, true);
	}

	adda->prepared[stream] = true;

	return 0;
}

static const struct snd_soc_dai_ops mt8516_afe_int_adda_ops = {
	.startup	= mt8516_afe_int_adda_startup,
	.shutdown	= mt8516_afe_int_adda_shutdown,
	.hw_params	= mt8516_afe_int_adda_hw_params,
	.prepare	= mt8516_afe_int_adda_prepare,
};

static struct snd_soc_dai_driver mt8516_afe_adda_dai_driver[] = {
	{
		.name = "INT ADDA",
		.id = MT8516_AFE_IO_INT_ADDA,
		.playback = {
			.stream_name = "INT ADDA Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
		},
		.capture = {
			.stream_name = "INT ADDA Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000 |
				SNDRV_PCM_RATE_16000 |
				SNDRV_PCM_RATE_32000 |
				SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &mt8516_afe_int_adda_ops,
	},
};

/*
 * Like the vendor AFE graph: the downlink mixers O03/O04 feed the
 * ADDA the same way they feed the I2S output (the vendor had an
 * always-on switch in between; the lidar port of the I2S link uses
 * the direct route).
 */
static const struct snd_soc_dapm_route mt8516_afe_adda_routes[] = {
	{"INT ADDA Playback", NULL, "O03"},
	{"INT ADDA Playback", NULL, "O04"},
};

int mt8516_afe_adda_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	dai->dai_drivers = mt8516_afe_adda_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt8516_afe_adda_dai_driver);
	dai->dapm_routes = mt8516_afe_adda_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt8516_afe_adda_routes);
	list_add(&dai->list, &afe->sub_dais);

	return 0;
}
