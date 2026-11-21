/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT8516 audio front end common definitions
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Derived from the MediaTek/Amazon mt8167 driver in Amazon's GPL release.
 */

#ifndef _MT8516_AFE_COMMON_H_
#define _MT8516_AFE_COMMON_H_

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/types.h>

enum {
	MT8516_AFE_MEMIF_DL1,
	MT8516_AFE_MEMIF_AWB,
	MT8516_AFE_MEMIF_TDM_IN,
	MT8516_AFE_MEMIF_NUM,
	MT8516_AFE_IO_I2S = MT8516_AFE_MEMIF_NUM,
	MT8516_AFE_IO_TDM_IN,
	MT8516_AFE_IO_DL_BE,
};

enum {
	MT8516_AFE_IRQ1,
	MT8516_AFE_IRQ7,
	MT8516_AFE_IRQ10,
	MT8516_AFE_IRQ_NUM,
};

enum {
	MT8516_CLK_TOP_PDN_AUD,
	MT8516_CLK_AUD1,
	MT8516_CLK_AUD2,
	MT8516_CLK_ENGEN1,
	MT8516_CLK_ENGEN2,
	MT8516_CLK_I2S0_M_SEL,
	MT8516_CLK_I2S1_M_SEL,
	MT8516_CLK_I2S2_M_SEL,
	MT8516_CLK_I2S3_M_SEL,
	MT8516_CLK_I2S4_M_SEL,
	MT8516_CLK_I2S5_M_SEL,
	MT8516_CLK_APLL12_DIV0,
	MT8516_CLK_APLL12_DIV1,
	MT8516_CLK_APLL12_DIV2,
	MT8516_CLK_APLL12_DIV3,
	MT8516_CLK_APLL12_DIV4,
	MT8516_CLK_APLL12_DIV4B,
	MT8516_CLK_APLL12_DIV5,
	MT8516_CLK_APLL12_DIV5B,
	MT8516_CLK_NUM,
};

enum {
	MT8516_AFE_CG_AFE,
	MT8516_AFE_CG_22M,
	MT8516_AFE_CG_24M,
	MT8516_AFE_CG_APLL_TUNER,
	MT8516_AFE_CG_APLL2_TUNER,
	MT8516_AFE_CG_NUM,
};

enum {
	MT8516_AFE_APLL1,
	MT8516_AFE_APLL2,
	MT8516_AFE_APLL_NUM,
	MT8516_AFE_APLL_NONE = -1,
};

struct mt8516_afe_i2s_data {
	unsigned int mclk_rate;
	unsigned int rate;
	unsigned int width;
	unsigned int active_rate;
	unsigned int active_width;
	int apll;
};

struct mt8516_afe_tdm_in_data {
	unsigned int fmt;
	unsigned int rate;
	unsigned int channels;
	unsigned int width;
	int apll;
};

struct mt8516_afe_private {
	struct clk *clocks[MT8516_CLK_NUM];
	/* protects the reference counters below */
	struct mutex clk_lock;
	int afe_on_ref_cnt;
	int top_cg_ref_cnt[MT8516_AFE_CG_NUM];
	int apll_ref_cnt[MT8516_AFE_APLL_NUM];
	struct mt8516_afe_i2s_data i2s;
	struct mt8516_afe_tdm_in_data tdm_in;
};

#endif
