/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT8516 audio front end clock control
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Derived from the MediaTek/Amazon mt8167 driver in Amazon's GPL release.
 */

#ifndef _MT8516_AFE_CLK_H_
#define _MT8516_AFE_CLK_H_

struct mtk_base_afe;

int mt8516_afe_init_audio_clk(struct mtk_base_afe *afe);
int mt8516_afe_enable_top_cg(struct mtk_base_afe *afe, unsigned int cg);
void mt8516_afe_disable_top_cg(struct mtk_base_afe *afe, unsigned int cg);
int mt8516_afe_enable_main_clk(struct mtk_base_afe *afe);
void mt8516_afe_disable_main_clk(struct mtk_base_afe *afe);
void mt8516_afe_enable_afe_on(struct mtk_base_afe *afe);
void mt8516_afe_disable_afe_on(struct mtk_base_afe *afe);
int mt8516_afe_enable_apll(struct mtk_base_afe *afe, int apll);
void mt8516_afe_disable_apll(struct mtk_base_afe *afe, int apll);

#endif
