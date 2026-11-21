// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8516 audio front end clock control
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Derived from the MediaTek/Amazon mt8167 driver in Amazon's GPL release.
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/regmap.h>

#include "mt8516-afe-clk.h"
#include "mt8516-afe-common.h"
#include "mt8516-reg.h"
#include "../common/mtk-base-afe.h"

static const char * const mt8516_afe_clk_names[MT8516_CLK_NUM] = {
	[MT8516_CLK_TOP_PDN_AUD] = "top_pdn_audio",
	[MT8516_CLK_AUD1] = "aud1",
	[MT8516_CLK_AUD2] = "aud2",
	[MT8516_CLK_ENGEN1] = "engen1",
	[MT8516_CLK_ENGEN2] = "engen2",
	[MT8516_CLK_I2S0_M_SEL] = "i2s0_m_sel",
	[MT8516_CLK_I2S1_M_SEL] = "i2s1_m_sel",
	[MT8516_CLK_I2S2_M_SEL] = "i2s2_m_sel",
	[MT8516_CLK_I2S3_M_SEL] = "i2s3_m_sel",
	[MT8516_CLK_I2S4_M_SEL] = "i2s4_m_sel",
	[MT8516_CLK_I2S5_M_SEL] = "i2s5_m_sel",
	[MT8516_CLK_APLL12_DIV0] = "apll12_div0",
	[MT8516_CLK_APLL12_DIV1] = "apll12_div1",
	[MT8516_CLK_APLL12_DIV2] = "apll12_div2",
	[MT8516_CLK_APLL12_DIV3] = "apll12_div3",
	[MT8516_CLK_APLL12_DIV4] = "apll12_div4",
	[MT8516_CLK_APLL12_DIV4B] = "apll12_div4b",
	[MT8516_CLK_APLL12_DIV5] = "apll12_div5",
	[MT8516_CLK_APLL12_DIV5B] = "apll12_div5b",
};

static const unsigned int mt8516_afe_cg_mask[MT8516_AFE_CG_NUM] = {
	[MT8516_AFE_CG_AFE] = AUD_TCON0_PDN_AFE,
	[MT8516_AFE_CG_22M] = AUD_TCON0_PDN_22M,
	[MT8516_AFE_CG_24M] = AUD_TCON0_PDN_24M,
	[MT8516_AFE_CG_APLL_TUNER] = AUD_TCON0_PDN_APLL_TUNER,
	[MT8516_AFE_CG_APLL2_TUNER] = AUD_TCON0_PDN_APLL2_TUNER,
};

int mt8516_afe_init_audio_clk(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	int i;

	for (i = 0; i < MT8516_CLK_NUM; i++) {
		afe_priv->clocks[i] = devm_clk_get(afe->dev,
						   mt8516_afe_clk_names[i]);
		if (IS_ERR(afe_priv->clocks[i]))
			return dev_err_probe(afe->dev,
					     PTR_ERR(afe_priv->clocks[i]),
					     "failed to get clock %s\n",
					     mt8516_afe_clk_names[i]);
	}

	return 0;
}

int mt8516_afe_enable_top_cg(struct mtk_base_afe *afe, unsigned int cg)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	guard(mutex)(&afe_priv->clk_lock);

	if (afe_priv->top_cg_ref_cnt[cg]++)
		return 0;

	return regmap_clear_bits(afe->regmap, AUDIO_TOP_CON0,
				 mt8516_afe_cg_mask[cg]);
}

void mt8516_afe_disable_top_cg(struct mtk_base_afe *afe, unsigned int cg)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	guard(mutex)(&afe_priv->clk_lock);

	if (--afe_priv->top_cg_ref_cnt[cg])
		return;

	regmap_set_bits(afe->regmap, AUDIO_TOP_CON0, mt8516_afe_cg_mask[cg]);
}

int mt8516_afe_enable_main_clk(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = clk_prepare_enable(afe_priv->clocks[MT8516_CLK_TOP_PDN_AUD]);
	if (ret)
		return ret;

	ret = mt8516_afe_enable_top_cg(afe, MT8516_AFE_CG_AFE);
	if (ret)
		clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_TOP_PDN_AUD]);

	return ret;
}

void mt8516_afe_disable_main_clk(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	mt8516_afe_disable_top_cg(afe, MT8516_AFE_CG_AFE);
	clk_disable_unprepare(afe_priv->clocks[MT8516_CLK_TOP_PDN_AUD]);
}

void mt8516_afe_enable_afe_on(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	guard(mutex)(&afe_priv->clk_lock);

	if (afe_priv->afe_on_ref_cnt++)
		return;

	regmap_set_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_AFE_ON);
}

void mt8516_afe_disable_afe_on(struct mtk_base_afe *afe)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	guard(mutex)(&afe_priv->clk_lock);

	if (--afe_priv->afe_on_ref_cnt)
		return;

	regmap_clear_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_AFE_ON);
}

static void mt8516_afe_enable_apll_tuner(struct mtk_base_afe *afe, int apll)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	unsigned int reg, val;

	guard(mutex)(&afe_priv->clk_lock);

	if (afe_priv->apll_ref_cnt[apll]++)
		return;

	if (apll == MT8516_AFE_APLL1) {
		reg = AFE_APLL1_TUNER_CFG;
		val = AFE_APLL1_TUNER_CFG_VAL;
	} else {
		reg = AFE_APLL2_TUNER_CFG;
		val = AFE_APLL2_TUNER_CFG_VAL;
	}

	regmap_update_bits(afe->regmap, reg, AFE_APLL_TUNER_CFG_MASK, val);
	regmap_set_bits(afe->regmap, reg, AFE_APLL_TUNER_CFG_EN);
}

static void mt8516_afe_disable_apll_tuner(struct mtk_base_afe *afe, int apll)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;

	guard(mutex)(&afe_priv->clk_lock);

	if (--afe_priv->apll_ref_cnt[apll])
		return;

	regmap_clear_bits(afe->regmap,
			  apll == MT8516_AFE_APLL1 ? AFE_APLL1_TUNER_CFG :
						     AFE_APLL2_TUNER_CFG,
			  AFE_APLL_TUNER_CFG_EN);
}

/*
 * Everything an interface clocked from one of the audio PLLs needs besides
 * its own MCLK/BCK dividers: the engine clock, its gates and the tuner that
 * keeps the AFE in sync with the PLL.
 */
int mt8516_afe_enable_apll(struct mtk_base_afe *afe, int apll)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	bool apll1 = apll == MT8516_AFE_APLL1;
	int ret;

	ret = clk_prepare_enable(afe_priv->clocks[apll1 ? MT8516_CLK_ENGEN1 :
							  MT8516_CLK_ENGEN2]);
	if (ret)
		return ret;

	mt8516_afe_enable_top_cg(afe, apll1 ? MT8516_AFE_CG_22M :
					      MT8516_AFE_CG_24M);
	mt8516_afe_enable_top_cg(afe, apll1 ? MT8516_AFE_CG_APLL_TUNER :
					      MT8516_AFE_CG_APLL2_TUNER);
	mt8516_afe_enable_apll_tuner(afe, apll);

	return 0;
}

void mt8516_afe_disable_apll(struct mtk_base_afe *afe, int apll)
{
	struct mt8516_afe_private *afe_priv = afe->platform_priv;
	bool apll1 = apll == MT8516_AFE_APLL1;

	mt8516_afe_disable_apll_tuner(afe, apll);
	mt8516_afe_disable_top_cg(afe, apll1 ? MT8516_AFE_CG_APLL_TUNER :
					       MT8516_AFE_CG_APLL2_TUNER);
	mt8516_afe_disable_top_cg(afe, apll1 ? MT8516_AFE_CG_22M :
					       MT8516_AFE_CG_24M);
	clk_disable_unprepare(afe_priv->clocks[apll1 ? MT8516_CLK_ENGEN1 :
						       MT8516_CLK_ENGEN2]);
}
