// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8516 ALSA SoC audio front end platform driver
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Derived from the MediaTek/Amazon mt8167 driver in Amazon's GPL release.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt8516-afe-clk.h"
#include "mt8516-afe-common.h"
#include "mt8516-reg.h"
#include "../common/mtk-afe-fe-dai.h"
#include "../common/mtk-afe-platform-driver.h"
#include "../common/mtk-base-afe.h"

#define MT8516_AFE_MCLK_MULTIPLIER	256

#define MT8516_AFE_RATES	SNDRV_PCM_RATE_8000_192000
#define MT8516_AFE_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

static const unsigned int mt8516_afe_backup_list[] = {
	AUDIO_TOP_CON0,
	AFE_CONN1,
	AFE_CONN2,
	AFE_CONN_24BIT,
	AFE_DAC_CON0,
	AFE_DAC_CON1,
	AFE_DL1_BASE,
	AFE_DL1_END,
	AFE_AWB_BASE,
	AFE_AWB_END,
	AFE_HDMI_IN_2CH_BASE,
	AFE_HDMI_IN_2CH_END,
	AFE_MEMIF_PBUF_SIZE,
	AFE_MEMIF_PBUF2_SIZE,
};

static const struct snd_pcm_hardware mt8516_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 512 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.fifo_size = 0,
};

struct mt8516_afe_rate {
	unsigned int rate;
	unsigned int regvalue;
};

static const struct mt8516_afe_rate mt8516_afe_rates[] = {
	{ .rate = 8000, .regvalue = 0 },
	{ .rate = 11025, .regvalue = 1 },
	{ .rate = 12000, .regvalue = 2 },
	{ .rate = 16000, .regvalue = 4 },
	{ .rate = 22050, .regvalue = 5 },
	{ .rate = 24000, .regvalue = 6 },
	{ .rate = 32000, .regvalue = 8 },
	{ .rate = 44100, .regvalue = 9 },
	{ .rate = 48000, .regvalue = 10 },
	{ .rate = 88200, .regvalue = 11 },
	{ .rate = 96000, .regvalue = 12 },
	{ .rate = 176400, .regvalue = 13 },
	{ .rate = 192000, .regvalue = 14 },
};

static int mt8516_afe_fs(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt8516_afe_rates); i++)
		if (mt8516_afe_rates[i].rate == rate)
			return mt8516_afe_rates[i].regvalue;

	return -EINVAL;
}

static int mt8516_afe_memif_fs(struct snd_pcm_substream *substream,
			       unsigned int rate)
{
	return mt8516_afe_fs(rate);
}

/* The 44.1 kHz rate family runs from APLL1, the 48 kHz family from APLL2. */
static int mt8516_afe_rate_to_apll(unsigned int rate)
{
	return rate % 8000 ? MT8516_AFE_APLL1 : MT8516_AFE_APLL2;
}

static struct clk *mt8516_afe_apll_to_clk(struct mt8516_afe_private *afe_priv,
					  int apll)
{
	return afe_priv->clocks[apll == MT8516_AFE_APLL1 ? MT8516_CLK_AUD1 :
							    MT8516_CLK_AUD2];
}

/* FE DAIs: memory interfaces */

static const unsigned int mt8516_afe_tdm_in_channels[] = { 2, 4, 6, 8 };

static const struct snd_pcm_hw_constraint_list mt8516_afe_tdm_in_ch_list = {
	.count = ARRAY_SIZE(mt8516_afe_tdm_in_channels),
	.list = mt8516_afe_tdm_in_channels,
};

static int mt8516_afe_fe_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	int ret;

	ret = mt8516_afe_enable_main_clk(afe);
	if (ret)
		return ret;

	mt8516_afe_enable_afe_on(afe);

	ret = mtk_afe_fe_startup(substream, dai);
	if (ret >= 0 && dai->id == MT8516_AFE_MEMIF_TDM_IN)
		ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
						 SNDRV_PCM_HW_PARAM_CHANNELS,
						 &mt8516_afe_tdm_in_ch_list);
	if (ret < 0) {
		afe->memif[dai->id].substream = NULL;
		mt8516_afe_disable_afe_on(afe);
		mt8516_afe_disable_main_clk(afe);
		return ret;
	}

	return 0;
}

static void mt8516_afe_fe_shutdown(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);

	mtk_afe_fe_shutdown(substream, dai);
	/*
	 * All memifs use const_irq, for which the common helper keeps
	 * memif->substream; drop it so the IRQ handler never sees a closed
	 * stream.
	 */
	afe->memif[dai->id].substream = NULL;
	mt8516_afe_disable_afe_on(afe);
	mt8516_afe_disable_main_clk(afe);
}

static int mt8516_afe_fe_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int mask = AFE_CONN_24BIT_O05 | AFE_CONN_24BIT_O06;
	int ret;

	ret = mtk_afe_fe_hw_params(substream, params, dai);
	if (ret)
		return ret;

	if (dai->id == MT8516_AFE_MEMIF_AWB)
		regmap_update_bits(afe->regmap, AFE_CONN_24BIT, mask,
				   params_width(params) > 16 ? mask : 0);

	return 0;
}

static const struct snd_soc_dai_ops mt8516_afe_fe_dai_ops = {
	.startup	= mt8516_afe_fe_startup,
	.shutdown	= mt8516_afe_fe_shutdown,
	.hw_params	= mt8516_afe_fe_hw_params,
	.hw_free	= mtk_afe_fe_hw_free,
	.prepare	= mtk_afe_fe_prepare,
	.trigger	= mtk_afe_fe_trigger,
};

/*
 * BE DAI: I2S output on the I2S_8CH pads.
 *
 * The pads are clocked by the I2S input block, which shares bit and frame
 * clock with the output block, so both blocks and both master clock dividers
 * are always set up together.
 */

static int mt8516_afe_i2s_set_mclk(struct mtk_base_afe *afe, unsigned int mclk)
{
	static const struct {
		unsigned int sel;
		unsigned int div;
	} clks[] = {
		{ MT8516_CLK_I2S1_M_SEL, MT8516_CLK_APLL12_DIV1 },
		{ MT8516_CLK_I2S2_M_SEL, MT8516_CLK_APLL12_DIV2 },
	};
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct clk *parent;
	int i, ret;

	parent = mt8516_afe_apll_to_clk(afe_priv, mt8516_afe_rate_to_apll(mclk));

	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		ret = clk_set_parent(afe_priv->clocks[clks[i].sel], parent);
		if (ret)
			return ret;

		ret = clk_set_rate(afe_priv->clocks[clks[i].div], mclk);
		if (ret)
			return ret;
	}

	return 0;
}

static void mt8516_afe_i2s_stop(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_i2s_data *i2s = &afe_priv->i2s;

	if (i2s->apll == MT8516_AFE_APLL_NONE)
		return;

	regmap_clear_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_EN);
	regmap_clear_bits(afe->regmap, AFE_I2S_CON2, AFE_I2S_CON2_EN);
	mt8516_afe_disable_apll(afe, i2s->apll);
	i2s->apll = MT8516_AFE_APLL_NONE;
}

static int mt8516_afe_i2s_startup(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = mt8516_afe_enable_main_clk(afe);
	if (ret)
		return ret;

	ret = clk_prepare_enable(afe_priv->clocks[MT8516_CLK_APLL12_DIV1]);
	if (ret)
		goto err_main_clk;

	ret = clk_prepare_enable(afe_priv->clocks[MT8516_CLK_APLL12_DIV2]);
	if (ret)
		goto err_div1;

	return 0;

err_div1:
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_APLL12_DIV1]);
err_main_clk:
	mt8516_afe_disable_main_clk(afe);
	return ret;
}

static void mt8516_afe_i2s_shutdown(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	mt8516_afe_i2s_stop(afe);
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_APLL12_DIV2]);
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_APLL12_DIV1]);
	mt8516_afe_disable_main_clk(afe);
}

static int mt8516_afe_i2s_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_i2s_data *i2s = &afe_priv->i2s;
	unsigned int mask = AFE_CONN_24BIT_O03 | AFE_CONN_24BIT_O04;

	i2s->rate = params_rate(params);
	i2s->width = params_width(params);

	regmap_update_bits(afe->regmap, AFE_CONN_24BIT, mask,
			   i2s->width > 16 ? mask : 0);

	return 0;
}

static int mt8516_afe_i2s_prepare(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_i2s_data *i2s = &afe_priv->i2s;
	int apll = mt8516_afe_rate_to_apll(i2s->rate);
	int fs = mt8516_afe_fs(i2s->rate);
	unsigned int mclk, val;
	int ret;

	if (fs < 0)
		return fs;

	/* Do not disturb the running clocks when recovering from an xrun. */
	if (i2s->apll != MT8516_AFE_APLL_NONE &&
	    i2s->active_rate == i2s->rate && i2s->active_width == i2s->width)
		return 0;

	mclk = i2s->mclk_rate;
	if (!mclk)
		mclk = i2s->rate * MT8516_AFE_MCLK_MULTIPLIER;

	if (mt8516_afe_rate_to_apll(mclk) != apll) {
		dev_err(afe->dev, "MCLK %u Hz does not fit sample rate %u Hz\n",
			mclk, i2s->rate);
		return -EINVAL;
	}

	mt8516_afe_i2s_stop(afe);

	val = AFE_I2S_CON1_I2S2_TO_PAD | AFE_I2S_CON1_LOW_JITTER_CLK |
	      FIELD_PREP(AFE_I2S_CON1_RATE_MASK, fs) | AFE_I2S_CON1_FORMAT_I2S;
	if (i2s->width > 16)
		val |= AFE_I2S_CON1_WLEN_32BIT;
	regmap_update_bits(afe->regmap, AFE_I2S_CON1,
			   ~(u32)AFE_I2S_CON1_EN, val);

	/* The update word field has to keep its reset value. */
	val = AFE_I2S_CON2_LOW_JITTER_CLK |
	      FIELD_PREP(AFE_I2S_CON2_RATE_MASK, fs) | AFE_I2S_CON2_FORMAT_I2S;
	if (i2s->width > 16)
		val |= AFE_I2S_CON2_WLEN_32BIT;
	regmap_update_bits(afe->regmap, AFE_I2S_CON2,
			   ~(u32)(AFE_I2S_CON2_EN | AFE_I2S_CON2_UPDATE_WORD),
			   val);
	regmap_set_bits(afe->regmap, AFE_ADDA_TOP_CON0,
			AFE_ADDA_TOP_CON0_EXT_ADC);

	ret = mt8516_afe_enable_apll(afe, apll);
	if (ret)
		return ret;

	ret = mt8516_afe_i2s_set_mclk(afe, mclk);
	if (ret) {
		mt8516_afe_disable_apll(afe, apll);
		return ret;
	}

	regmap_set_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_EN);
	regmap_set_bits(afe->regmap, AFE_I2S_CON2, AFE_I2S_CON2_EN);

	i2s->apll = apll;
	i2s->active_rate = i2s->rate;
	i2s->active_width = i2s->width;

	return 0;
}

static int mt8516_afe_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				     unsigned int freq, int dir)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	if (dir != SND_SOC_CLOCK_OUT)
		return -EINVAL;

	afe_priv->i2s.mclk_rate = freq;
	if (!freq)
		return 0;

	/*
	 * Set the dividers right away so that a codec deriving its own
	 * clocking from MCLK sees the final rate in its hw_params().
	 */
	return mt8516_afe_i2s_set_mclk(afe, freq);
}

static int mt8516_afe_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops mt8516_afe_i2s_ops = {
	.startup	= mt8516_afe_i2s_startup,
	.shutdown	= mt8516_afe_i2s_shutdown,
	.hw_params	= mt8516_afe_i2s_hw_params,
	.prepare	= mt8516_afe_i2s_prepare,
	.set_sysclk	= mt8516_afe_i2s_set_sysclk,
	.set_fmt	= mt8516_afe_i2s_set_fmt,
};

/* BE DAI: TDM input, the SoC provides bit and frame clock */

static unsigned int mt8516_afe_tdm_ch_fixup(unsigned int channels)
{
	if (channels > 4)
		return 8;
	else if (channels > 2)
		return 4;
	else
		return 2;
}

static void mt8516_afe_tdm_in_release_apll(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_tdm_in_data *tdm = &afe_priv->tdm_in;

	if (tdm->apll == MT8516_AFE_APLL_NONE)
		return;

	mt8516_afe_disable_apll(afe, tdm->apll);
	tdm->apll = MT8516_AFE_APLL_NONE;
}

static int mt8516_afe_tdm_in_startup(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = mt8516_afe_enable_main_clk(afe);
	if (ret)
		return ret;

	ret = clk_prepare_enable(afe_priv->clocks[MT8516_CLK_APLL12_DIV5]);
	if (ret)
		goto err_main_clk;

	ret = clk_prepare_enable(afe_priv->clocks[MT8516_CLK_APLL12_DIV5B]);
	if (ret)
		goto err_div5;

	return 0;

err_div5:
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_APLL12_DIV5]);
err_main_clk:
	mt8516_afe_disable_main_clk(afe);
	return ret;
}

static void mt8516_afe_tdm_in_shutdown(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	mt8516_afe_tdm_in_release_apll(afe);
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_APLL12_DIV5B]);
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_APLL12_DIV5]);
	mt8516_afe_disable_main_clk(afe);
}

static int mt8516_afe_tdm_in_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params,
				       struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_tdm_in_data *tdm = &afe_priv->tdm_in;

	tdm->rate = params_rate(params);
	tdm->channels = params_channels(params);
	tdm->width = params_physical_width(params);

	return 0;
}

static int mt8516_afe_tdm_in_prepare(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	struct mt8516_afe_tdm_in_data *tdm = &afe_priv->tdm_in;
	int apll = mt8516_afe_rate_to_apll(tdm->rate);
	unsigned int val = 0;
	int ret;

	switch (tdm->channels) {
	case 2:
		val |= AFE_TDM_IN_CON1_2CH_PER_SDATA;
		break;
	case 4:
		val |= AFE_TDM_IN_CON1_4CH_PER_SDATA;
		break;
	case 6:
		val |= AFE_TDM_IN_CON1_8CH_PER_SDATA |
		       AFE_TDM_IN_CON1_DISABLE_CH67;
		break;
	case 8:
		val |= AFE_TDM_IN_CON1_8CH_PER_SDATA;
		break;
	default:
		return -EINVAL;
	}

	if (tdm->apll != apll) {
		mt8516_afe_tdm_in_release_apll(afe);

		ret = mt8516_afe_enable_apll(afe, apll);
		if (ret)
			return ret;

		tdm->apll = apll;
	}

	ret = clk_set_parent(afe_priv->clocks[MT8516_CLK_I2S5_M_SEL],
			     mt8516_afe_apll_to_clk(afe_priv, apll));
	if (ret)
		return ret;

	/* The bit clock divider is fed by the master clock divider. */
	ret = clk_set_rate(afe_priv->clocks[MT8516_CLK_APLL12_DIV5],
			   tdm->rate * MT8516_AFE_MCLK_MULTIPLIER);
	if (ret)
		return ret;

	ret = clk_set_rate(afe_priv->clocks[MT8516_CLK_APLL12_DIV5B],
			   mt8516_afe_tdm_ch_fixup(tdm->channels) * tdm->rate *
			   tdm->width);
	if (ret)
		return ret;

	if ((tdm->fmt & SND_SOC_DAIFMT_FORMAT_MASK) == SND_SOC_DAIFMT_I2S)
		val |= AFE_TDM_IN_CON1_I2S;

	switch (tdm->fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_IB_IF:
		val |= AFE_TDM_IN_CON1_LRCK_INV | AFE_TDM_IN_CON1_BCK_INV;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		val |= AFE_TDM_IN_CON1_LRCK_INV;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		val |= AFE_TDM_IN_CON1_BCK_INV;
		break;
	default:
		break;
	}

	if (tdm->width > 16)
		val |= AFE_TDM_IN_CON1_WLEN_32BIT |
		       AFE_TDM_IN_CON1_FAST_LRCK_CYCLE_32BCK |
		       AFE_TDM_IN_CON1_LRCK_WIDTH(32);
	else
		val |= AFE_TDM_IN_CON1_WLEN_16BIT |
		       AFE_TDM_IN_CON1_FAST_LRCK_CYCLE_16BCK |
		       AFE_TDM_IN_CON1_LRCK_WIDTH(16);

	regmap_update_bits(afe->regmap, AFE_TDM_IN_CON1,
			   ~(u32)AFE_TDM_IN_CON1_EN, val);

	regmap_update_bits(afe->regmap, AFE_CONN_TDMIN_CON,
			   AFE_CONN_TDMIN_CON0_MASK,
			   AFE_CONN_TDMIN_O40_I40 | AFE_CONN_TDMIN_O41_I41);

	return 0;
}

static int mt8516_afe_tdm_in_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;

	/*
	 * Without the one bit clock delay of I2S, the first slot starts
	 * together with the frame clock: left justified for stereo, DSP mode
	 * B for a multi channel TDM frame.
	 */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_LEFT_J:
	case SND_SOC_DAIFMT_DSP_B:
		break;
	default:
		return -EINVAL;
	}

	afe_priv->tdm_in.fmt = fmt;

	return 0;
}

static int mt8516_afe_tdm_in_trigger(struct snd_pcm_substream *substream,
				     int cmd, struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return regmap_set_bits(afe->regmap, AFE_TDM_IN_CON1,
				       AFE_TDM_IN_CON1_EN);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return regmap_clear_bits(afe->regmap, AFE_TDM_IN_CON1,
					 AFE_TDM_IN_CON1_EN);
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt8516_afe_tdm_in_ops = {
	.startup	= mt8516_afe_tdm_in_startup,
	.shutdown	= mt8516_afe_tdm_in_shutdown,
	.hw_params	= mt8516_afe_tdm_in_hw_params,
	.prepare	= mt8516_afe_tdm_in_prepare,
	.set_fmt	= mt8516_afe_tdm_in_set_fmt,
	.trigger	= mt8516_afe_tdm_in_trigger,
};

static struct snd_soc_dai_driver mt8516_afe_dai_drivers[] = {
	/* FE DAIs: memory interfaces to CPU */
	{
		.name = "DL1",
		.id = MT8516_AFE_MEMIF_DL1,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT8516_AFE_RATES,
			.formats = MT8516_AFE_FORMATS,
		},
		.ops = &mt8516_afe_fe_dai_ops,
	}, {
		.name = "AWB",
		.id = MT8516_AFE_MEMIF_AWB,
		.capture = {
			.stream_name = "AWB",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT8516_AFE_RATES,
			.formats = MT8516_AFE_FORMATS,
		},
		.ops = &mt8516_afe_fe_dai_ops,
	}, {
		.name = "TDM_IN",
		.id = MT8516_AFE_MEMIF_TDM_IN,
		.capture = {
			.stream_name = "TDM_IN",
			.channels_min = 2,
			.channels_max = 8,
			.rates = MT8516_AFE_RATES,
			.formats = MT8516_AFE_FORMATS,
		},
		.ops = &mt8516_afe_fe_dai_ops,
	},
	/* BE DAIs */
	{
		.name = "I2S",
		.id = MT8516_AFE_IO_I2S,
		.playback = {
			.stream_name = "I2S Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT8516_AFE_RATES,
			.formats = MT8516_AFE_FORMATS,
		},
		.ops = &mt8516_afe_i2s_ops,
	}, {
		.name = "TDM_IN_IO",
		.id = MT8516_AFE_IO_TDM_IN,
		.capture = {
			.stream_name = "TDM IN Capture",
			.channels_min = 2,
			.channels_max = 8,
			.rates = MT8516_AFE_RATES,
			.formats = MT8516_AFE_FORMATS,
		},
		.ops = &mt8516_afe_tdm_in_ops,
	}, {
		/* Loops the downlink data back into the AWB memory interface */
		.name = "DL Input",
		.id = MT8516_AFE_IO_DL_BE,
		.capture = {
			.stream_name = "DL Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT8516_AFE_RATES,
			.formats = MT8516_AFE_FORMATS,
		},
	},
};

static const struct snd_kcontrol_new mt8516_afe_o03_mix[] = {
	SOC_DAPM_SINGLE("I05 Switch", AFE_CONN1, AFE_CONN1_I05_O03_S, 1, 0),
};

static const struct snd_kcontrol_new mt8516_afe_o04_mix[] = {
	SOC_DAPM_SINGLE("I06 Switch", AFE_CONN2, AFE_CONN2_I06_O04_S, 1, 0),
};

static const struct snd_kcontrol_new mt8516_afe_o05_mix[] = {
	SOC_DAPM_SINGLE("I05 Switch", AFE_CONN2, AFE_CONN2_I05_O05_S, 1, 0),
};

static const struct snd_kcontrol_new mt8516_afe_o06_mix[] = {
	SOC_DAPM_SINGLE("I06 Switch", AFE_CONN2, AFE_CONN2_I06_O06_S, 1, 0),
};

static const struct snd_soc_dapm_widget mt8516_afe_widgets[] = {
	/* inter-connections */
	SND_SOC_DAPM_MIXER("I05", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I06", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I05L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I06L", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("O03", SND_SOC_NOPM, 0, 0,
			   mt8516_afe_o03_mix, ARRAY_SIZE(mt8516_afe_o03_mix)),
	SND_SOC_DAPM_MIXER("O04", SND_SOC_NOPM, 0, 0,
			   mt8516_afe_o04_mix, ARRAY_SIZE(mt8516_afe_o04_mix)),
	SND_SOC_DAPM_MIXER("O05", SND_SOC_NOPM, 0, 0,
			   mt8516_afe_o05_mix, ARRAY_SIZE(mt8516_afe_o05_mix)),
	SND_SOC_DAPM_MIXER("O06", SND_SOC_NOPM, 0, 0,
			   mt8516_afe_o06_mix, ARRAY_SIZE(mt8516_afe_o06_mix)),

	SND_SOC_DAPM_INPUT("DL Source"),
};

static const struct snd_soc_dapm_route mt8516_afe_routes[] = {
	/* downlink */
	{"I05", NULL, "DL1"},
	{"I06", NULL, "DL1"},
	{"O03", "I05 Switch", "I05"},
	{"O04", "I06 Switch", "I06"},
	{"I2S Playback", NULL, "O03"},
	{"I2S Playback", NULL, "O04"},

	/* downlink loopback */
	{"DL Capture", NULL, "DL Source"},
	{"I05L", NULL, "DL Capture"},
	{"I06L", NULL, "DL Capture"},
	{"O05", "I05 Switch", "I05L"},
	{"O06", "I06 Switch", "I06L"},
	{"AWB", NULL, "O05"},
	{"AWB", NULL, "O06"},

	/* uplink */
	{"TDM_IN", NULL, "TDM IN Capture"},
};

static const struct mtk_base_memif_data mt8516_afe_memif_data[MT8516_AFE_MEMIF_NUM] = {
	[MT8516_AFE_MEMIF_DL1] = {
		.name = "DL1",
		.id = MT8516_AFE_MEMIF_DL1,
		.reg_ofs_base = AFE_DL1_BASE,
		.reg_ofs_cur = AFE_DL1_CUR,
		.reg_ofs_end = AFE_DL1_END,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = 0,
		.fs_maskbit = 0xf,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = 21,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = 1,
		.hd_reg = AFE_MEMIF_PBUF_SIZE,
		.hd_shift = 16,
		.hd_align_reg = AFE_MEMIF_PBUF_SIZE,
		.hd_align_mshift = 17,
		.agent_disable_reg = -1,
	},
	[MT8516_AFE_MEMIF_AWB] = {
		.name = "AWB",
		.id = MT8516_AFE_MEMIF_AWB,
		.reg_ofs_base = AFE_AWB_BASE,
		.reg_ofs_cur = AFE_AWB_CUR,
		.reg_ofs_end = AFE_AWB_END,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = 12,
		.fs_maskbit = 0xf,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = 24,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = 6,
		.hd_reg = AFE_MEMIF_PBUF_SIZE,
		.hd_shift = 20,
		.hd_align_reg = AFE_MEMIF_PBUF_SIZE,
		.hd_align_mshift = 21,
		.agent_disable_reg = -1,
	},
	[MT8516_AFE_MEMIF_TDM_IN] = {
		.name = "TDM_IN",
		.id = MT8516_AFE_MEMIF_TDM_IN,
		.reg_ofs_base = AFE_HDMI_IN_2CH_BASE,
		.reg_ofs_cur = AFE_HDMI_IN_2CH_CUR,
		.reg_ofs_end = AFE_HDMI_IN_2CH_END,
		.fs_reg = -1,
		.fs_shift = -1,
		.mono_reg = -1,
		.mono_shift = -1,
		.enable_reg = AFE_HDMI_IN_2CH_CON0,
		.enable_shift = 0,
		.hd_reg = AFE_MEMIF_PBUF2_SIZE,
		.hd_shift = 4,
		.hd_align_reg = AFE_MEMIF_PBUF2_SIZE,
		.hd_align_mshift = 5,
		.agent_disable_reg = -1,
	},
};

static const struct mtk_base_irq_data mt8516_afe_irq_data[MT8516_AFE_IRQ_NUM] = {
	[MT8516_AFE_IRQ1] = {
		.id = MT8516_AFE_IRQ1,
		.irq_cnt_reg = AFE_IRQ_CNT1,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = 0x3ffff,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = 4,
		.irq_fs_maskbit = 0xf,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = 0,
		.irq_clr_reg = AFE_IRQ_CLR,
		.irq_clr_shift = 0,
	},
	[MT8516_AFE_IRQ7] = {
		.id = MT8516_AFE_IRQ7,
		.irq_cnt_reg = AFE_IRQ_CNT7,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = 0x3ffff,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = 24,
		.irq_fs_maskbit = 0xf,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = 14,
		.irq_clr_reg = AFE_IRQ_CLR,
		.irq_clr_shift = 6,
	},
	[MT8516_AFE_IRQ10] = {
		/* counts frames of the TDM input, there is no rate to set */
		.id = MT8516_AFE_IRQ10,
		.irq_cnt_reg = AFE_IRQ_CNT10,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = 0x3ffff,
		.irq_fs_reg = -1,
		.irq_en_reg = AFE_IRQ_MCU_CON2,
		.irq_en_shift = 4,
		.irq_clr_reg = AFE_IRQ_CLR,
		.irq_clr_shift = 9,
	},
};

static const int mt8516_afe_memif_irqs[MT8516_AFE_MEMIF_NUM] = {
	[MT8516_AFE_MEMIF_DL1] = MT8516_AFE_IRQ1,
	[MT8516_AFE_MEMIF_AWB] = MT8516_AFE_IRQ7,
	[MT8516_AFE_MEMIF_TDM_IN] = MT8516_AFE_IRQ10,
};

static const struct regmap_config mt8516_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AFE_MAX_REGISTER,
};

static irqreturn_t mt8516_afe_irq_handler(int irq, void *dev_id)
{
	struct mtk_base_afe *afe = dev_id;
	unsigned int status;
	int i, ret;

	ret = regmap_read(afe->regmap, AFE_IRQ_STATUS, &status);
	if (ret) {
		dev_err_ratelimited(afe->dev, "failed to read irq status\n");
		status = AFE_IRQ_STATUS_BITS;
		goto out;
	}

	for (i = 0; i < MT8516_AFE_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];
		const struct mtk_base_irq_data *irq_data;

		irq_data = afe->irqs[memif->irq_usage].irq_data;
		if (!memif->substream ||
		    !(status & BIT(irq_data->irq_clr_shift)))
			continue;

		snd_pcm_period_elapsed(memif->substream);
	}

out:
	regmap_write(afe->regmap, AFE_IRQ_CLR, status & AFE_IRQ_STATUS_BITS);

	return IRQ_HANDLED;
}

static void mt8516_afe_init_registers(struct mtk_base_afe *afe)
{
	/* Let all AFE interrupts through to the CPU. */
	regmap_set_bits(afe->regmap, AFE_IRQ_MCU_EN, AFE_IRQ_MCU_EN_ALL);

	/*
	 * Default routing: DL1 to the I2S output (O03/O04) and to the AWB
	 * loopback (O05/O06). The mixer switches reflect these bits.
	 */
	regmap_set_bits(afe->regmap, AFE_CONN1, BIT(AFE_CONN1_I05_O03_S));
	regmap_set_bits(afe->regmap, AFE_CONN2,
			BIT(AFE_CONN2_I06_O04_S) | BIT(AFE_CONN2_I05_O05_S) |
			BIT(AFE_CONN2_I06_O06_S));
}

static int mt8516_afe_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < afe->reg_back_up_list_num; i++)
		regmap_read(afe->regmap, afe->reg_back_up_list[i],
			    &afe->reg_back_up[i]);

	return 0;
}

static int mt8516_afe_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	int i;

	regmap_set_bits(afe->regmap, AFE_IRQ_MCU_EN, AFE_IRQ_MCU_EN_ALL);

	for (i = 0; i < afe->reg_back_up_list_num; i++)
		regmap_write(afe->regmap, afe->reg_back_up_list[i],
			     afe->reg_back_up[i]);

	return 0;
}

static void mt8516_afe_disable_afe_cg(void *data)
{
	mt8516_afe_disable_top_cg(data, MT8516_AFE_CG_AFE);
}

static int mt8516_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt8516_afe_private *afe_priv;
	struct mtk_base_afe_dai *dai;
	struct mtk_base_afe *afe;
	int i, irq, ret;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	afe_priv = devm_kzalloc(dev, sizeof(*afe_priv), GFP_KERNEL);
	if (!afe_priv)
		return -ENOMEM;

	afe->dev = dev;
	afe->platform_priv = afe_priv;
	afe_priv->i2s.apll = MT8516_AFE_APLL_NONE;
	afe_priv->tdm_in.apll = MT8516_AFE_APLL_NONE;
	platform_set_drvdata(pdev, afe);

	ret = devm_mutex_init(dev, &afe_priv->clk_lock);
	if (ret)
		return ret;

	ret = devm_mutex_init(dev, &afe->irq_alloc_lock);
	if (ret)
		return ret;

	/* The memory interfaces take 32 bit buffer addresses. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	afe->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	ret = mt8516_afe_init_audio_clk(afe);
	if (ret)
		return ret;

	afe->regmap = devm_regmap_init_mmio_clk(dev, "top_pdn_audio",
						afe->base_addr,
						&mt8516_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	afe->memif_size = MT8516_AFE_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif)
		return -ENOMEM;

	afe->irqs_size = MT8516_AFE_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs)
		return -ENOMEM;

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &mt8516_afe_irq_data[i];

	/* Every memory interface has its interrupt hardwired. */
	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &mt8516_afe_memif_data[i];
		afe->memif[i].irq_usage = mt8516_afe_memif_irqs[i];
		afe->memif[i].const_irq = 1;
		afe->irqs[mt8516_afe_memif_irqs[i]].irq_occupyed = true;
	}

	afe->mtk_afe_hardware = &mt8516_afe_hardware;
	afe->memif_fs = mt8516_afe_memif_fs;
	afe->irq_fs = mt8516_afe_memif_fs;

	afe->reg_back_up_list = mt8516_afe_backup_list;
	afe->reg_back_up_list_num = ARRAY_SIZE(mt8516_afe_backup_list);
	afe->reg_back_up = devm_kcalloc(dev, afe->reg_back_up_list_num,
					sizeof(*afe->reg_back_up), GFP_KERNEL);
	if (!afe->reg_back_up)
		return -ENOMEM;

	INIT_LIST_HEAD(&afe->sub_dais);

	dai = devm_kzalloc(dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	dai->dai_drivers = mt8516_afe_dai_drivers;
	dai->num_dai_drivers = ARRAY_SIZE(mt8516_afe_dai_drivers);
	dai->dapm_widgets = mt8516_afe_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt8516_afe_widgets);
	dai->dapm_routes = mt8516_afe_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt8516_afe_routes);
	list_add(&dai->list, &afe->sub_dais);

	ret = mtk_afe_combine_sub_dai(afe);
	if (ret)
		return ret;

	/* The AFE clock gate has to stay open for mixer register access. */
	ret = mt8516_afe_enable_top_cg(afe, MT8516_AFE_CG_AFE);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, mt8516_afe_disable_afe_cg, afe);
	if (ret)
		return ret;

	mt8516_afe_init_registers(afe);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, mt8516_afe_irq_handler, 0,
			       dev_name(dev), afe);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	return devm_snd_soc_register_component(dev, &mtk_afe_pcm_platform,
					       afe->dai_drivers,
					       afe->num_dai_drivers);
}

static const struct of_device_id mt8516_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt8516-afe-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt8516_afe_pcm_dt_match);

static DEFINE_SIMPLE_DEV_PM_OPS(mt8516_afe_pm_ops, mt8516_afe_suspend,
				mt8516_afe_resume);

static struct platform_driver mt8516_afe_pcm_driver = {
	.driver = {
		.name = "mt8516-afe-pcm",
		.of_match_table = mt8516_afe_pcm_dt_match,
		.pm = pm_sleep_ptr(&mt8516_afe_pm_ops),
	},
	.probe = mt8516_afe_pcm_dev_probe,
};
module_platform_driver(mt8516_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek MT8516 ALSA SoC AFE platform driver");
MODULE_LICENSE("GPL");
