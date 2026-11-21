/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT8516 integrated ADDA (INT ADDA) back end DAI
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Port of the "INT ADDA" DAI of the MediaTek/Amazon mt8167 driver in
 * Amazon's 4.4 GPL release (dot3-6.5.7.0, sound/soc/mediatek/mt8167/
 * mt8167-afe-pcm.c) into the lidar MT8516 AFE framework, as its own
 * sub DAI like upstream does for the mt8183 ADDA.  The register
 * sequences are the vendor ones; the downlink path resamples the DL1
 * memory interface data through the DL SRC2 to the analog part of the
 * integrated codec, the "i2s out" register block carries the serial
 * link to it (vendor names, kept).
 */

#ifndef _MT8516_AFE_ADDA_H_
#define _MT8516_AFE_ADDA_H_

struct mtk_base_afe;

int mt8516_afe_adda_register(struct mtk_base_afe *afe);

#endif
