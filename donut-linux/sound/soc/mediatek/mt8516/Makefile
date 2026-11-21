# SPDX-License-Identifier: GPL-2.0

# platform driver
snd-soc-mt8516-afe-y := \
	mt8516-afe-clk.o \
	mt8516-afe-pcm.o \
	mt8516-afe-adda.o

obj-$(CONFIG_SND_SOC_MT8516) += snd-soc-mt8516-afe.o

# machine driver
obj-$(CONFIG_SND_SOC_MT8516_AMAZON_LIDAR) += mt8516-amazon-lidar.o
# donut: the module name (snd-soc-mt8516-amazon-donut) is part of the
# donut image contract; keep the composite target when touching this.
snd-soc-mt8516-amazon-donut-y := mt8516-amazon-donut.o
obj-$(CONFIG_SND_SOC_MT8516_AMAZON_DONUT) += snd-soc-mt8516-amazon-donut.o
