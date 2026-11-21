// SPDX-License-Identifier: GPL-2.0
/*
 * extcon driver for the headset jack of MediaTek MT8167/MT8516 boards
 * (Amazon variant)
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (c) 2018 Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * Port of the Amazon ACCDET driver of the 4.4 GPL release
 * (dot3-6.5.7.0, drivers/misc/mediatek/accdet/mt8167/accdet_amzn.c) to
 * the mainline extcon framework.  The detection hardware is a plain
 * GPIO (EINT0 on the donut): the interrupt is level triggered and its
 * polarity is flipped on every event, the debounce is re-programmed
 * per direction (plug-in vs plug-out values from the device tree),
 * exactly as the vendor driver does it.
 *
 * Substitutions against the vendor driver, deliberate:
 *  - the Android "h2w" switch class reports through extcon
 *    (EXTCON_JACK_HEADPHONE); a plain GPIO cannot distinguish a
 *    headset with microphone, so only the headphone cable is reported
 *  - the /dev/accdet char device, the never-reporting input device and
 *    the accdet_plugged sysfs attribute of the vendor driver are
 *    dropped; the jack state is readable through the extcon sysfs
 *    (state file of the extcon device)
 *  - the "invert" device tree property became the GPIO polarity flag
 *    of the accdet-gpios property (same effect, mainline mechanics)
 *  - the external amplifier fast-mute hook
 *    (sound/external_amp_mute.h) is parsed but stubbed; the donut
 *    device tree does not request it.  TODO(M0)
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/extcon-provider.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#define ACCDET_DEBUG(format, args...) pr_debug(format, ##args)
#define ACCDET_INFO(format, args...) pr_warn(format, ##args)
#define ACCDET_ERROR(format, args...) pr_err(format, ##args)

/* Status of the jack GPIO */
enum accdet_status {
	PLUG_OUT = 0,
	HEADSET = 1,
};

/* Used to let accdet know if the pin has been fully plugged-in */
#define EINT_PIN_PLUG_IN	1
#define EINT_PIN_PLUG_OUT	0

static const unsigned int mt8167_accdet_cables[] = {
	EXTCON_JACK_HEADPHONE,
	EXTCON_NONE,
};

struct mt8167_accdet_data {
	struct device *dev;
	struct gpio_desc *gpiod;
	struct gpio_desc *gpiod_optical;
	struct extcon_dev *edev;
	int irq;
	unsigned int eint_type;
	u32 eint_debounce;
	u32 accdet_plugout_debounce;
	bool mute_amplifier;
	unsigned int cur_eint_state;
	int jack_status;
	bool opticalin_cable;
	bool initialized;
	/* protects the jack state against concurrent work and resume */
	struct mutex sync_lock;
	struct workqueue_struct *wq;
	struct work_struct work;
};

static const char * const accdet_status_string[] = {
	"Plug_out",
	"Plug_in"
};

/*
 * Checks status of the connector.  Returns true when the previous
 * state was inserted (an amplifier fast-mute would be needed there).
 */
static bool mt8167_accdet_check_status(struct mt8167_accdet_data *accdet)
{
	struct gpio_desc *gpiod = accdet->gpiod;
	int current_status, optical_status = 0;
	bool pull_out = false;

	ACCDET_DEBUG("[accdet]%s+ Plugged Before=%d\n", __func__,
		     accdet->jack_status);

	current_status = gpiod_get_value_cansleep(gpiod);

	/* Check optical status */
	if (accdet->gpiod_optical)
		optical_status = gpiod_get_value_cansleep(accdet->gpiod_optical);

	scoped_guard(mutex, &accdet->sync_lock) {
		if (current_status != accdet->jack_status) {
			ACCDET_INFO("[accdet]%s- jack status switch:[%s]->[%s]\n",
				__func__,
				accdet_status_string[accdet->jack_status],
				accdet_status_string[current_status]);

			/* Check for Optical Cable */
			if (current_status == HEADSET && optical_status == 1)
				accdet->opticalin_cable = true;
			/* Set pull_out if previous state was insert */
			else if (accdet->jack_status == HEADSET) {
				/*
				 * Mute Amp fast here to avoid pop.
				 * The vendor hook into the audio
				 * subsystem is not ported (TODO(M0));
				 * the donut device tree does not
				 * request it.
				 */
				if (accdet->mute_amplifier &&
				    accdet->opticalin_cable == false)
					dev_warn_once(accdet->dev,
						"amplifier fast-mute on pull-out not ported, TODO(M0)\n");

				pull_out = true;
				accdet->opticalin_cable = false;
			}

			accdet->jack_status = current_status;
		}
	}

	ACCDET_INFO("[accdet]%s- Plugged After=%d opticalin_cable %d pull_out %d\n",
		__func__, accdet->jack_status, accdet->opticalin_cable,
		pull_out);

	return pull_out;
}

/*
 * Reports the status of the connector through extcon; optical cable
 * insertions are not reported (the vendor uevents of the optical
 * input reported them separately).
 */
static void mt8167_accdet_report_status(struct mt8167_accdet_data *accdet)
{
	int optical_status = 0;
	bool pulled_out;

	ACCDET_DEBUG("[accdet]%s+\n", __func__);

	pulled_out = mt8167_accdet_check_status(accdet);

	/* Optical status is reported by uevents of driver. Don't report here */
	if (accdet->gpiod_optical) {
		optical_status =
			gpiod_get_value_cansleep(accdet->gpiod_optical);

		/*
		 * Report status if analog cable was removed,
		 * or analog cable is inserted.
		 */
		if ((pulled_out == true && accdet->opticalin_cable == false) ||
			(pulled_out == false && optical_status == 0)) {
			extcon_set_state_sync(accdet->edev,
				EXTCON_JACK_HEADPHONE, accdet->jack_status);
		}
	} else {
		/* Report status always if optical is not configured */
		extcon_set_state_sync(accdet->edev, EXTCON_JACK_HEADPHONE,
				      accdet->jack_status);
	}

	ACCDET_DEBUG("[accdet]%s-\n", __func__);
}

/* Bottom half of the irq: checks and reports, then re-enables the irq */
static void mt8167_accdet_work_callback(struct work_struct *work)
{
	struct mt8167_accdet_data *accdet =
		container_of(work, struct mt8167_accdet_data, work);

	ACCDET_DEBUG("[accdet]%s+\n", __func__);

	mt8167_accdet_report_status(accdet);
	enable_irq(accdet->irq);

	ACCDET_DEBUG("[accdet]%s-: enable_irq  !!!!!!\n", __func__);
}

/*
 * Callback from the gpio irq.  Flips the trigger level to catch the
 * opposite edge of the mechanical insertion, re-programs the hardware
 * debounce for the coming direction and queues the status work.
 */
static irqreturn_t mt8167_accdet_irq_callback(int irq, void *data)
{
	struct mt8167_accdet_data *accdet = data;

	if (!accdet->initialized) {
		ACCDET_ERROR("[accdet]%s Cannot proceed! init=%d\n",
			__func__, accdet->initialized);
		return IRQ_HANDLED;
	}

	if (accdet->cur_eint_state == EINT_PIN_PLUG_IN) {
		/*
		 *  To trigger EINT when the headset was plugged in
		 *  We set the polarity back as we initialed.
		 */
		if (accdet->eint_type == IRQ_TYPE_LEVEL_HIGH)
			irq_set_irq_type(accdet->irq, IRQ_TYPE_LEVEL_HIGH);
		else
			irq_set_irq_type(accdet->irq, IRQ_TYPE_LEVEL_LOW);
		gpiod_set_debounce(accdet->gpiod, accdet->eint_debounce);
		/* update the eint status */
		accdet->cur_eint_state = EINT_PIN_PLUG_OUT;
	} else {
		/*
		 *  To trigger EINT when the headset was plugged out
		 *  We set the opposite polarity to what we initialed.
		 */
		if (accdet->eint_type == IRQ_TYPE_LEVEL_HIGH)
			irq_set_irq_type(accdet->irq, IRQ_TYPE_LEVEL_LOW);
		else
			irq_set_irq_type(accdet->irq, IRQ_TYPE_LEVEL_HIGH);

		gpiod_set_debounce(accdet->gpiod,
			accdet->accdet_plugout_debounce);
		/* update the eint status */
		accdet->cur_eint_state = EINT_PIN_PLUG_IN;
	}
	disable_irq_nosync(accdet->irq);
	ACCDET_DEBUG("[accdet]accdet_eint_callback after cur_eint_state=%d\n",
		accdet->cur_eint_state);

	queue_work(accdet->wq, &accdet->work);

	return IRQ_HANDLED;
}

static int mt8167_accdet_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt8167_accdet_data *accdet;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_eint_int;
	int ret;

	ACCDET_INFO("[accdet]%s+\n", __func__);

	accdet = devm_kzalloc(dev, sizeof(*accdet), GFP_KERNEL);
	if (!accdet)
		return -ENOMEM;

	accdet->dev = dev;
	accdet->jack_status = PLUG_OUT;
	accdet->cur_eint_state = EINT_PIN_PLUG_OUT;

	ret = devm_mutex_init(dev, &accdet->sync_lock);
	if (ret)
		return ret;

	accdet->edev = devm_extcon_dev_allocate(dev, mt8167_accdet_cables);
	if (IS_ERR(accdet->edev))
		return PTR_ERR(accdet->edev);

	ret = devm_extcon_dev_register(dev, accdet->edev);
	if (ret) {
		ACCDET_ERROR("[accdet]%s extcon register returned:%d!\n",
			__func__, ret);
		return ret;
	}

	/* configure the jack pin as GPIO function, external interrupt */
	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(dev, PTR_ERR(pinctrl),
				     "failed to get pinctrl\n");

	pins_eint_int = pinctrl_lookup_state(pinctrl, "state_eint_as_int");
	if (IS_ERR(pins_eint_int))
		return dev_err_probe(dev, PTR_ERR(pins_eint_int),
				     "missing pinctrl state state_eint_as_int\n");

	ret = pinctrl_select_state(pinctrl, pins_eint_int);
	if (ret)
		return dev_err_probe(dev, ret, "failed to select EINT state\n");

	/*
	 * The donut jack pin reads low when a plug is inserted; the
	 * vendor expressed this with "invert = <1>", here it is the
	 * polarity flag of the gpio.
	 */
	accdet->gpiod = devm_gpiod_get(dev, "accdet", GPIOD_IN);
	if (IS_ERR(accdet->gpiod))
		return dev_err_probe(dev, PTR_ERR(accdet->gpiod),
				     "failed to get accdet gpio\n");

	accdet->gpiod_optical = devm_gpiod_get_optional(dev, "optical",
							GPIOD_IN);
	if (IS_ERR(accdet->gpiod_optical))
		return dev_err_probe(dev, PTR_ERR(accdet->gpiod_optical),
				     "failed to get optical gpio\n");

	/* vendor debounce/threshold values, read from the device tree */
	device_property_read_u32(dev, "eint-debounce",
				 &accdet->eint_debounce);
	device_property_read_u32(dev, "accdet-plugout-debounce",
				 &accdet->accdet_plugout_debounce);
	accdet->mute_amplifier =
		device_property_read_bool(dev, "mute-amplifier");
	if (accdet->mute_amplifier)
		ACCDET_INFO("[accdet]%s amplifier fast-mute requested but not ported, TODO(M0)\n",
			    __func__);

	gpiod_set_debounce(accdet->gpiod, accdet->eint_debounce);

	/* the level type of the device tree interrupt starts the toggling */
	accdet->irq = platform_get_irq(pdev, 0);
	if (accdet->irq < 0)
		return accdet->irq;

	accdet->eint_type = irq_get_trigger_type(accdet->irq);
	if (!(accdet->eint_type & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)))
		return dev_err_probe(dev, -EINVAL,
				     "interrupt must be level triggered\n");

	platform_set_drvdata(pdev, accdet);

	accdet->wq = create_singlethread_workqueue("accdet_eint");
	if (!accdet->wq)
		return -ENOMEM;

	INIT_WORK(&accdet->work, mt8167_accdet_work_callback);

	accdet->initialized = true;

	ret = devm_request_irq(dev, accdet->irq, mt8167_accdet_irq_callback,
			       IRQF_TRIGGER_NONE, "accdet-eint", accdet);
	if (ret) {
		ACCDET_ERROR("[accdet]%s EINT IRQ LINE NOT AVAILABLE\n",
			     __func__);
		goto clean_wq;
	}

	ACCDET_INFO("[accdet]%s- accdet_irq=%d, deb=%d optical=%d amp=%d\n",
			__func__, accdet->irq, accdet->eint_debounce,
			accdet->gpiod_optical ? 1 : 0, accdet->mute_amplifier);

	/* report the state the connector is already in */
	mt8167_accdet_report_status(accdet);

	device_init_wakeup(dev, true);

	return 0;

clean_wq:
	accdet->initialized = false;
	destroy_workqueue(accdet->wq);
	return ret;
}

static void mt8167_accdet_remove(struct platform_device *pdev)
{
	struct mt8167_accdet_data *accdet = platform_get_drvdata(pdev);

	ACCDET_DEBUG("[accdet]%s+\n", __func__);

	accdet->initialized = false;
	cancel_work_sync(&accdet->work);
	destroy_workqueue(accdet->wq);
}

/*
 * Read and update the state after resume (vendor
 * mt_accdet_pm_restore_noirq): the connector may have changed while
 * suspended.
 */
static int mt8167_accdet_resume(struct device *dev)
{
	struct mt8167_accdet_data *accdet = dev_get_drvdata(dev);
	bool temp_opticalin_cable = false;
	int current_status_restore;

	ACCDET_DEBUG("[accdet]%s+\n", __func__);

	current_status_restore = gpiod_get_value_cansleep(accdet->gpiod);

	if (current_status_restore) {
		accdet->jack_status = HEADSET;

		/* Check for optical cable */
		if (accdet->gpiod_optical &&
		    gpiod_get_value_cansleep(accdet->gpiod_optical) == 1)
			temp_opticalin_cable = true;
	} else {
		accdet->jack_status = PLUG_OUT;
	}

	/* Report status except when optical cable is detected */
	if (temp_opticalin_cable == false)
		extcon_set_state_sync(accdet->edev, EXTCON_JACK_HEADPHONE,
				      accdet->jack_status);

	scoped_guard(mutex, &accdet->sync_lock)
		accdet->opticalin_cable = temp_opticalin_cable;

	ACCDET_DEBUG("[accdet]%s- opticalin_cable %d\n", __func__,
		     accdet->opticalin_cable);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt8167_accdet_pm_ops, NULL,
				mt8167_accdet_resume);

static const struct of_device_id mt8167_accdet_of_match[] = {
	{ .compatible = "mediatek,mt8167-accdet", },
	{ }
};
MODULE_DEVICE_TABLE(of, mt8167_accdet_of_match);

static struct platform_driver mt8167_accdet_driver = {
	.driver = {
		.name = "mt8167-accdet",
		.of_match_table = mt8167_accdet_of_match,
		.pm = pm_sleep_ptr(&mt8167_accdet_pm_ops),
	},
	.probe = mt8167_accdet_probe,
	.remove = mt8167_accdet_remove,
};
module_platform_driver(mt8167_accdet_driver);

MODULE_DESCRIPTION("MediaTek MT8167 headset jack ACCDET driver (Amazon variant)");
MODULE_AUTHOR("Anny <Anny.Hu@mediatek.com>");
MODULE_LICENSE("GPL");
