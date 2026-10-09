/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ODIN_OEM_UFS_H
#define _ODIN_OEM_UFS_H

#include "ufshcd.h"

/* Xiaomi's vendor ABI, from android_kernel_xiaomi_sweet@c148b77d. */
struct ufs_err_state_debug {
	u64 err_occurred;
	char err_reason[10][32];
};

struct ufs_data {
	struct ufs_hba *hba;
	struct scsi_device *sdev;
	struct ufs_err_state_debug ufs_err_state;
};

void odin_ufs_device_added(struct scsi_device *sdev);
void odin_ufs_device_removed(struct scsi_device *sdev);
u8 memblock_mem_size_in_gb(void);
struct ufs_hba *get_ufs_hba_data(void);
struct scsi_device *get_ufs_sdev_data(void);
struct ufs_data *get_ufs_data(void);
int ufshcd_read_desc(struct ufs_hba *hba, enum desc_idn desc_id,
		     int desc_index, void *buf, u32 size);
int ufs_read_desc_param(struct ufs_hba *hba, enum desc_idn desc_id,
			u8 desc_index, u8 param_offset, void *buf, u8 param_size);
int ufs_get_string_desc(struct ufs_hba *hba, void *buf, int size,
		       enum device_desc_param pname, bool ascii_std);
#endif
