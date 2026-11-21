// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(C) 2016 MediaTek Inc.
 *
 * Whole-chip reset for the MT7668 SDIO combo chip.
 *
 * The reset is delegated to the MMC core: mmc_hw_reset() power cycles the
 * card through the host's regulator/power sequence. While the Bluetooth
 * function is bound as well, the core instead removes and re-detects the
 * whole card so that every function driver is re-probed.
 */

#include "precomp.h"
#include "gl_os.h"
#include <linux/device.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/sdio_func.h>
#include <linux/sched/clock.h>

#if CFG_CHIP_RESET_SUPPORT

static const char *const apcChipResetReason[RST_REASON_MAX] = {
	"RST_UNKNOWN",
	"RST_PROCESS_ABNORMAL_INT",
	"RST_DRV_OWN_FAIL",
	"RST_FW_ASSERT",
	"RST_BT_TRIGGER",
	"RST_OID_TIMEOUT",
	"RST_CMD_TRIGGER",
	"RST_CR_ACCESS_FAIL",
	"RST_HIF_FAIL",
	"RST_PROBE_FAIL",
};

struct rst_struct rst_data = {
	.rst_mutex = __MUTEX_INITIALIZER(rst_data.rst_mutex),
};

enum _ENUM_CHIP_RESET_REASON_TYPE_T eResetReason;
uint64_t u8ResetTime;

static BOOLEAN g_fgIsResetting;

static void glResetWork(struct work_struct *work)
{
	struct sdio_func *func = rst_data.func;
	struct mmc_card *card = func->card;
	bool bound = func->dev.driver != NULL;
	bool shared = atomic_read(&card->sdio_funcs_probed) > (bound ? 1 : 0);
	int ret;

	if (shared && !bound) {
		/* an in-place power cycle would pull the rug from under Bluetooth */
		DBGLOG(INIT, ERROR, "[RST] card is in use by another function, not resetting\n");
		g_fgIsResetting = FALSE;
		return;
	}

	/* let cfg80211/ioctl callers that are already inside the driver finish */
	while (rst_data.entry_conut != 0)
		kalMsleep(100);

	/*
	 * Sole user: tear the WLAN function down before the chip loses power.
	 * Shared card: the MMC core unbinds every function itself.
	 */
	if (!shared)
		device_release_driver(&func->dev);

	sdio_claim_host(func);
	ret = mmc_hw_reset(card);
	sdio_release_host(func);

	if (ret < 0) {
		DBGLOG(INIT, ERROR, "[RST] mmc_hw_reset failed: %d\n", ret);
		g_fgIsResetting = FALSE;
		return;
	}

	/* ret > 0: the card is being removed and re-detected by the MMC core */
	if (ret == 0 && device_attach(&func->dev) < 0)
		DBGLOG(INIT, ERROR, "[RST] re-probe failed\n");
}

static DECLARE_WORK(rst_work, glResetWork);

BOOLEAN checkResetState(void)
{
	return g_fgIsResetting;
}

VOID glGetRstReason(enum _ENUM_CHIP_RESET_REASON_TYPE_T eReason)
{
	if (checkResetState())
		return;

	u8ResetTime = sched_clock();
	eResetReason = eReason;
}

VOID glResetTrigger(P_ADAPTER_T prAdapter, const UINT_8 *pucFile, UINT_32 u4Line)
{
	mutex_lock(&rst_data.rst_mutex);
	if (checkResetState()) {
		mutex_unlock(&rst_data.rst_mutex);
		return;
	}
	g_fgIsResetting = TRUE;
	mutex_unlock(&rst_data.rst_mutex);

	if (eResetReason > 0 && eResetReason < RST_REASON_MAX)
		DBGLOG(INIT, ERROR, "[RST] chip reset (%s) triggered in %s line %u\n",
		       apcChipResetReason[eResetReason], pucFile, u4Line);

	schedule_work(&rst_work);
}

/* Called when the function is (re-)probed, whether that succeeds or not. */
VOID glResetDone(void)
{
	g_fgIsResetting = FALSE;
}

VOID glResetUninit(void)
{
	cancel_work_sync(&rst_work);
}

#endif /* CFG_CHIP_RESET_SUPPORT */

/*
 * This routine is called for checking if the connectivity chip is resetting.
 */
BOOLEAN kalIsResetting(VOID)
{
#if CFG_CHIP_RESET_SUPPORT
	return checkResetState();
#else
	return FALSE;
#endif
}
