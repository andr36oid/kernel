#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <net/mac80211.h>
#include <linux/time.h>
#include <linux/pm.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <net/rtnetlink.h>

#include "core.h"
#include "if_io.h"
#include "soc.h"
#include "hal.h"
#include "utils.h"
#include "platform.h"
#include "hal_io.h"
#include "sdio.h"

struct hal_priv *hpriv;
bool m0_jtag_enable = false;

/* Runtime fallback is allowed only between PM/restart transactions. Turning
 * this off does not claim an already reset chip is live: the next open still
 * performs its necessary checked cold start, then later sleeps use legacy PM. */
bool pm_checked_resume = true;
static int rk915_set_pm_checked_resume(const char *val,
				      const struct kernel_param *kp)
{
	bool enabled, sleep_locked = false;
	int ret = kstrtobool(val, &enabled);

	if (ret)
		return ret;
	/* Serialize the two policy flags against the WHOLE PM transaction,
	 * not two independently sampled PREPARE flags. The PM helper marks
	 * this writer freezer-skippable while waiting; resume never takes
	 * the module parameter lock. No radio IO or blocking lock follows. */
	if (hpriv) {
		lock_system_sleep();
		sleep_locked = true;
	}
	/* Same lifetime order as probe/remove: pair lock, RTNL, firmware. */
	if (!rk915_sdio_policy_lock()) {
		ret = -EBUSY;
		goto unlock_sleep;
	}
	if (!rtnl_trylock()) {
		rk915_sdio_policy_unlock();
		ret = -EBUSY;
		goto unlock_sleep;
	}
	if (hpriv && !mutex_trylock(&hpriv->fw_mutex)) {
		ret = -EBUSY;
		goto unlock;
	}
	if (hpriv) {
		if (READ_ONCE(hpriv->pm_system_suspend) ||
		    READ_ONCE(hpriv->during_pm_resume))
			ret = -EBUSY;
		else if (hpriv->hal_init && wifi && wifi->hw)
			ret = ieee80211_set_deferred_resume(wifi->hw, enabled);
		if (!ret)
			WRITE_ONCE(*(bool *)kp->arg, enabled);
		mutex_unlock(&hpriv->fw_mutex);
	} else {
		WRITE_ONCE(*(bool *)kp->arg, enabled);
	}
unlock:
	rtnl_unlock();
	rk915_sdio_policy_unlock();
unlock_sleep:
	if (sleep_locked)
		unlock_system_sleep();
	return ret;
}
static const struct kernel_param_ops pm_checked_resume_ops = {
	.set = rk915_set_pm_checked_resume,
	.get = param_get_bool,
};
module_param_cb(pm_checked_resume, &pm_checked_resume_ops,
		&pm_checked_resume, 0644);
MODULE_PARM_DESC(pm_checked_resume,
	"HELD: Y=async cold restart; N=legacy retained PM; writable only while awake and no restart active");

/* Caller serializes the complete firmware lifecycle with fw_mutex. */
static int rk915_fw_ready_locked(struct hal_priv *priv, bool cold)
{
	int ret, state;
	bool host_locked = false;

	if (cold) {
		WRITE_ONCE(priv->pm_fresh_ready, false);
		WRITE_ONCE(priv->pm_transport_ready, false);
		/* The PM hold is paired with start()'s MAC-ready release. Legacy
		 * firmware recovery has its own ready path, not that release. */
		if (READ_ONCE(priv->pm_need_cold)) {
			ret = sdio_set_cold_reset(priv->io_info->priv_data, true);
			if (ret)
				goto out;
		}
		/* No host I/O may overlap reset, function setup or firmware download. */
		rk915_lock(priv);
		host_locked = true;
		ret = rk915_poweron();
		if (ret)
			goto out;
		msleep(RK915_POWER_ON_DELAY_MS);
		ret = rk915_platform_bus_rec_init(priv->io_info);
		if (ret)
			goto out;
		msleep(RK915_POWER_ON_DELAY_MS);
	} else {
		ret = rk915_platform_bus_init(priv->io_info);
		if (ret)
			goto out;
	}

	ret = rk915_download_firmware(priv);
	if (ret)
		goto out;
	if (cold) {
		/* Do not accept stale M0_SLEEP or RESET_COMPLETE as fresh firmware. */
		state = priv->io_info->io_ops->io_readb(priv->io_info, IO_FW_STATE);
		if (state != M0_READY) {
			ret = state < 0 ? state : -EPROTO;
			goto out;
		}
	}
	ret = rk915_io_init(priv);
	if (ret)
		goto out;
	if (!priv->io_info->irq_request && !down_fw_in_probe) {
		ret = rk915_register_irq(priv->io_info);
		if (ret)
			goto out;
	}
	if (cold && READ_ONCE(priv->pm_need_cold)) {
		/* Only this checked read permits skipping the retained-M0 wake. */
		WRITE_ONCE(priv->pm_fresh_ready, true);
		WRITE_ONCE(priv->pm_transport_ready, true);
		WRITE_ONCE(priv->pm_notify_error, 0);
	}
out:
	if (host_locked)
		rk915_unlock(priv);
	if (ret)
		RPU_ERROR_MAIN("checked firmware ready cold=%u failed: %d\n", cold, ret);
	return ret;
}

static int fw_bring_up(void *p)
{
	struct hal_priv *priv = p;
	bool cold;
	int ret;

	mutex_lock(&priv->fw_mutex);
	cold = READ_ONCE(priv->fw_error_processing) || READ_ONCE(priv->pm_need_cold);
	ret = rk915_fw_ready_locked(priv, cold);
	mutex_unlock(&priv->fw_mutex);
	return ret;
}

static int fw_tear_down(void *p)
{
	struct hal_priv *priv = p;
	int ret = 0;

	mutex_lock(&priv->fw_mutex);
	WRITE_ONCE(priv->pm_transport_ready, false);
	if (priv->fw_error_processing)
		rk915_poweroff();
	else
		ret = rk915_platform_bus_deinit(priv->io_info);
	mutex_unlock(&priv->fw_mutex);
	return ret;
}

static void rk915_core_deinit(void)
{
	struct host_io_info *host;

	if (!hpriv)
		return;

	host = hpriv->io_info;
	if (host)
		rk915_free_firmware_buf(&host->firmware);
	if (host && host->rx_serias_buf)
		kfree(host->rx_serias_buf);
	if (hpriv)
		kfree(hpriv);
	if (host)
		kfree(host);
	hpriv = NULL;
}

static int rk915_core_init(void)
{
	struct host_io_info *host = NULL;
	struct hal_priv *priv = NULL;

	RPU_INFO_MAIN("%s.\n", __func__);

	host = kzalloc(sizeof(struct host_io_info), GFP_KERNEL);
	if (!host) {
		RPU_ERROR_MAIN("%s: kalloc hal_priv failed\n", __func__);
		goto err;
	}

	host->rx_serias_buf = kzalloc(MAX_RX_SERIAS_BYTES, GFP_KERNEL);
	if (!host->rx_serias_buf) {
		RPU_ERROR_MAIN("%s: kalloc hal_priv failed\n", __func__);
		goto err;
	}

	host->rx_serias_idx = -1;
	host->rx_serias_count = 0;
	host->rx_next_len = 0;
	host->bus_init = false;

	if (rk915_alloc_firmware_buf(&host->firmware) != 0) {
		RPU_ERROR_MAIN("%s: rk915_alloc_firmware_buf failed\n", __func__);
		goto err;
	}

	priv = kzalloc(sizeof(struct hal_priv), GFP_KERNEL);
	if (!priv) {
		RPU_ERROR_MAIN("%s: kalloc hal_priv failed\n", __func__);
		goto err;
	}

	hpriv = priv;
	priv->io_info = host;
	mutex_init(&priv->fw_mutex);
	mutex_init(&priv->rx_io_mutex);

	rk915_sdio_pre_init();

	return 0;

err:
	rk915_core_deinit();

	return -1;
}

int rk915_probe(struct platform_device *pdev)
{
	int ret;
	struct host_io_info *host = hpriv->io_info;

	RPU_INFO_MAIN("%s\n", __func__);

	hpriv->fw_bring_up_func = fw_bring_up;
	hpriv->fw_tear_down_func = fw_tear_down;

	/* Initialize the rest of the layer */
	ret = hal_ops.init(host->dev);
	if (ret < 0) {
		RPU_ERROR_MAIN("%s: hal_ops.init failed\n", __func__);
		return -1;
	}

	if (down_fw_in_probe) {
		if (fw_bring_up(hpriv)) {
			RPU_ERROR_MAIN("%s: fw_bring_up failed\n", __func__);
			return -1;
		}

		if (rk915_register_irq(hpriv->io_info)) {
			RPU_ERROR_MAIN("%s: rk915_irq_register failed\n", __func__);
			return -1;
		}
	}

	return 0;
}

int rk915_remove(struct platform_device *pdev)
{
	RPU_INFO_MAIN("%s\n", __func__);

	hal_ops.deinit(NULL);

	rk915_free_irq(hpriv->io_info);

	return 0;
}

void rk915_shutdown(struct platform_device *pdev)
{
	//RPU_INFO_MAIN("%s\n", __func__);
	hpriv->shutdown = 1;
}

static const struct platform_device_id rk915_id_table[] = {
    {
        .name = "rk915",
        .driver_data = 0x00,
    },
    {},
};
MODULE_DEVICE_TABLE(platform, rk915_id_table);

static struct platform_driver rk915_driver =
{
    .probe = rk915_probe,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
    .remove = __devexit_p(rk915_remove),
#else
    .remove = rk915_remove,
#endif
    .shutdown = rk915_shutdown,
    .id_table = rk915_id_table,
    .driver = {
        .name = "Rockchip rk915 wifi driver",
        .owner = THIS_MODULE,
        .probe_type = PROBE_FORCE_SYNCHRONOUS,
    }
};

static int __init rk915_init(void)
{
	int ret;

	RPU_INFO_MAIN("=======================================================\n");
	RPU_INFO_MAIN("==== Launching Wi-Fi driver! (Powered by Rockchip) ====\n");
	RPU_INFO_MAIN("=======================================================\n");
	RPU_INFO_MAIN("RK915 WiFi Ver: %s\n", VERSION_INFO);
	RPU_INFO_MAIN("Build time: %s %s\n", __DATE__, __TIME__);

	ret = rk915_core_init();
	if (ret) {
		RPU_ERROR_MAIN("%s: rk915_core_init failed\n", __func__);
		goto error;
	}

	ret = platform_driver_register(&rk915_driver);
	if (ret)
		goto error;
	/* Restore/scan BEFORE driver registration: bus probe writes CCCR
	 * before calling our probe, so powering up afterwards is too late. */
	ret = rk915_sdio_prepare_card();
	if (ret)
		goto error1;
	ret = rk915_bus_register_driver();
	if (ret)
		goto error1;

	ret = rk915_sdio_init(hpriv->io_info);
	if (ret) {
		RPU_ERROR_MAIN("%s: platform_bus_init failed\n", __func__);
		goto error2;
	}

	return ret;
error2:
	rk915_bus_unregister_driver();
error1:
	platform_driver_unregister(&rk915_driver);
error:
	rk915_core_deinit();
	return ret;
}

static void __exit rk915_exit(void)
{
	RPU_INFO_MAIN("==========================================================\n");
	RPU_INFO_MAIN("==== Dislaunching Wi-Fi driver! (Powered by Rockchip) ====\n");
	RPU_INFO_MAIN("==========================================================\n");

	/* Prevent new function-pair probes before unregistering the platform. */
	rk915_bus_unregister_driver();
	platform_driver_unregister(&rk915_driver);
	rk915_poweroff();

	rk915_core_deinit();
}

module_init(rk915_init);
module_exit(rk915_exit);

module_param_named(jtag, m0_jtag_enable, bool, 0644);
MODULE_AUTHOR("Rockchips");
MODULE_DESCRIPTION("Driver for Rockchips RK915 SDIO WiFi Devices");
MODULE_LICENSE("GPL");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
