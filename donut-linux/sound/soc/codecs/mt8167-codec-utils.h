// SPDX-License-Identifier: GPL-2.0
/*
 * mt8167-codec-utils.h  --  MT8167 codec driver utility functions
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Ported from Amazon's 4.4 GPL release (dot3-6.5.7.0) to the v7.3
 * snd_soc_component API.
 */

#ifndef __MT8167_CODEC_UTILS_H__
#define __MT8167_CODEC_UTILS_H__

#include <linux/types.h>
#include <sound/soc.h>
#include "mt8167-codec.h"

/*
 * HP on sequence as below
 *  A -> B -> C -> D
 *
 *  A: Depop VCM on
 *  B: DAC on
 *  C: OP Amp on
 *  D: Ready to playback (same voltage as A)
 *
 * sequence divided into 3 steps
 *     A -> B
 *     B -> C
 *     C -> D
 *
 * HP_CALI_VCM_TO_DAC: the dc offset between A and B
 * HP_CALI_DAC_TO_OP : the dc offset between B and C
 * HP_CALI_OP_TO_VCM : the dc offset between A and C
 *
 * the dc offset used for ramping between two stages
 *
 * reverse sequence for HP off
 *  D -> C -> B -> A
 */
enum hp_cali_step_id {
	HP_CALI_VCM_TO_DAC = 0,
	HP_CALI_DAC_TO_OP,
	HP_CALI_OP_TO_VCM,
	HP_CALI_NUMS,
};

int mt8167_codec_valid_new_dc_comp(struct snd_soc_component *component);

void mt8167_codec_update_dc_comp(struct snd_soc_component *component,
	enum hp_channel_id cid,
	u32 lch_dccomp_val, u32 rch_dccomp_val);

void mt8167_codec_dc_offset_ramp(struct snd_soc_component *component,
	enum hp_cali_step_id sid, enum hp_channel_id cid,
	enum hp_sequence_id seq_id,
	u32 lch_dccomp[HP_CALI_NUMS][HP_PGA_GAIN_NUMS],
	u32 rch_dccomp[HP_CALI_NUMS][HP_PGA_GAIN_NUMS],
	u32 lpga_index, u32 rpga_index);

void mt8167_codec_show_voltage_of_hp(struct snd_soc_component *component);

void mt8167_codec_gather_comp_val(struct snd_soc_component *component,
	u32 dccomp_val[HP_CALI_NUMS][HP_PGA_GAIN_NUMS],
	long dc_offset[HP_CALI_NUMS][HP_PGA_GAIN_NUMS]);

u64 mt8167_codec_get_hp_cali_comp_val(struct snd_soc_component *component,
	u32 dccomp_val[HP_CALI_NUMS][HP_PGA_GAIN_NUMS],
	long dc_offset[HP_CALI_NUMS][HP_PGA_GAIN_NUMS],
	enum hp_channel_id cid);

void mt8167_codec_turn_off_lpbk_path(struct snd_soc_component *component,
	u32 lpbk_type);

void mt8167_codec_turn_on_lpbk_path(struct snd_soc_component *component,
	u32 lpbk_type);

#endif
