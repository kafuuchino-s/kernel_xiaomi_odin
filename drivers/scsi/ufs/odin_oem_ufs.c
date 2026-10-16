// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi UFS information ABI, adapted from sweet@c148b77d for odin 5.4.
 * The original boot's disassembly confirms the getters, descriptor operations,
 * endianness conversion and RAM-size calculation. No storage settings are written.
 */
#include <linux/export.h>
#include <linux/memblock.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <asm/unaligned.h>
#include <scsi/scsi_host.h>
#include "odin_oem_ufs.h"

static struct ufs_data ufs_data;

void odin_ufs_device_added(struct scsi_device *sdev)
{
	if (sdev->lun == 0) {
		WRITE_ONCE(ufs_data.hba, shost_priv(sdev->host));
		WRITE_ONCE(ufs_data.sdev, sdev);
	}
}

void odin_ufs_device_removed(struct scsi_device *sdev)
{
	if (READ_ONCE(ufs_data.sdev) == sdev) {
		WRITE_ONCE(ufs_data.sdev, NULL);
		WRITE_ONCE(ufs_data.hba, NULL);
	}
}

struct ufs_hba *get_ufs_hba_data(void)
{
	return READ_ONCE(ufs_data.hba);
}
EXPORT_SYMBOL(get_ufs_hba_data);

struct scsi_device *get_ufs_sdev_data(void)
{
	return READ_ONCE(ufs_data.sdev);
}
EXPORT_SYMBOL(get_ufs_sdev_data);

struct ufs_data *get_ufs_data(void)
{
	return &ufs_data;
}
EXPORT_SYMBOL(get_ufs_data);

u8 memblock_mem_size_in_gb(void)
{
	return (memblock_phys_mem_size() + memblock_reserved_size()) >> 30;
}
EXPORT_SYMBOL(memblock_mem_size_in_gb);

static int odin_ufs_resume(struct ufs_hba *hba)
{
	int ret = pm_runtime_get_sync(hba->dev);

	if (ret < 0)
		pm_runtime_put_noidle(hba->dev);
	return ret < 0 ? ret : 0;
}

int ufshcd_read_desc(struct ufs_hba *hba, enum desc_idn desc_id,
		     int desc_index, void *buf, u32 size)
{
	int ret;

	if (!hba || !buf || !size || size > QUERY_DESC_MAX_SIZE)
		return -EINVAL;
	ret = odin_ufs_resume(hba);
	if (ret)
		return ret;
	ret = ufshcd_read_desc_param(hba, desc_id, desc_index, 0, buf, size);
	pm_runtime_put_sync(hba->dev);
	return ret;
}
EXPORT_SYMBOL(ufshcd_read_desc);

int ufs_read_desc_param(struct ufs_hba *hba, enum desc_idn desc_id,
			u8 desc_index, u8 param_offset, void *buf, u8 param_size)
{
	u8 raw[8];
	int ret;

	if (!hba || !buf || !param_size || param_size > sizeof(raw))
		return -EINVAL;
	ret = odin_ufs_resume(hba);
	if (ret)
		return ret;
	ret = ufshcd_read_desc_param(hba, desc_id, desc_index, param_offset,
				     raw, param_size);
	pm_runtime_put_sync(hba->dev);
	if (ret)
		return ret;
	switch (param_size) {
	case 2:
		put_unaligned(get_unaligned_be16(raw), (u16 *)buf);
		break;
	case 4:
		put_unaligned(get_unaligned_be32(raw), (u32 *)buf);
		break;
	case 8:
		put_unaligned(get_unaligned_be64(raw), (u64 *)buf);
		break;
	default:
		*(u8 *)buf = raw[0];
	}
	return 0;
}
EXPORT_SYMBOL(ufs_read_desc_param);

int ufs_get_string_desc(struct ufs_hba *hba, void *buf, int size,
		       enum device_desc_param pname, bool ascii_std)
{
	u8 *descriptor, *string = NULL;
	int length = QUERY_DESC_MAX_SIZE, ret;

	if (!hba || !buf || size <= 0 || pname < 0 || pname >= length)
		return -EINVAL;
	descriptor = kzalloc(length, GFP_KERNEL);
	if (!descriptor)
		return -ENOMEM;
	ret = odin_ufs_resume(hba);
	if (ret)
		goto free_descriptor;
	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_READ_DESC,
		QUERY_DESC_IDN_DEVICE, 0, 0, descriptor, &length);
	if (ret)
		goto release_pm;
	if (pname >= length) {
		ret = -EINVAL;
		goto release_pm;
	}
	ret = ufshcd_read_string_desc(hba, descriptor[pname], &string, ascii_std);
	if (ret >= 0) {
		/* The provider allocates only the returned length, not size bytes. */
		memset(buf, 0, size);
		if (string)
			memcpy(buf, string, min(size, ret));
	}
	kfree(string);
release_pm:
	pm_runtime_put_sync(hba->dev);
free_descriptor:
	kfree(descriptor);
	return ret;
}
EXPORT_SYMBOL(ufs_get_string_desc);
