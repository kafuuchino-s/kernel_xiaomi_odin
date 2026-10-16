/*
 * mi_cld1.c
 *
 *  Created on: 2020-10-20
 *      Author: shane
 */

#include "../../ufshcd.h"
#include "mi_cld.h"

#define INFO_MSG pr_debug
#define ERR_MSG pr_err

#define QUERY_ATTR_IDN_CLD_ENABLE 0x20
#define QUERY_ATTR_IDN_CLD_LEVEL 0x21
#define HID_SELECTOR 1
#define CLD_WAIT_CLEAN_TIMES 20000

int samsung_get_frag_level(struct ufscld_dev *cld, int *frag_level)
{
	struct ufs_hba *hba = cld->hba;
	int ret;
	u32 attr = 1; /* Analyze before reading the fragmentation level. */
	ret =  mi_ufs_query_attr(hba, UPIU_QUERY_OPCODE_WRITE_ATTR, (enum attr_idn)QUERY_ATTR_IDN_CLD_ENABLE, 0, HID_SELECTOR, &attr);
	if (ret) {
		ERR_MSG("do analysis fail. op code=UPIU_QUERY_OPCODE_WRITE_ATTR, attr_id=%d\n", (int)QUERY_ATTR_IDN_CLD_ENABLE);
		return ret;
	}
	ret = mi_ufs_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR, (enum attr_idn)QUERY_ATTR_IDN_CLD_LEVEL, 0, HID_SELECTOR, &attr);
	if (ret) {
		ERR_MSG("read frag level  fail,op code=UPIU_QUERY_OPCODE_READ_ATTR, attr_id=%d\n", (int)QUERY_ATTR_IDN_CLD_LEVEL);
		return ret;
	}

	INFO_MSG("get_level pass, attr=0x%x\n", attr);


	attr = attr & 0xF;

	if (attr == 0) {
		*frag_level = CLD_LEV_NA;
	} else if (attr == 1) {
		*frag_level = CLD_LEV_CLEAN;
	} else if (attr == 2) {
		*frag_level = CLD_LEV_WARN;
	} else if (attr == 3) {
		*frag_level = CLD_LEV_CRITICAL;
	} else {
		pr_info("samsung cld unknown level %d\n", attr);
		return -ERANGE;
	}
	return 0;
}

int samsung_cld_set_trigger(struct ufscld_dev *cld, u32 trigger)
{
	struct ufs_hba *hba = cld->hba;
	int ret = 0;

	if (trigger)
		trigger  = 2;// execute after analyze.

	ret =  mi_ufs_query_attr(hba, UPIU_QUERY_OPCODE_WRITE_ATTR, (enum attr_idn)QUERY_ATTR_IDN_CLD_ENABLE, 0, HID_SELECTOR, &trigger);

	if (ret) {
		ERR_MSG("send execute afger analysis  failed. attr_idn=%d, opcode=UPIU_QUERY_OPCODE_WRITE_ATTR, trigger=%d", QUERY_ATTR_IDN_CLD_ENABLE, trigger);
	}

	return ret;
}

int samsung_cld_get_trigger(struct ufscld_dev *cld, u32 *trigger)
{
	struct ufs_hba *hba = cld->hba;
	int ret = 0;

	ret =  mi_ufs_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR, (enum attr_idn)QUERY_ATTR_IDN_CLD_ENABLE, 0, HID_SELECTOR, trigger);

	if (ret) {
		ERR_MSG("read cld trigger  failed. attr_idn=%d, opcode=UPIU_QUERY_OPCODE_WRITE_ATTR, trigger=%d", QUERY_ATTR_IDN_CLD_ENABLE, trigger);
	}

	return ret;
}

int samsung_cld_operation_status(struct ufscld_dev *cld, int *op_status)
{
	enum CLD_LEVEL frag_level;
	enum CLD_STATUS op_status_tmp;
	int ret = 0;
	ktime_t delta_time_stamp = 0;

	ret = samsung_get_frag_level(cld, (int*) &frag_level);
	if (ret)
		return ret;

	if (frag_level == CLD_LEV_CLEAN || frag_level == CLD_LEV_NA) {// if cld was done
		op_status_tmp = CLD_STATUS_IDLE;
	} else if (frag_level == CLD_LEV_WARN || frag_level == CLD_LEV_CRITICAL){
		op_status_tmp = CLD_STATUS_PROGRESSING;
	} else {
		op_status_tmp = CLD_STATUS_NA;
	}

	delta_time_stamp = ktime_to_ms(ktime_get()) - cld->start_time_stamp;

	if (op_status_tmp == CLD_STATUS_IDLE) {
		if (delta_time_stamp >= CLD_WAIT_CLEAN_TIMES) {
			*op_status = CLD_STATUS_IDLE;
		} else {
			*op_status = CLD_STATUS_PROGRESSING;
		}
	} else {
		*op_status = op_status_tmp;
	}

	return 0;
}

struct ufscld_ops samsung_cld_ops = {
		.cld_get_frag_level = samsung_get_frag_level,
		.cld_set_trigger = samsung_cld_set_trigger,
		.cld_get_trigger = samsung_cld_get_trigger,
		.cld_operation_status = samsung_cld_operation_status
};
