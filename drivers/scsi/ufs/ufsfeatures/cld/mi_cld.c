// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi CLD, adapted from Kartik-91/android_kernel_xiaomi_mt6877@0835b4fe.
 * Keep the public CLD layout unchanged for odin's vendor ABI. sysfs_lock
 * serializes operations; host_lock serializes close/admission with MMIO.
 * Never synchronously drain work from EH or while holding these locks.
 */
#include "../../ufshcd.h"
#include "mi_cld.h"
#include "mi_cld_sysfs.h"

#define CLD_MAX_RUN_MS (10 * 60 * 1000)
#define CLD_DOORBELL_TIMEOUT_US 1000000ULL

extern struct ufscld_ops samsung_cld_ops;
extern struct ufscld_ops hynix_cld_ops;
static struct vendor_ops cld_ops_arry[] = {
	{ "SAMSUNG", AUTO_HIBERN8_ENABLE, &samsung_cld_ops },
	{ "SKhynix", AUTO_HIBERN8_ENABLE, &hynix_cld_ops },
};

/* ponytail: odin has one internal UFS controller. Use per-host private storage
 * if porting to a machine with multiple UFS controllers; don't change the ABI.
 * Operation/rollback flags use sysfs_lock. initialized is published during
 * the probe; the native UFS teardown drains that probe before withdrawing it.
 */
static struct {
	struct ufs_hba *owner;
	bool initialized;
	bool disable_needed;
	bool restore_ahit;
	bool cleanup;
	bool off_event_pending;
	int last_error;
} cld_runtime;

int ufscld_get_state(struct ufs_hba *hba)
{
	return atomic_read(&hba->cld.cld_state);
}

void ufscld_set_state(struct ufs_hba *hba, int state)
{
	unsigned long flags;

	spin_lock_irqsave(hba->host->host_lock, flags);
	if (ufscld_get_state(hba) != CLD_REMOVED)
		atomic_set(&hba->cld.cld_state, state);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	/* cld_ops is assigned only after the HBA tag queue is initialized. */
	if (hba->cld.cld_ops)
		wake_up(&hba->dev_cmd.tag_wq);
}

int ufscld_is_not_present(struct ufscld_dev *cld)
{
	return ufscld_get_state(cld->hba) == CLD_PRESENT ? 0 : -ENODEV;
}

/* Called at the actual doorbell/AHIT write with host_lock held. Core recovery
 * queries use a separate, unguarded path so recovery cannot deadlock on CLD.
 */
bool ufscld_command_allowed(struct ufs_hba *hba, bool cleanup)
{
	int state = ufscld_get_state(hba);

	lockdep_assert_held(hba->host->host_lock);
	if (cld_runtime.owner != hba || !READ_ONCE(cld_runtime.initialized))
		return false;
	if (!cleanup)
		return state == CLD_PRESENT;
	return state == CLD_PRESENT || state == CLD_STOPPING ||
	       state == CLD_SUSPENDED || state == CLD_REMOVED ||
	       state == CLD_FAILED;
}

int mi_ufs_query_attr(struct ufs_hba *hba, enum query_opcode opcode,
		     enum attr_idn idn, u8 index, u8 selector, u32 *value)
{
	int ret, attempt;
	bool writing = opcode == UPIU_QUERY_OPCODE_WRITE_ATTR;
	bool disabling = writing && !*value;

	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}
	/* A command may have reached the device even if its response times out. */
	if (writing && !disabling)
		cld_runtime.disable_needed = true;
	for (attempt = 0; attempt < 3; attempt++) {
		ret = ufshcd_query_cld_attr(hba, opcode, idn, index, selector,
					    value, cld_runtime.cleanup);
		if (!ret || ret == -EBUSY || ret == -ENODEV)
			break;
	}
	if (!ret && disabling)
		cld_runtime.disable_needed = false;
	pm_runtime_put_sync(hba->dev);
	return ret;
}

static bool ufscld_ahit_supported(struct ufs_hba *hba)
{
	return ufshcd_is_auto_hibern8_supported(hba) &&
	       !(hba->quirks & UFSHCD_QUIRK_BROKEN_AUTO_HIBERN8);
}

int ufscld_auto_hibern8_enable(struct ufscld_dev *cld, unsigned int val)
{
	struct ufs_hba *hba = cld->hba;
	unsigned long flags;
	int ret;

	val = !!val;
	if (!ufscld_ahit_supported(hba))
		return -EOPNOTSUPP;
	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}
	ret = ufshcd_hold(hba, false);
	if (ret)
		goto put_pm;
	down_write(&hba->clk_scaling_lock);
	ufshcd_scsi_block_requests(hba);
	ret = ufshcd_wait_for_doorbell_clr(hba, CLD_DOORBELL_TIMEOUT_US);
	if (!ret) {
		spin_lock_irqsave(hba->host->host_lock, flags);
		if (!ufshcd_cld_command_allowed(hba, cld_runtime.cleanup)) {
			ret = -EBUSY;
		} else {
			ufshcd_writel(hba, val ? hba->ahit : cld_AUTO_HIBERN8_DISABLE,
				      REG_AUTO_HIBERNATE_IDLE_TIMER);
			mb();
			cld->is_auto_enabled = val;
			if (val)
				cld_runtime.restore_ahit = false;
		}
		spin_unlock_irqrestore(hba->host->host_lock, flags);
	}
	ufshcd_scsi_unblock_requests(hba);
	up_write(&hba->clk_scaling_lock);
	ufshcd_release(hba);
put_pm:
	pm_runtime_put_sync(hba->dev);
	return ret;
}

int ufscld_block_enter_suspend(struct ufscld_dev *cld)
{
	struct ufs_hba *hba = cld->hba;
	int ret;

	if (cld->block_suspend)
		return 0;
	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}
	ret = ufshcd_hold(hba, false);
	if (ret) {
		pm_runtime_put_sync(hba->dev);
		return ret;
	}
	cld->block_suspend = true;
	return 0;
}

void ufscld_allow_enter_suspend(struct ufscld_dev *cld)
{
	if (!cld->block_suspend)
		return;
	cld->block_suspend = false;
	ufshcd_release(cld->hba);
	pm_runtime_mark_last_busy(cld->hba->dev);
	pm_runtime_put_sync(cld->hba->dev);
}

int ufscld_get_frag_level(struct ufscld_dev *cld, int *frag_level)
{
	return cld->cld_ops->cld_get_frag_level(cld, frag_level);
}

int ufscld_get_operation_status(struct ufscld_dev *cld, int *op_status)
{
	return cld->cld_ops->cld_operation_status(cld, op_status);
}

/* Sysfs diagnostics may start ANALYZE even after the HAL receives OFF. */
int ufscld_finish_read(struct ufscld_dev *cld, int result)
{
	struct ufs_hba *hba = cld->hba;
	unsigned long flags;
	int ret;

	if (READ_ONCE(cld->cld_trigger) || !cld_runtime.disable_needed)
		return result;
	cld_runtime.cleanup = true;
	ret = cld->cld_ops->cld_set_trigger(cld, 0);
	cld_runtime.cleanup = false;
	if (ret) {
		WRITE_ONCE(cld_runtime.last_error, ret);
		spin_lock_irqsave(hba->host->host_lock, flags);
		if (ufscld_get_state(hba) == CLD_PRESENT)
			atomic_set(&cld->cld_state, CLD_STOPPING);
		if (ufscld_get_state(hba) == CLD_STOPPING)
			schedule_delayed_work(&cld->cld_trigger_work, 0);
		spin_unlock_irqrestore(hba->host->host_lock, flags);
	}
	return result ? result : ret;
}

/* Caller holds sysfs_lock. Failed hardware actions remain retryable. */
int ufscld_trigger_off(struct ufscld_dev *cld)
{
	struct ufs_hba *hba = cld->hba;
	unsigned long flags;
	char error[32];
	char *event[] = { "CLD_TRIGGER=OFF", error, NULL };
	int ret = 0, ahit_ret;

	spin_lock_irqsave(hba->host->host_lock, flags);
	WRITE_ONCE(cld->cld_trigger, false);
	if (ufscld_get_state(hba) == CLD_PRESENT)
		atomic_set(&cld->cld_state, CLD_STOPPING);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	cancel_delayed_work(&cld->cld_trigger_work);
	cld_runtime.cleanup = true;
	if (cld_runtime.disable_needed)
		ret = cld->cld_ops->cld_set_trigger(cld, 0);
	if (cld_runtime.restore_ahit) {
		ahit_ret = ufscld_auto_hibern8_enable(cld, 1);
		if (!ret)
			ret = ahit_ret;
	}
	cld_runtime.cleanup = false;
	ufscld_allow_enter_suspend(cld);
	spin_lock_irqsave(hba->host->host_lock, flags);
	if (!ret && ufscld_get_state(hba) == CLD_STOPPING)
		atomic_set(&cld->cld_state, CLD_PRESENT);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	if (cld_runtime.off_event_pending) {
		/* OFF means the operation terminated, not proof the media is clean. */
		if (ret)
			WRITE_ONCE(cld_runtime.last_error, ret);
		snprintf(error, sizeof(error), "CLD_ERROR=%d", READ_ONCE(cld_runtime.last_error));
		kobject_uevent_env(&hba->dev->kobj, KOBJ_CHANGE, event);
		cld_runtime.off_event_pending = false;
	}
	if (ret)
		dev_err(hba->dev, "CLD stop/rollback incomplete: %d\n", ret);
	return ret;
}

/* Call only under host_lock. Idle activity never extends an existing timer. */
static void ufscld_queue(struct ufscld_dev *cld, unsigned int delay_ms)
{
	s64 remaining = CLD_MAX_RUN_MS -
		(ktime_to_ms(ktime_get()) - cld->start_time_stamp);

	if (remaining <= 0)
		delay_ms = 0;
	else
		delay_ms = min_t(s64, delay_ms, remaining);
	schedule_delayed_work(&cld->cld_trigger_work, msecs_to_jiffies(delay_ms));
}

int ufscld_trigger_on(struct ufscld_dev *cld)
{
	struct ufs_hba *hba = cld->hba;
	unsigned long flags;
	int ret, status;

	if (ufscld_is_not_present(cld))
		return -ENODEV;
	if (READ_ONCE(cld->cld_trigger))
		return 0;
	WRITE_ONCE(cld_runtime.last_error, 0);
	ret = ufscld_block_enter_suspend(cld);
	if (ret)
		return ret;
	if (cld->vendor_ops->auto_hibern8_enable && ufscld_ahit_supported(hba)) {
		bool was_enabled = cld->is_auto_enabled;

		ret = ufscld_auto_hibern8_enable(cld, 0);
		if (ret)
			goto stop;
		cld_runtime.restore_ahit = was_enabled;
	}
	cld->start_time_stamp = ktime_to_ms(ktime_get());
	/* Samsung requires ANALYZE -> read level -> EXECUTE. */
	ret = ufscld_get_operation_status(cld, &status);
	if (ret)
		goto stop;
	ret = cld->cld_ops->cld_set_trigger(cld, 1);
	if (ret)
		goto stop;
	spin_lock_irqsave(hba->host->host_lock, flags);
	if (ufscld_get_state(hba) != CLD_PRESENT) {
		spin_unlock_irqrestore(hba->host->host_lock, flags);
		ret = -ENODEV;
		goto stop;
	}
	WRITE_ONCE(cld->cld_trigger, true);
	cld_runtime.off_event_pending = true;
	ufscld_queue(cld, 0);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	return 0;
stop:
	WRITE_ONCE(cld_runtime.last_error, ret);
	ufscld_trigger_off(cld);
	return ret;
}

static void ufscld_trigger_work_fn(struct work_struct *work)
{
	struct ufscld_dev *cld = container_of(work, struct ufscld_dev,
					    cld_trigger_work.work);
	struct ufs_hba *hba = cld->hba;
	unsigned long flags;
	int ret, status, state;

	mutex_lock(&cld->sysfs_lock);
	state = ufscld_get_state(hba);
	if (state == CLD_STOPPING || state == CLD_FAILED) {
		ufscld_trigger_off(cld);
		goto out;
	}
	if (!READ_ONCE(cld->cld_trigger) || state != CLD_PRESENT)
		goto out;
	if (ktime_to_ms(ktime_get()) - cld->start_time_stamp >= CLD_MAX_RUN_MS) {
		dev_err(hba->dev, "CLD exceeded maximum run time\n");
		WRITE_ONCE(cld_runtime.last_error, -ETIMEDOUT);
		ufscld_trigger_off(cld);
		goto out;
	}
	ret = ufscld_get_operation_status(cld, &status);
	if (ufscld_is_not_present(cld))
		goto out;
	if (ret || status == CLD_STATUS_IDLE) {
		WRITE_ONCE(cld_runtime.last_error, ret);
		ufscld_trigger_off(cld);
		goto out;
	}
	ret = cld->cld_ops->cld_set_trigger(cld, 1);
	if (ret) {
		WRITE_ONCE(cld_runtime.last_error, ret);
		ufscld_trigger_off(cld);
		goto out;
	}
	spin_lock_irqsave(hba->host->host_lock, flags);
	if (ufscld_get_state(hba) == CLD_PRESENT && READ_ONCE(cld->cld_trigger))
		ufscld_queue(cld, cld->cld_trigger_delay);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
out:
	mutex_unlock(&cld->sysfs_lock);
}

void ufscld_on_idle(struct ufs_hba *hba)
{
	struct ufscld_dev *cld = &hba->cld;

	lockdep_assert_held(hba->host->host_lock);
	if (ufscld_get_state(hba) == CLD_PRESENT &&
	    READ_ONCE(cld->cld_trigger) && !hba->outstanding_reqs &&
	    !work_busy(&cld->cld_trigger_work.work))
		ufscld_queue(cld, min(cld->cld_trigger_delay, 1000U));
}

void ufscld_set_init_state(struct ufs_hba *hba)
{
	struct ufscld_dev *cld = &hba->cld;

	cld->hba = hba;
	mutex_init(&cld->sysfs_lock);
	INIT_DELAYED_WORK(&cld->cld_trigger_work, ufscld_trigger_work_fn);
	cld->cld_trigger_delay = CLD_TRIGGER_WORKER_DELAY_MS_DEFAULT;
	if (cld_runtime.owner && cld_runtime.owner != hba) {
		atomic_set(&cld->cld_state, CLD_FAILED);
		return;
	}
	memset(&cld_runtime, 0, sizeof(cld_runtime));
	cld_runtime.owner = hba;
	atomic_set(&cld->cld_state, CLD_NEED_INIT);
}

void ufscld_init(struct ufs_hba *hba)
{
	struct ufscld_dev *cld = &hba->cld;
	struct scsi_device *sdev = hba->sdev_ufs_device;
	int i, ret;

	if (ufscld_get_state(hba) != CLD_NEED_INIT)
		return;
	if (!sdev || !sdev->inquiry || sdev->inquiry_len < 16) {
		ufscld_set_state(hba, CLD_FAILED);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(cld_ops_arry); i++) {
		if (!memcmp(sdev->inquiry + 8, cld_ops_arry[i].vendor_id,
			    strlen((char *)cld_ops_arry[i].vendor_id))) {
			cld->cld_ops = cld_ops_arry[i].cld_ops;
			cld->vendor_ops = &cld_ops_arry[i];
			break;
		}
	}
	if (!cld->cld_ops) {
		ufscld_set_state(hba, CLD_FAILED);
		dev_info(hba->dev, "CLD unavailable for UFS vendor %.8s\n", sdev->inquiry + 8);
		return;
	}
	cld->is_auto_enabled = ufscld_ahit_supported(hba);
	ret = ufscld_create_sysfs(cld);
	WRITE_ONCE(cld_runtime.initialized, !ret);
	ufscld_set_state(hba, ret ? CLD_FAILED : CLD_PRESENT);
}

void ufscld_remove(struct ufs_hba *hba)
{
	struct ufscld_dev *cld = &hba->cld;
	bool published;

	if (!cld->hba || cld_runtime.owner != hba)
		return;
	ufscld_set_state(hba, CLD_REMOVED);
	published = cld->kobj.state_in_sysfs;
	if (published)
		ufscld_remove_sysfs(cld);
	cancel_delayed_work_sync(&cld->cld_trigger_work);
	mutex_lock(&cld->sysfs_lock);
	if (cld->cld_ops)
		ufscld_trigger_off(cld);
	mutex_unlock(&cld->sysfs_lock);
	cld_runtime.owner = NULL;
	if (published)
		kobject_put(&cld->kobj);
}

int ufscld_suspend(struct ufs_hba *hba)
{
	struct ufscld_dev *cld = &hba->cld;
	unsigned long flags;
	int ret = 0;

	if (!cld->hba || cld_runtime.owner != hba ||
	    !READ_ONCE(cld_runtime.initialized))
		return 0;
	spin_lock_irqsave(hba->host->host_lock, flags);
	if (ufscld_get_state(hba) != CLD_REMOVED)
		atomic_set(&cld->cld_state, CLD_SUSPENDED);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	if (cld->cld_ops)
		wake_up(&hba->dev_cmd.tag_wq);
	cancel_delayed_work_sync(&cld->cld_trigger_work);
	mutex_lock(&cld->sysfs_lock);
	if (cld->cld_ops)
		ret = ufscld_trigger_off(cld);
	mutex_unlock(&cld->sysfs_lock);
	if (ret)
		ufscld_resume(hba);
	return ret;
}

/* A reset interrupts cleanup; it never restarts an old EXECUTE automatically. */
void ufscld_reset_host(struct ufs_hba *hba)
{
	unsigned long flags;

	spin_lock_irqsave(hba->host->host_lock, flags);
	if (ufscld_get_state(hba) == CLD_PRESENT ||
	    ufscld_get_state(hba) == CLD_STOPPING) {
		atomic_set(&hba->cld.cld_state, CLD_RESET);
		WRITE_ONCE(hba->cld.cld_trigger, false);
		WRITE_ONCE(cld_runtime.last_error, -EINTR);
		cancel_delayed_work(&hba->cld.cld_trigger_work);
	}
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	wake_up(&hba->dev_cmd.tag_wq);
}

void ufscld_reset(struct ufs_hba *hba, bool recovered)
{
	unsigned long flags;
	int state;

	spin_lock_irqsave(hba->host->host_lock, flags);
	state = ufscld_get_state(hba);
	if (cld_runtime.owner == hba && READ_ONCE(cld_runtime.initialized) &&
	    hba->cld.cld_ops && (state == CLD_RESET || state == CLD_FAILED)) {
		atomic_set(&hba->cld.cld_state,
			recovered ? CLD_STOPPING : CLD_FAILED);
		schedule_delayed_work(&hba->cld.cld_trigger_work, 0);
	}
	spin_unlock_irqrestore(hba->host->host_lock, flags);
}

void ufscld_resume(struct ufs_hba *hba)
{
	unsigned long flags;

	spin_lock_irqsave(hba->host->host_lock, flags);
	if (cld_runtime.owner == hba && READ_ONCE(cld_runtime.initialized) &&
	    hba->cld.cld_ops && ufscld_get_state(hba) == CLD_SUSPENDED) {
		atomic_set(&hba->cld.cld_state, CLD_STOPPING);
		schedule_delayed_work(&hba->cld.cld_trigger_work, 0);
	}
	spin_unlock_irqrestore(hba->host->host_lock, flags);
}
