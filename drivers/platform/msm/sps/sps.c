/* Copyright (c) 2011-2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Smart-Peripheral-Switch (SPS) Module. */

#include <linux/types.h>	/* u32 */
#include <linux/kernel.h>	/* pr_info() */
#include <linux/module.h>	/* module_init() */
#include <linux/slab.h>		/* kzalloc() */
#include <linux/mutex.h>	/* mutex */
#include <linux/device.h>	/* device */
#include <linux/fs.h>		/* alloc_chrdev_region() */
#include <linux/list.h>		/* list_head */
#include <linux/memory.h>	/* memset */
#include <linux/io.h>		/* ioremap() */
#include <linux/clk.h>		/* clk_enable() */
#include <linux/platform_device.h>	/* platform_get_resource_byname() */
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "sps_bam.h"
#include "spsi.h"
#include "sps_core.h"

#define SPS_DRV_NAME "msm_sps"	/* must match the platform_device name */

/**
 *  SPS driver state
 */
struct sps_drv *sps;

u32 d_type;
bool enhd_pipe;
bool imem;
enum sps_bam_type bam_type;
enum sps_bam_type bam_types[] = {SPS_BAM_LEGACY, SPS_BAM_NDP, SPS_BAM_NDP_4K};

static void sps_device_de_init(void);

#ifdef CONFIG_DEBUG_FS
u8 debugfs_record_enabled;
u8 logging_option;
u8 debug_level_option;
u8 print_limit_option;
u8 reg_dump_option;
u32 testbus_sel;
u32 bam_pipe_sel;
u32 desc_option;
/**
 * Specifies range of log level from level 0 to level 3 to have fine-granularity for logging
 * to serve all BAM use cases.
 */
u32 log_level_sel;

static char *debugfs_buf;
static u32 debugfs_buf_size;
static u32 debugfs_buf_used;
static int wraparound;
static struct mutex sps_debugfs_lock;

struct dentry *dent;
struct dentry *dfile_info;
struct dentry *dfile_logging_option;
struct dentry *dfile_debug_level_option;
struct dentry *dfile_print_limit_option;
struct dentry *dfile_reg_dump_option;
struct dentry *dfile_testbus_sel;
struct dentry *dfile_bam_pipe_sel;
struct dentry *dfile_desc_option;
struct dentry *dfile_bam_addr;
struct dentry *dfile_log_level_sel;

static struct sps_bam *phy2bam(phys_addr_t phys_addr);

/* record debug info for debugfs */
void sps_debugfs_record(const char *msg)
{
	mutex_lock(&sps_debugfs_lock);
	if (debugfs_record_enabled) {
		if (debugfs_buf_used + MAX_MSG_LEN >= debugfs_buf_size) {
			debugfs_buf_used = 0;
			wraparound = true;
		}
		debugfs_buf_used += scnprintf(debugfs_buf + debugfs_buf_used,
				debugfs_buf_size - debugfs_buf_used, msg);

		if (wraparound)
			scnprintf(debugfs_buf + debugfs_buf_used,
					debugfs_buf_size - debugfs_buf_used,
					"\n**** end line of sps log ****\n\n");
	}
	mutex_unlock(&sps_debugfs_lock);
}

/* read the recorded debug info to userspace */
static ssize_t sps_read_info(struct file *file, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int ret = 0;
	int size;

	mutex_lock(&sps_debugfs_lock);
	if (debugfs_record_enabled) {
		if (wraparound)
			size = debugfs_buf_size - MAX_MSG_LEN;
		else
			size = debugfs_buf_used;

		ret = simple_read_from_buffer(ubuf, count, ppos,
				debugfs_buf, size);
	}
	mutex_unlock(&sps_debugfs_lock);

	return ret;
}

/*
 * set the buffer size (in KB) for debug info
 */
static ssize_t sps_set_info(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	unsigned long missing;
	char str[MAX_MSG_LEN];
	int i;
	u32 buf_size_kb = 0;
	u32 new_buf_size;
	u32 size = sizeof(str) < count ? sizeof(str) : count;

	memset(str, 0, sizeof(str));
	missing = copy_from_user(str, buf, size);
	if (missing)
		return -EFAULT;

	for (i = 0; i < sizeof(str) && (str[i] >= '0') && (str[i] <= '9'); ++i)
		buf_size_kb = (buf_size_kb * 10) + (str[i] - '0');

	pr_info("sps:debugfs: input buffer size is %dKB\n", buf_size_kb);

	if ((logging_option == 0) || (logging_option == 2)) {
		pr_info("sps:debugfs: need to first turn on recording.\n");
		return -EFAULT;
	}

	if (buf_size_kb < 1) {
		pr_info("sps:debugfs: buffer size should be "
			"no less than 1KB.\n");
		return -EFAULT;
	}

	if (buf_size_kb > (INT_MAX/SZ_1K)) {
		pr_err("sps:debugfs: buffer size is too large\n");
		return -EFAULT;
	}

	new_buf_size = buf_size_kb * SZ_1K;

	mutex_lock(&sps_debugfs_lock);
	if (debugfs_record_enabled) {
		if (debugfs_buf_size == new_buf_size) {
			/* need do nothing */
			pr_info("sps:debugfs: input buffer size "
				"is the same as before.\n");
			mutex_unlock(&sps_debugfs_lock);
			return count;
		} else {
			/* release the current buffer */
			debugfs_record_enabled = false;
			debugfs_buf_used = 0;
			wraparound = false;
			kfree(debugfs_buf);
			debugfs_buf = NULL;
		}
	}

	/* allocate new buffer */
	debugfs_buf_size = new_buf_size;

	debugfs_buf = kzalloc(sizeof(char) * debugfs_buf_size,
			GFP_KERNEL);
	if (!debugfs_buf) {
		debugfs_buf_size = 0;
		pr_err("sps:fail to allocate memory for debug_fs.\n");
		mutex_unlock(&sps_debugfs_lock);
		return -ENOMEM;
	}

	debugfs_buf_used = 0;
	wraparound = false;
	debugfs_record_enabled = true;
	mutex_unlock(&sps_debugfs_lock);

	return count;
}

const struct file_operations sps_info_ops = {
	.read = sps_read_info,
	.write = sps_set_info,
};

/* return the current logging option to userspace */
static ssize_t sps_read_logging_option(struct file *file, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	char value[MAX_MSG_LEN];
	int nbytes;

	nbytes = snprintf(value, MAX_MSG_LEN, "%d\n", logging_option);

	return simple_read_from_buffer(ubuf, count, ppos, value, nbytes);
}

/*
 * set the logging option
 */
static ssize_t sps_set_logging_option(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	unsigned long missing;
	char str[MAX_MSG_LEN];
	int i;
	u8 option = 0;
	u32 size = sizeof(str) < count ? sizeof(str) : count;

	memset(str, 0, sizeof(str));
	missing = copy_from_user(str, buf, size);
	if (missing)
		return -EFAULT;

	for (i = 0; i < sizeof(str) && (str[i] >= '0') && (str[i] <= '9'); ++i)
		option = (option * 10) + (str[i] - '0');

	pr_info("sps:debugfs: try to change logging option to %d\n", option);

	if (option > 3) {
		pr_err("sps:debugfs: invalid logging option:%d\n", option);
		return count;
	}

	mutex_lock(&sps_debugfs_lock);
	if (((option == 0) || (option == 2)) &&
		((logging_option == 1) || (logging_option == 3))) {
		debugfs_record_enabled = false;
		kfree(debugfs_buf);
		debugfs_buf = NULL;
		debugfs_buf_used = 0;
		debugfs_buf_size = 0;
		wraparound = false;
	}

	logging_option = option;
	mutex_unlock(&sps_debugfs_lock);

	return count;
}

const struct file_operations sps_logging_option_ops = {
	.read = sps_read_logging_option,
	.write = sps_set_logging_option,
};

/*
 * input the bam physical address
 */
static ssize_t sps_set_bam_addr(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	unsigned long missing;
	char str[MAX_MSG_LEN];
	u32 i;
	u32 bam_addr = 0;
	struct sps_bam *bam;
	u32 num_pipes = 0;
	void *vir_addr;
	u32 size = sizeof(str) < count ? sizeof(str) : count;

	memset(str, 0, sizeof(str));
	missing = copy_from_user(str, buf, size);
	if (missing)
		return -EFAULT;

	for (i = 0; i < sizeof(str) && (str[i] >= '0') && (str[i] <= '9'); ++i)
		bam_addr = (bam_addr * 10) + (str[i] - '0');

	pr_info("sps:debugfs:input BAM physical address:0x%x\n", bam_addr);

	bam = phy2bam(bam_addr);

	if (bam == NULL) {
		pr_err("sps:debugfs:BAM 0x%x is not registered.", bam_addr);
		return count;
	} else {
		vir_addr = &bam->base;
		num_pipes = bam->props.num_pipes;
		if (log_level_sel <= SPS_IPC_MAX_LOGLEVEL)
			bam->ipc_loglevel = log_level_sel;
	}

	switch (reg_dump_option) {
	case 1: /* output all registers of this BAM */
		print_bam_reg(bam->base);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_reg(bam->base, i);
		break;
	case 2: /* output BAM-level registers */
		print_bam_reg(bam->base);
		break;
	case 3: /* output selected BAM-level registers */
		print_bam_selected_reg(vir_addr, bam->props.ee);
		break;
	case 4: /* output selected registers of all pipes */
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 5: /* output selected registers of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 6: /* output selected registers of typical pipes */
		print_bam_pipe_selected_reg(vir_addr, 4);
		print_bam_pipe_selected_reg(vir_addr, 5);
		break;
	case 7: /* output desc FIFO of all pipes */
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_desc_fifo(vir_addr, i, 0);
		break;
	case 8: /* output desc FIFO of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
		break;
	case 9: /* output desc FIFO of typical pipes */
		print_bam_pipe_desc_fifo(vir_addr, 4, 0);
		print_bam_pipe_desc_fifo(vir_addr, 5, 0);
		break;
	case 10: /* output selected registers and desc FIFO of all pipes */
		for (i = 0; i < num_pipes; i++) {
			print_bam_pipe_selected_reg(vir_addr, i);
			print_bam_pipe_desc_fifo(vir_addr, i, 0);
		}
		break;
	case 11: /* output selected registers and desc FIFO of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
			}
		break;
	case 12: /* output selected registers and desc FIFO of typical pipes */
		print_bam_pipe_selected_reg(vir_addr, 4);
		print_bam_pipe_desc_fifo(vir_addr, 4, 0);
		print_bam_pipe_selected_reg(vir_addr, 5);
		print_bam_pipe_desc_fifo(vir_addr, 5, 0);
		break;
	case 13: /* output BAM_TEST_BUS_REG */
		if (testbus_sel)
			print_bam_test_bus_reg(vir_addr, testbus_sel);
		else {
			pr_info("sps:output TEST_BUS_REG for all TEST_BUS_SEL");
			print_bam_test_bus_reg(vir_addr, testbus_sel);
		}
		break;
	case 14: /* output partial desc FIFO of selected pipes */
		if (desc_option == 0)
			desc_option = 1;
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i,
							desc_option);
		break;
	case 15: /* output partial data blocks of descriptors */
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 100);
		break;
	case 16: /* output all registers of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_reg(bam->base, i);
		break;
	case 91: /* output testbus register, BAM global regisers
			and registers of all pipes */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 92: /* output testbus register, BAM global regisers
			and registers of selected pipes */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 93: /* output registers and partial desc FIFOs
			of selected pipes: format 1 */
		if (desc_option == 0)
			desc_option = 1;
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i,
							desc_option);
		break;
	case 94: /* output registers and partial desc FIFOs
			of selected pipes: format 2 */
		if (desc_option == 0)
			desc_option = 1;
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i,
							desc_option);
			}
		break;
	case 95: /* output registers and desc FIFOs
			of selected pipes: format 1 */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
		break;
	case 96: /* output registers and desc FIFOs
			of selected pipes: format 2 */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
			}
		break;
	case 97: /* output registers, desc FIFOs and partial data blocks
			of selected pipes: format 1 */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 100);
		break;
	case 98: /* output registers, desc FIFOs and partial data blocks
			of selected pipes: format 2 */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (bam_pipe_sel & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
				print_bam_pipe_desc_fifo(vir_addr, i, 100);
			}
		break;
	case 99: /* output all registers, desc FIFOs and partial data blocks */
		print_bam_test_bus_reg(vir_addr, testbus_sel);
		print_bam_reg(bam->base);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_reg(bam->base, i);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_desc_fifo(vir_addr, i, 0);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_desc_fifo(vir_addr, i, 100);
		break;
	default:
		pr_info("sps:no dump option is chosen yet.");
	}

	return count;
}

const struct file_operations sps_bam_addr_ops = {
	.write = sps_set_bam_addr,
};

static void sps_debugfs_init(void)
{
	debugfs_record_enabled = false;
	logging_option = 0;
	debug_level_option = 0;
	print_limit_option = 0;
	reg_dump_option = 0;
	testbus_sel = 0;
	bam_pipe_sel = 0;
	desc_option = 0;
	debugfs_buf_size = 0;
	debugfs_buf_used = 0;
	wraparound = false;
	log_level_sel = SPS_IPC_MAX_LOGLEVEL + 1;

	dent = debugfs_create_dir("sps", 0);
	if (IS_ERR(dent)) {
		pr_err("sps:fail to create the folder for debug_fs.\n");
		return;
	}

	dfile_info = debugfs_create_file("info", 0664, dent, 0,
			&sps_info_ops);
	if (!dfile_info || IS_ERR(dfile_info)) {
		pr_err("sps:fail to create the file for debug_fs info.\n");
		goto info_err;
	}

	dfile_logging_option = debugfs_create_file("logging_option", 0664,
			dent, 0, &sps_logging_option_ops);
	if (!dfile_logging_option || IS_ERR(dfile_logging_option)) {
		pr_err("sps:fail to create the file for debug_fs "
			"logging_option.\n");
		goto logging_option_err;
	}

	dfile_debug_level_option = debugfs_create_u8("debug_level_option",
					0664, dent, &debug_level_option);
	if (!dfile_debug_level_option || IS_ERR(dfile_debug_level_option)) {
		pr_err("sps:fail to create the file for debug_fs "
			"debug_level_option.\n");
		goto debug_level_option_err;
	}

	dfile_print_limit_option = debugfs_create_u8("print_limit_option",
					0664, dent, &print_limit_option);
	if (!dfile_print_limit_option || IS_ERR(dfile_print_limit_option)) {
		pr_err("sps:fail to create the file for debug_fs "
			"print_limit_option.\n");
		goto print_limit_option_err;
	}

	dfile_reg_dump_option = debugfs_create_u8("reg_dump_option", 0664,
						dent, &reg_dump_option);
	if (!dfile_reg_dump_option || IS_ERR(dfile_reg_dump_option)) {
		pr_err("sps:fail to create the file for debug_fs "
			"reg_dump_option.\n");
		goto reg_dump_option_err;
	}

	dfile_testbus_sel = debugfs_create_u32("testbus_sel", 0664,
						dent, &testbus_sel);
	if (!dfile_testbus_sel || IS_ERR(dfile_testbus_sel)) {
		pr_err("sps:fail to create debug_fs file for testbus_sel.\n");
		goto testbus_sel_err;
	}

	dfile_bam_pipe_sel = debugfs_create_u32("bam_pipe_sel", 0664,
						dent, &bam_pipe_sel);
	if (!dfile_bam_pipe_sel || IS_ERR(dfile_bam_pipe_sel)) {
		pr_err("sps:fail to create debug_fs file for bam_pipe_sel.\n");
		goto bam_pipe_sel_err;
	}

	dfile_desc_option = debugfs_create_u32("desc_option", 0664,
						dent, &desc_option);
	if (!dfile_desc_option || IS_ERR(dfile_desc_option)) {
		pr_err("sps:fail to create debug_fs file for desc_option.\n");
		goto desc_option_err;
	}

	dfile_bam_addr = debugfs_create_file("bam_addr", 0664,
			dent, 0, &sps_bam_addr_ops);
	if (!dfile_bam_addr || IS_ERR(dfile_bam_addr)) {
		pr_err("sps:fail to create the file for debug_fs "
			"bam_addr.\n");
		goto bam_addr_err;
	}

	dfile_log_level_sel = debugfs_create_u32("log_level_sel", 0664,
						dent, &log_level_sel);
	if (!dfile_log_level_sel || IS_ERR(dfile_log_level_sel)) {
		pr_err("sps:fail to create debug_fs file for log_level_sel.\n");
		goto bam_log_level_err;
	}

	mutex_init(&sps_debugfs_lock);

	return;

bam_log_level_err:
	debugfs_remove(dfile_bam_addr);
bam_addr_err:
	debugfs_remove(dfile_desc_option);
desc_option_err:
	debugfs_remove(dfile_bam_pipe_sel);
bam_pipe_sel_err:
	debugfs_remove(dfile_testbus_sel);
testbus_sel_err:
	debugfs_remove(dfile_reg_dump_option);
reg_dump_option_err:
	debugfs_remove(dfile_print_limit_option);
print_limit_option_err:
	debugfs_remove(dfile_debug_level_option);
debug_level_option_err:
	debugfs_remove(dfile_logging_option);
logging_option_err:
	debugfs_remove(dfile_info);
info_err:
	debugfs_remove(dent);
}

static void sps_debugfs_exit(void)
{
	if (dfile_info)
		debugfs_remove(dfile_info);
	if (dfile_logging_option)
		debugfs_remove(dfile_logging_option);
	if (dfile_debug_level_option)
		debugfs_remove(dfile_debug_level_option);
	if (dfile_print_limit_option)
		debugfs_remove(dfile_print_limit_option);
	if (dfile_reg_dump_option)
		debugfs_remove(dfile_reg_dump_option);
	if (dfile_testbus_sel)
		debugfs_remove(dfile_testbus_sel);
	if (dfile_bam_pipe_sel)
		debugfs_remove(dfile_bam_pipe_sel);
	if (dfile_desc_option)
		debugfs_remove(dfile_desc_option);
	if (dfile_bam_addr)
		debugfs_remove(dfile_bam_addr);
	if (dent)
		debugfs_remove(dent);
	debugfs_remove(dfile_log_level_sel);
	kfree(debugfs_buf);
	debugfs_buf = NULL;
}
#endif

/* Get the debug info of BAM registers and descriptor FIFOs */
int sps_get_bam_debug_info(unsigned long dev, u32 option, u32 para,
		u32 tb_sel, u32 desc_sel)
{
	int res = 0;
	struct sps_bam *bam;
	u32 i;
	u32 num_pipes = 0;
	void *vir_addr;

	if (dev == 0) {
		SPS_ERR(sps,
			"sps:%s:device handle should not be 0.\n", __func__);
		return SPS_ERROR;
	}

	if (sps == NULL || !sps->is_ready) {
		SPS_DBG3(sps, "sps:%s:sps driver is not ready.\n", __func__);
		return -EPROBE_DEFER;
	}

	mutex_lock(&sps->lock);
	/* Search for the target BAM device */
	bam = sps_h2bam(dev);
	if (bam == NULL) {
		pr_err("sps:Can't find any BAM with handle 0x%pK.",
					(void *)dev);
		mutex_unlock(&sps->lock);
		return SPS_ERROR;
	}
	mutex_unlock(&sps->lock);

	vir_addr = &bam->base;
	num_pipes = bam->props.num_pipes;

	SPS_DUMP("sps:<bam-addr> dump BAM:%pa.\n", &bam->props.phys_addr);

	switch (option) {
	case 1: /* output all registers of this BAM */
		print_bam_reg(bam->base);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_reg(bam->base, i);
		break;
	case 2: /* output BAM-level registers */
		print_bam_reg(bam->base);
		break;
	case 3: /* output selected BAM-level registers */
		print_bam_selected_reg(vir_addr, bam->props.ee);
		break;
	case 4: /* output selected registers of all pipes */
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 5: /* output selected registers of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 6: /* output selected registers of typical pipes */
		print_bam_pipe_selected_reg(vir_addr, 4);
		print_bam_pipe_selected_reg(vir_addr, 5);
		break;
	case 7: /* output desc FIFO of all pipes */
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_desc_fifo(vir_addr, i, 0);
		break;
	case 8: /* output desc FIFO of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
		break;
	case 9: /* output desc FIFO of typical pipes */
		print_bam_pipe_desc_fifo(vir_addr, 4, 0);
		print_bam_pipe_desc_fifo(vir_addr, 5, 0);
		break;
	case 10: /* output selected registers and desc FIFO of all pipes */
		for (i = 0; i < num_pipes; i++) {
			print_bam_pipe_selected_reg(vir_addr, i);
			print_bam_pipe_desc_fifo(vir_addr, i, 0);
		}
		break;
	case 11: /* output selected registers and desc FIFO of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
			}
		break;
	case 12: /* output selected registers and desc FIFO of typical pipes */
		print_bam_pipe_selected_reg(vir_addr, 4);
		print_bam_pipe_desc_fifo(vir_addr, 4, 0);
		print_bam_pipe_selected_reg(vir_addr, 5);
		print_bam_pipe_desc_fifo(vir_addr, 5, 0);
		break;
	case 13: /* output BAM_TEST_BUS_REG */
		if (tb_sel)
			print_bam_test_bus_reg(vir_addr, tb_sel);
		else
			pr_info("sps:TEST_BUS_SEL should NOT be zero.");
		break;
	case 14: /* output partial desc FIFO of selected pipes */
		if (desc_sel == 0)
			desc_sel = 1;
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i,
							desc_sel);
		break;
	case 15: /* output partial data blocks of descriptors */
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 100);
		break;
	case 16: /* output all registers of selected pipes */
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_reg(bam->base, i);
		break;
	case 91: /* output testbus register, BAM global regisers
			and registers of all pipes */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 92: /* output testbus register, BAM global regisers
			and registers of selected pipes */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		break;
	case 93: /* output registers and partial desc FIFOs
			of selected pipes: format 1 */
		if (desc_sel == 0)
			desc_sel = 1;
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i,
							desc_sel);
		break;
	case 94: /* output registers and partial desc FIFOs
			of selected pipes: format 2 */
		if (desc_sel == 0)
			desc_sel = 1;
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i,
							desc_sel);
			}
		break;
	case 95: /* output registers and desc FIFOs
			of selected pipes: format 1 */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
		break;
	case 96: /* output registers and desc FIFOs
			of selected pipes: format 2 */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
			}
		break;
	case 97: /* output registers, desc FIFOs and partial data blocks
			of selected pipes: format 1 */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i))
				print_bam_pipe_desc_fifo(vir_addr, i, 100);
		break;
	case 98: /* output registers, desc FIFOs and partial data blocks
			of selected pipes: format 2 */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			if (para & (1UL << i)) {
				print_bam_pipe_selected_reg(vir_addr, i);
				print_bam_pipe_desc_fifo(vir_addr, i, 0);
				print_bam_pipe_desc_fifo(vir_addr, i, 100);
			}
		break;
	case 99: /* output all registers, desc FIFOs and partial data blocks */
		print_bam_test_bus_reg(vir_addr, tb_sel);
		print_bam_reg(bam->base);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_reg(bam->base, i);
		print_bam_selected_reg(vir_addr, bam->props.ee);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_selected_reg(vir_addr, i);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_desc_fifo(vir_addr, i, 0);
		for (i = 0; i < num_pipes; i++)
			print_bam_pipe_desc_fifo(vir_addr, i, 100);
		break;
	default:
		pr_info("sps:no option is chosen yet.");
	}

	return res;
}
EXPORT_SYMBOL(sps_get_bam_debug_info);

/**
 * Initialize SPS device
 *
 * This function initializes the SPS device.
 *
 * @return 0 on success, negative value on error
 *
 */
static int sps_device_init(void)
{
	int result;
	int success;
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	struct sps_bam_props bamdma_props = {0};
#endif

	SPS_DBG3(sps, "sps:%s.", __func__);

	success = false;

	result = sps_mem_init(sps->pipemem_phys_base, sps->pipemem_size);
	if (result) {
		SPS_ERR(sps, "sps:%s:SPS memory init failed", __func__);
		goto exit_err;
	}

	INIT_LIST_HEAD(&sps->bams_q);
	mutex_init(&sps->lock);

	if (sps_rm_init(&sps->connection_ctrl, sps->options)) {
		SPS_ERR(sps, "sps:%s:Fail to init SPS resource manager",
				__func__);
		goto exit_err;
	}

	result = sps_bam_driver_init(sps->options);
	if (result) {
		SPS_ERR(sps, "sps:%s:SPS BAM driver init failed", __func__);
		goto exit_err;
	}

	/* Initialize the BAM DMA device */
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	bamdma_props.phys_addr = sps->bamdma_bam_phys_base;
	bamdma_props.virt_addr = ioremap(sps->bamdma_bam_phys_base,
					 sps->bamdma_bam_size);

	if (!bamdma_props.virt_addr) {
		SPS_ERR(sps, "sps:%s:Fail to IO map BAM-DMA BAM registers.\n",
				__func__);
		goto exit_err;
	}

	SPS_DBG3(sps, "sps:bamdma_bam.phys=%pa.virt=0x%pK.",
		&bamdma_props.phys_addr,
		bamdma_props.virt_addr);

	bamdma_props.periph_phys_addr =	sps->bamdma_dma_phys_base;
	bamdma_props.periph_virt_size = sps->bamdma_dma_size;
	bamdma_props.periph_virt_addr = ioremap(sps->bamdma_dma_phys_base,
						sps->bamdma_dma_size);

	if (!bamdma_props.periph_virt_addr) {
		SPS_ERR(sps, "sps:%s:Fail to IO map BAM-DMA peripheral reg.\n",
				__func__);
		goto exit_err;
	}

	SPS_DBG3(sps, "sps:bamdma_dma.phys=%pa.virt=0x%pK.",
		&bamdma_props.periph_phys_addr,
		bamdma_props.periph_virt_addr);

	bamdma_props.irq = sps->bamdma_irq;

	bamdma_props.event_threshold = 0x10;	/* Pipe event threshold */
	bamdma_props.summing_threshold = 0x10;	/* BAM event threshold */

	bamdma_props.options = SPS_BAM_OPT_BAMDMA;
	bamdma_props.restricted_pipes =	sps->bamdma_restricted_pipes;

	result = sps_dma_init(&bamdma_props);
	if (result) {
		SPS_ERR(sps, "sps:%s:SPS BAM DMA driver init failed", __func__);
		goto exit_err;
	}
#endif /* CONFIG_SPS_SUPPORT_BAMDMA */

	result = sps_map_init(NULL, sps->options);
	if (result) {
		SPS_ERR(sps,
			"sps:%s:SPS connection mapping init failed", __func__);
		goto exit_err;
	}

	success = true;
exit_err:
	if (!success) {
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
		sps_device_de_init();
#endif
		return SPS_ERROR;
	}

	return 0;
}

/**
 * De-initialize SPS device
 *
 * This function de-initializes the SPS device.
 *
 * @return 0 on success, negative value on error
 *
 */
static void sps_device_de_init(void)
{
	SPS_DBG3(sps, "sps:%s.", __func__);

	if (sps != NULL) {
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
		sps_dma_de_init();
#endif
		/* Are there any remaining BAM registrations? */
		if (!list_empty(&sps->bams_q))
			SPS_ERR(sps,
				"sps:%s:BAMs are still registered", __func__);

		sps_map_de_init();
	}

	sps_mem_de_init();
}

/**
 * Initialize client state context
 *
 * This function initializes a client state context struct.
 *
 * @client - Pointer to client state context
 *
 * @return 0 on success, negative value on error
 *
 */
static int sps_client_init(struct sps_pipe *client)
{
	SPS_DBG(sps, "sps:%s.", __func__);

	if (client == NULL)
		return -EINVAL;

	/*
	 * NOTE: Cannot store any state within the SPS driver because
	 * the driver init function may not have been called yet.
	 */
	memset(client, 0, sizeof(*client));
	sps_rm_config_init(&client->connect);

	client->client_state = SPS_STATE_DISCONNECT;
	client->bam = NULL;

	return 0;
}

/**
 * De-initialize client state context
 *
 * This function de-initializes a client state context struct.
 *
 * @client - Pointer to client state context
 *
 * @return 0 on success, negative value on error
 *
 */
static int sps_client_de_init(struct sps_pipe *client)
{
	SPS_DBG(sps, "sps:%s.", __func__);

	if (client->client_state != SPS_STATE_DISCONNECT) {
		SPS_ERR(sps, "sps:De-init client in connected state: 0x%x",
				   client->client_state);
		return SPS_ERROR;
	}

	client->bam = NULL;
	client->map = NULL;
	memset(&client->connect, 0, sizeof(client->connect));

	return 0;
}

/**
 * Find the BAM device from the physical address
 *
 * This function finds a BAM device in the BAM registration list that
 * matches the specified physical address.
 *
 * @phys_addr - physical address of the BAM
 *
 * @return - pointer to the BAM device struct, or NULL on error
 *
 */
static struct sps_bam *phy2bam(phys_addr_t phys_addr)
{
	struct sps_bam *bam;

	SPS_DBG2(sps, "sps:%s.", __func__);

	list_for_each_entry(bam, &sps->bams_q, list) {
		if (bam->props.phys_addr == phys_addr)
			return bam;
	}

	return NULL;
}

/**
 * Find the handle of a BAM device based on the physical address
 *
 * This function finds a BAM device in the BAM registration list that
 * matches the specified physical address, and returns its handle.
 *
 * @phys_addr - physical address of the BAM
 *
 * @h - device handle of the BAM
 *
 * @return 0 on success, negative value on error
 *
 */
int sps_phy2h(phys_addr_t phys_addr, unsigned long *handle)
{
	struct sps_bam *bam;

	SPS_DBG2(sps, "sps:%s.", __func__);

	if (sps == NULL || !sps->is_ready) {
		SPS_DBG3(sps, "sps:%s:sps driver is not ready.\n", __func__);
		return -EPROBE_DEFER;
	}

	if (handle == NULL) {
		SPS_ERR(sps, "sps:%s:handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	list_for_each_entry(bam, &sps->bams_q, list) {
		if (bam->props.phys_addr == phys_addr) {
			*handle = (uintptr_t) bam;
			return 0;
		}
	}

	SPS_ERR(sps,
		"sps: BAM device %pa is not registered yet.\n", &phys_addr);

	return -ENODEV;
}
EXPORT_SYMBOL(sps_phy2h);

/**
 * Setup desc/data FIFO for bam-to-bam connection
 *
 * @mem_buffer - Pointer to struct for allocated memory properties.
 *
 * @addr - address of FIFO
 *
 * @size - FIFO size
 *
 * @use_offset - use address offset instead of absolute address
 *
 * @return 0 on success, negative value on error
 *
 */
int sps_setup_bam2bam_fifo(struct sps_mem_buffer *mem_buffer,
		  u32 addr, u32 size, int use_offset)
{
	SPS_DBG1(sps, "sps:%s.", __func__);

	if ((mem_buffer == NULL) || (size == 0)) {
		SPS_ERR(sps, "sps:%s:invalid buffer address or size.",
				__func__);
		return SPS_ERROR;
	}

	if (sps == NULL || !sps->is_ready) {
		SPS_DBG3(sps, "sps:%s:sps driver is not ready.\n", __func__);
		return -EPROBE_DEFER;
	}

	if (use_offset) {
		if ((addr + size) <= sps->pipemem_size)
			mem_buffer->phys_base = sps->pipemem_phys_base + addr;
		else {
			SPS_ERR(sps,
				"sps:%s:requested mem is out of pipe mem range.\n",
				__func__);
			return SPS_ERROR;
		}
	} else {
		if (addr >= sps->pipemem_phys_base &&
			(addr + size) <= (sps->pipemem_phys_base
						+ sps->pipemem_size))
			mem_buffer->phys_base = addr;
		else {
			SPS_ERR(sps,
				"sps:%s:requested mem is out of pipe mem range.\n",
				__func__);
			return SPS_ERROR;
		}
	}

	mem_buffer->base = spsi_get_mem_ptr(mem_buffer->phys_base);
	mem_buffer->size = size;

	memset(mem_buffer->base, 0, mem_buffer->size);

	return 0;
}
EXPORT_SYMBOL(sps_setup_bam2bam_fifo);

/**
 * Find the BAM device from the handle
 *
 * This function finds a BAM device in the BAM registration list that
 * matches the specified device handle.
 *
 * @h - device handle of the BAM
 *
 * @return - pointer to the BAM device struct, or NULL on error
 *
 */
struct sps_bam *sps_h2bam(unsigned long h)
{
	struct sps_bam *bam;

	SPS_DBG1(sps, "sps:%s: BAM handle:0x%pK.", __func__, (void *)h);

	if (h == SPS_DEV_HANDLE_MEM || h == SPS_DEV_HANDLE_INVALID)
		return NULL;

	list_for_each_entry(bam, &sps->bams_q, list) {
		if ((uintptr_t) bam == h)
			return bam;
	}

	SPS_ERR(sps, "sps:Can't find BAM device for handle 0x%pK.", (void *)h);

	return NULL;
}

/**
 * Lock BAM device
 *
 * This function obtains the BAM spinlock on the client's connection.
 *
 * @pipe - pointer to client pipe state
 *
 * @return pointer to BAM device struct, or NULL on error
 *
 */
static struct sps_bam *sps_bam_lock(struct sps_pipe *pipe)
{
	struct sps_bam *bam;
	u32 pipe_index;

	bam = pipe->bam;
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:Connection is not in connected state.",
				__func__);
		return NULL;
	}

	spin_lock_irqsave(&bam->connection_lock, bam->irqsave_flags);

	/* Verify client owns this pipe */
	pipe_index = pipe->pipe_index;
	if (pipe_index >= bam->props.num_pipes ||
	    pipe != bam->pipes[pipe_index]) {
		SPS_ERR(bam,
			"sps:Client not owner of BAM %pa pipe: %d (max %d)",
			&bam->props.phys_addr, pipe_index,
			bam->props.num_pipes);
		spin_unlock_irqrestore(&bam->connection_lock,
						bam->irqsave_flags);
		return NULL;
	}

	return bam;
}

/**
 * Unlock BAM device
 *
 * This function releases the BAM spinlock on the client's connection.
 *
 * @bam - pointer to BAM device struct
 *
 */
static inline void sps_bam_unlock(struct sps_bam *bam)
{
	spin_unlock_irqrestore(&bam->connection_lock, bam->irqsave_flags);
}

/**
 * Connect an SPS connection end point
 *
 */
int sps_connect(struct sps_pipe *h, struct sps_connect *connect)
{
	struct sps_pipe *pipe = h;
	unsigned long dev;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (connect == NULL) {
		SPS_ERR(sps, "sps:%s:connection is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (sps == NULL)
		return -ENODEV;

	if (!sps->is_ready) {
		SPS_ERR(sps, "sps:%s:sps driver is not ready.\n", __func__);
		return -EAGAIN;
	}

	if ((connect->lock_group != SPSRM_CLEAR)
		&& (connect->lock_group > BAM_MAX_P_LOCK_GROUP_NUM)) {
		SPS_ERR(sps,
			"sps:%s:The value of pipe lock group is invalid.\n",
			__func__);
		return SPS_ERROR;
	}

	mutex_lock(&sps->lock);
	/*
	 * Must lock the BAM device at the top level function, so must
	 * determine which BAM is the target for the connection
	 */
	if (connect->mode == SPS_MODE_SRC)
		dev = connect->source;
	else
		dev = connect->destination;

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:Invalid BAM device handle: 0x%pK",
					(void *)dev);
		result = SPS_ERROR;
		goto exit_err;
	}

	mutex_lock(&bam->lock);
	SPS_DBG2(bam, "sps:sps_connect: bam %pa src 0x%pK dest 0x%pK mode %s",
			BAM_ID(bam),
			(void *)connect->source,
			(void *)connect->destination,
			connect->mode == SPS_MODE_SRC ? "SRC" : "DEST");

	/* Allocate resources for the specified connection */
	pipe->connect = *connect;
	result = sps_rm_state_change(pipe, SPS_STATE_ALLOCATE);
	if (result) {
		mutex_unlock(&bam->lock);
		goto exit_err;
	}

	/* Configure the connection */
	result = sps_rm_state_change(pipe, SPS_STATE_CONNECT);
	mutex_unlock(&bam->lock);
	if (result) {
		sps_disconnect(h);
		goto exit_err;
	}

exit_err:
	mutex_unlock(&sps->lock);

	return result;
}
EXPORT_SYMBOL(sps_connect);

/**
 * Disconnect an SPS connection end point
 *
 * This function disconnects an SPS connection end point.
 * The SPS hardware associated with that end point will be disabled.
 * For a connection involving system memory (SPS_DEV_HANDLE_MEM), all
 * connection resources are deallocated.  For a peripheral-to-peripheral
 * connection, the resources associated with the connection will not be
 * deallocated until both end points are closed.
 *
 * The client must call sps_connect() for the handle before calling
 * this function.
 *
 * @h - client context for SPS connection end point
 *
 * @return 0 on success, negative value on error
 *
 */
int sps_disconnect(struct sps_pipe *h)
{
	struct sps_pipe *pipe = h;
	struct sps_pipe *check;
	struct sps_bam *bam;
	int result;

	if (pipe == NULL) {
		SPS_ERR(sps, "sps:%s:Invalid pipe.", __func__);
		return SPS_ERROR;
	}

	bam = pipe->bam;
	if (bam == NULL) {
		SPS_ERR(sps,
			"sps:%s:BAM device of this pipe is NULL.", __func__);
		return SPS_ERROR;
	}

	SPS_DBG2(bam,
		"sps:sps_disconnect: bam %pa src 0x%pK dest 0x%pK mode %s",
		BAM_ID(bam),
		(void *)pipe->connect.source,
		(void *)pipe->connect.destination,
		pipe->connect.mode == SPS_MODE_SRC ? "SRC" : "DEST");

	result = SPS_ERROR;
	/* Cross-check client with map table */
	if (pipe->connect.mode == SPS_MODE_SRC)
		check = pipe->map->client_src;
	else
		check = pipe->map->client_dest;

	if (check != pipe) {
		SPS_ERR(sps, "sps:%s:Client context is corrupt", __func__);
		goto exit_err;
	}

	/* Disconnect the BAM pipe */
	mutex_lock(&bam->lock);
	result = sps_rm_state_change(pipe, SPS_STATE_DISCONNECT);
	mutex_unlock(&bam->lock);
	if (result)
		goto exit_err;

	sps_rm_config_init(&pipe->connect);
	result = 0;

exit_err:

	return result;
}
EXPORT_SYMBOL(sps_disconnect);

/**
 * Register an event object for an SPS connection end point
 *
 */
int sps_register_event(struct sps_pipe *h, struct sps_register_event *reg)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (reg == NULL) {
		SPS_ERR(sps, "sps:%s:registered event is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (sps == NULL)
		return -ENODEV;

	if (!sps->is_ready) {
		SPS_ERR(sps, "sps:%s:sps driver not ready.\n", __func__);
		return -EAGAIN;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG2(bam, "sps:%s; events:%d.\n", __func__, reg->options);

	result = sps_bam_pipe_reg_event(bam, pipe->pipe_index, reg);
	sps_bam_unlock(bam);
	if (result)
		SPS_ERR(bam,
			"sps:Fail to register event for BAM %pa pipe %d",
			&pipe->bam->props.phys_addr, pipe->pipe_index);

	return result;
}
EXPORT_SYMBOL(sps_register_event);

/**
 * Enable an SPS connection end point
 *
 */
int sps_flow_on(struct sps_pipe *h)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result = 0;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG2(bam, "sps:%s.\n", __func__);

	bam_pipe_halt(&bam->base, pipe->pipe_index, false);

	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_flow_on);

/**
 * Disable an SPS connection end point
 *
 */
int sps_flow_off(struct sps_pipe *h, enum sps_flow_off mode)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result = 0;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG2(bam, "sps:%s.\n", __func__);

	bam_pipe_halt(&bam->base, pipe->pipe_index, true);

	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_flow_off);

/**
 * Check if the flags on a descriptor/iovec are valid
 *
 * @flags - flags on a descriptor/iovec
 *
 * @return 0 on success, negative value on error
 *
 */
static int sps_check_iovec_flags(u32 flags)
{
	if ((flags & SPS_IOVEC_FLAG_NWD) &&
		!(flags & (SPS_IOVEC_FLAG_EOT | SPS_IOVEC_FLAG_CMD))) {
		SPS_ERR(sps,
			"sps:%s:NWD is only valid with EOT or CMD.\n",
			__func__);
		return SPS_ERROR;
	} else if ((flags & SPS_IOVEC_FLAG_EOT) &&
		(flags & SPS_IOVEC_FLAG_CMD)) {
		SPS_ERR(sps,
			"sps:%s:EOT and CMD are not allowed to coexist.\n",
			__func__);
		return SPS_ERROR;
	} else if (!(flags & SPS_IOVEC_FLAG_CMD) &&
		(flags & (SPS_IOVEC_FLAG_LOCK | SPS_IOVEC_FLAG_UNLOCK))) {
		static char err_msg[] =
		"pipe lock/unlock flags are only valid with Command Descriptor";
		SPS_ERR(sps, "sps:%s.\n", err_msg);
		return SPS_ERROR;
	} else if ((flags & SPS_IOVEC_FLAG_LOCK) &&
		(flags & SPS_IOVEC_FLAG_UNLOCK)) {
		static char err_msg[] =
		"Can't lock and unlock a pipe by the same Command Descriptor";
		SPS_ERR(sps, "sps:%s.\n", err_msg);
		return SPS_ERROR;
	} else if ((flags & SPS_IOVEC_FLAG_IMME) &&
		(flags & SPS_IOVEC_FLAG_CMD)) {
		SPS_ERR(sps,
			"sps:%s:Immediate and CMD are not allowed to coexist.\n",
			__func__);
		return SPS_ERROR;
	} else if ((flags & SPS_IOVEC_FLAG_IMME) &&
		(flags & SPS_IOVEC_FLAG_NWD)) {
		SPS_ERR(sps,
			"sps:%s:Immediate and NWD are not allowed to coexist.\n",
			__func__);
		return SPS_ERROR;
	}

	return 0;
}

/**
 * Perform a DMA transfer on an SPS connection end point
 *
 */
int sps_transfer(struct sps_pipe *h, struct sps_transfer *transfer)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;
	struct sps_iovec *iovec;
	int i;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (transfer == NULL) {
		SPS_ERR(sps, "sps:%s:transfer is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (transfer->iovec == NULL) {
		SPS_ERR(sps, "sps:%s:iovec list is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (transfer->iovec_count == 0) {
		SPS_ERR(sps, "sps:%s:iovec list is empty.\n", __func__);
		return SPS_ERROR;
	}

	/* Verify content of IOVECs */
	iovec = transfer->iovec;
	for (i = 0; i < transfer->iovec_count; i++) {
		u32 flags = iovec->flags;

		if (iovec->size > SPS_IOVEC_MAX_SIZE) {
			SPS_ERR(sps,
				"sps:%s:iovec size is invalid.\n", __func__);
			return SPS_ERROR;
		}

		if (sps_check_iovec_flags(flags))
			return SPS_ERROR;

		iovec++;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG(bam, "sps:%s.\n", __func__);

	result = sps_bam_pipe_transfer(bam, pipe->pipe_index, transfer);

	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_transfer);

/**
 * Perform a single DMA transfer on an SPS connection end point
 *
 */
int sps_transfer_one(struct sps_pipe *h, phys_addr_t addr, u32 size,
		     void *user, u32 flags)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (sps_check_iovec_flags(flags))
		return SPS_ERROR;

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG(bam, "sps:%s.\n", __func__);

	result = sps_bam_pipe_transfer_one(bam, pipe->pipe_index,
				SPS_GET_LOWER_ADDR(addr), size, user,
				DESC_FLAG_WORD(flags, addr));

	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_transfer_one);

/**
 * Read event queue for an SPS connection end point
 *
 */
int sps_get_event(struct sps_pipe *h, struct sps_event_notify *notify)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (notify == NULL) {
		SPS_ERR(sps, "sps:%s:event_notify is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG1(bam, "sps:%s.\n", __func__);

	result = sps_bam_pipe_get_event(bam, pipe->pipe_index, notify);
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_get_event);

/**
 * Determine whether an SPS connection end point FIFO is empty
 *
 */
int sps_is_pipe_empty(struct sps_pipe *h, u32 *empty)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (empty == NULL) {
		SPS_ERR(sps, "sps:%s:result pointer is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG1(bam, "sps:%s.\n", __func__);

	result = sps_bam_pipe_is_empty(bam, pipe->pipe_index, empty);
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_is_pipe_empty);

/**
 * Get number of free transfer entries for an SPS connection end point
 *
 */
int sps_get_free_count(struct sps_pipe *h, u32 *count)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (count == NULL) {
		SPS_ERR(sps, "sps:%s:result pointer is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG(bam, "sps:%s.\n", __func__);

	result = sps_bam_get_free_count(bam, pipe->pipe_index, count);
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_get_free_count);

/**
 * Reset an SPS BAM device
 *
 */
int sps_device_reset(unsigned long dev)
{
	struct sps_bam *bam;
	int result;

	if (dev == 0) {
		SPS_ERR(sps,
			"sps:%s:device handle should not be 0.\n", __func__);
		return SPS_ERROR;
	}

	if (sps == NULL || !sps->is_ready) {
		SPS_DBG3(sps, "sps:%s:sps driver is not ready.\n", __func__);
		return -EPROBE_DEFER;
	}

	mutex_lock(&sps->lock);
	/* Search for the target BAM device */
	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:Invalid BAM device handle: 0x%pK",
					(void *)dev);
		result = SPS_ERROR;
		goto exit_err;
	}

	SPS_DBG3(bam, "sps:%s.\n", __func__);

	mutex_lock(&bam->lock);
	result = sps_bam_reset(bam);
	mutex_unlock(&bam->lock);
	if (result) {
		SPS_ERR(sps, "sps:Fail to reset BAM device: 0x%pK",
					(void *)dev);
		goto exit_err;
	}

exit_err:
	mutex_unlock(&sps->lock);

	return result;
}
EXPORT_SYMBOL(sps_device_reset);

/**
 * Get the configuration parameters for an SPS connection end point
 *
 */
int sps_get_config(struct sps_pipe *h, struct sps_connect *config)
{
	struct sps_pipe *pipe = h;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (config == NULL) {
		SPS_ERR(sps, "sps:%s:config pointer is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (pipe->bam == NULL)
		SPS_DBG(sps, "sps:%s.\n", __func__);
	else
		SPS_DBG(pipe->bam,
			"sps:%s; BAM: %pa; pipe index:%d; options:0x%x.\n",
			__func__, BAM_ID(pipe->bam), pipe->pipe_index,
			pipe->connect.options);

	/* Copy current client connection state */
	*config = pipe->connect;

	return 0;
}
EXPORT_SYMBOL(sps_get_config);

/**
 * Set the configuration parameters for an SPS connection end point
 *
 */
int sps_set_config(struct sps_pipe *h, struct sps_connect *config)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (config == NULL) {
		SPS_ERR(sps, "sps:%s:config pointer is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is NULL.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d, config-options:0x%x.\n",
		__func__, BAM_ID(bam), pipe->pipe_index, config->options);

	result = sps_bam_pipe_set_params(bam, pipe->pipe_index,
					 config->options);
	if (result == 0)
		pipe->connect.options = config->options;
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_set_config);

/**
 * Set ownership of an SPS connection end point
 *
 */
int sps_set_owner(struct sps_pipe *h, enum sps_owner owner,
		  struct sps_satellite *connect)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (connect == NULL) {
		SPS_ERR(sps, "sps:%s:connection is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (owner != SPS_OWNER_REMOTE) {
		SPS_ERR(sps, "sps:Unsupported ownership state: %d", owner);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe->pipe_index);

	result = sps_bam_set_satellite(bam, pipe->pipe_index);
	if (result)
		goto exit_err;

	/* Return satellite connect info */
	if (connect == NULL)
		goto exit_err;

	if (pipe->connect.mode == SPS_MODE_SRC) {
		connect->dev = pipe->map->src.bam_phys;
		connect->pipe_index = pipe->map->src.pipe_index;
	} else {
		connect->dev = pipe->map->dest.bam_phys;
		connect->pipe_index = pipe->map->dest.pipe_index;
	}
	connect->config = SPS_CONFIG_SATELLITE;
	connect->options = (enum sps_option) 0;

exit_err:
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_set_owner);

/**
 * Allocate memory from the SPS Pipe-Memory.
 *
 */
int sps_alloc_mem(struct sps_pipe *h, enum sps_mem mem,
		  struct sps_mem_buffer *mem_buffer)
{
	if (sps == NULL)
		return -ENODEV;

	if (!sps->is_ready) {
		SPS_ERR(sps, "sps:%s:sps driver is not ready.", __func__);
		return -EAGAIN;
	}

	if (mem_buffer == NULL || mem_buffer->size == 0) {
		SPS_ERR(sps, "sps:%s:invalid memory buffer address or size",
				__func__);
		return SPS_ERROR;
	}

	if (h == NULL)
		SPS_DBG2(sps,
			"sps:%s:allocate pipe memory before setup pipe",
			__func__);
	else
		SPS_DBG2(sps,
			"sps:allocate pipe memory for pipe %d", h->pipe_index);

	mem_buffer->phys_base = sps_mem_alloc_io(mem_buffer->size);
	if (mem_buffer->phys_base == SPS_ADDR_INVALID) {
		SPS_ERR(sps, "sps:%s:invalid address of allocated memory",
				__func__);
		return SPS_ERROR;
	}

	mem_buffer->base = spsi_get_mem_ptr(mem_buffer->phys_base);

	return 0;
}
EXPORT_SYMBOL(sps_alloc_mem);

/**
 * Free memory from the SPS Pipe-Memory.
 *
 */
int sps_free_mem(struct sps_pipe *h, struct sps_mem_buffer *mem_buffer)
{
	SPS_DBG(sps, "sps:%s.", __func__);

	if (mem_buffer == NULL || mem_buffer->phys_base == SPS_ADDR_INVALID) {
		SPS_ERR(sps, "sps:%s:invalid memory to free", __func__);
		return SPS_ERROR;
	}

	if (h == NULL)
		SPS_DBG2(sps, "sps:%s:free pipe memory.", __func__);
	else
		SPS_DBG2(sps,
			"sps:free pipe memory for pipe %d.", h->pipe_index);

	sps_mem_free_io(mem_buffer->phys_base, mem_buffer->size);

	return 0;
}
EXPORT_SYMBOL(sps_free_mem);

/**
 * Get the number of unused descriptors in the descriptor FIFO
 * of a pipe
 *
 */
int sps_get_unused_desc_num(struct sps_pipe *h, u32 *desc_num)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (desc_num == NULL) {
		SPS_ERR(sps, "sps:%s:result pointer is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL)
		return SPS_ERROR;

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe->pipe_index);

	result = sps_bam_pipe_get_unused_desc_num(bam, pipe->pipe_index,
						desc_num);

	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_get_unused_desc_num);

/**
 * Vote for or relinquish BAM DMA clock
 *
 */
int sps_ctrl_bam_dma_clk(bool clk_on)
{
	int ret;

	if (sps == NULL || !sps->is_ready) {
		SPS_DBG3(sps, "sps:%s:sps driver is not ready.\n", __func__);
		return -EPROBE_DEFER;
	}

	if (clk_on == true) {
		SPS_DBG1(sps, "%s", "sps:vote for bam dma clk.\n");
		ret = clk_prepare_enable(sps->bamdma_clk);
		if (ret) {
			SPS_ERR(sps,
				"sps:fail to enable bamdma_clk:ret=%d\n", ret);
			return ret;
		}
	} else {
		SPS_DBG1(sps, "%s", "sps:relinquish bam dma clk.\n");
		clk_disable_unprepare(sps->bamdma_clk);
	}

	return 0;
}
EXPORT_SYMBOL(sps_ctrl_bam_dma_clk);

/**
 * Register a BAM device
 *
 */
int sps_register_bam_device(const struct sps_bam_props *bam_props,
				unsigned long *dev_handle)
{
	struct sps_bam *bam = NULL;
	void *virt_addr = NULL;
	char bam_name[MAX_MSG_LEN];
	u32 manage;
	int ok;
	int result;

	if (bam_props == NULL) {
		SPS_ERR(sps, "sps:%s:bam_props is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (dev_handle == NULL) {
		SPS_ERR(sps, "sps:%s:device handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (sps == NULL) {
		pr_err("sps:%s:sps driver is not ready.\n", __func__);
		return -EPROBE_DEFER;
	}

	SPS_DBG3(sps, "sps:%s: Client requests to register BAM %pa.\n",
		__func__, &bam_props->phys_addr);

	/* BAM-DMA is registered internally during power-up */
	if ((!sps->is_ready) && !(bam_props->options & SPS_BAM_OPT_BAMDMA)) {
		SPS_ERR(sps, "sps:%s:sps driver not ready.\n", __func__);
		return -EAGAIN;
	}

	/* Check BAM parameters */
	manage = bam_props->manage & SPS_BAM_MGR_ACCESS_MASK;
	if (manage != SPS_BAM_MGR_NONE) {
		if (bam_props->virt_addr == NULL && bam_props->virt_size == 0) {
			SPS_ERR(sps, "sps:Invalid properties for BAM: %pa",
					   &bam_props->phys_addr);
			return SPS_ERROR;
		}
	}
	if ((bam_props->manage & SPS_BAM_MGR_DEVICE_REMOTE) == 0) {
		/* BAM global is configured by local processor */
		if (bam_props->summing_threshold == 0) {
			SPS_ERR(sps,
				"sps:Invalid device ctrl properties for "
					"BAM: %pa", &bam_props->phys_addr);
			return SPS_ERROR;
		}
	}
	manage = bam_props->manage &
		  (SPS_BAM_MGR_PIPE_NO_CONFIG | SPS_BAM_MGR_PIPE_NO_CTRL);

	/* In case of error */
	*dev_handle = SPS_DEV_HANDLE_INVALID;
	result = SPS_ERROR;

	mutex_lock(&sps->lock);
	/* Is this BAM already registered? */
	bam = phy2bam(bam_props->phys_addr);
	if (bam != NULL) {
		mutex_unlock(&sps->lock);
		SPS_ERR(sps, "sps:BAM is already registered: %pa",
				&bam->props.phys_addr);
		result = -EEXIST;
		bam = NULL;   /* Avoid error clean-up kfree(bam) */
		goto exit_err;
	}

	/* Perform virtual mapping if required */
	if ((bam_props->manage & SPS_BAM_MGR_ACCESS_MASK) !=
	    SPS_BAM_MGR_NONE && bam_props->virt_addr == NULL) {
		/* Map the memory region */
		virt_addr = ioremap(bam_props->phys_addr, bam_props->virt_size);
		if (virt_addr == NULL) {
			SPS_ERR(sps,
				"sps:Unable to map BAM IO mem:%pa size:0x%x",
				&bam_props->phys_addr, bam_props->virt_size);
			goto exit_err;
		}
	}

	bam = kzalloc(sizeof(*bam), GFP_KERNEL);
	if (bam == NULL) {
		SPS_ERR(sps,
			"sps:Unable to allocate BAM device state: size 0x%zu",
			sizeof(*bam));
		goto exit_err;
	}
	memset(bam, 0, sizeof(*bam));

	mutex_init(&bam->lock);
	mutex_lock(&bam->lock);

	/* Copy configuration to BAM device descriptor */
	bam->props = *bam_props;
	if (virt_addr != NULL)
		bam->props.virt_addr = virt_addr;

	snprintf(bam_name, sizeof(bam_name), "sps_bam_%pa_0",
					&bam->props.phys_addr);
	bam->ipc_log0 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							bam_name, 0);
	if (!bam->ipc_log0)
		SPS_DBG(sps, "%s : unable to create IPC Logging 0 for bam %pa\n",
					__func__, &bam->props.phys_addr);

	snprintf(bam_name, sizeof(bam_name), "sps_bam_%pa_1",
					&bam->props.phys_addr);
	bam->ipc_log1 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							bam_name, 0);
	if (!bam->ipc_log1)
		SPS_DBG(sps, "%s : unable to create IPC Logging 1 for bam %pa\n",
					__func__, &bam->props.phys_addr);

	snprintf(bam_name, sizeof(bam_name), "sps_bam_%pa_2",
					&bam->props.phys_addr);
	bam->ipc_log2 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							bam_name, 0);
	if (!bam->ipc_log2)
		SPS_DBG(sps, "%s : unable to create IPC Logging 2 for bam %pa\n",
					__func__, &bam->props.phys_addr);

	snprintf(bam_name, sizeof(bam_name), "sps_bam_%pa_3",
					&bam->props.phys_addr);
	bam->ipc_log3 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							bam_name, 0);
	if (!bam->ipc_log3)
		SPS_DBG(sps, "%s : unable to create IPC Logging 3 for bam %pa\n",
					__func__, &bam->props.phys_addr);

	snprintf(bam_name, sizeof(bam_name), "sps_bam_%pa_4",
					&bam->props.phys_addr);
	bam->ipc_log4 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							bam_name, 0);
	if (!bam->ipc_log4)
		SPS_DBG(sps, "%s : unable to create IPC Logging 4 for bam %pa\n",
					__func__, &bam->props.phys_addr);

	if (bam_props->ipc_loglevel)
		bam->ipc_loglevel = bam_props->ipc_loglevel;
	else
		bam->ipc_loglevel = SPS_IPC_DEFAULT_LOGLEVEL;

	ok = sps_bam_device_init(bam);
	mutex_unlock(&bam->lock);
	if (ok) {
		SPS_ERR(bam, "sps:Fail to init BAM device: phys %pa",
			&bam->props.phys_addr);
		goto exit_err;
	}

	/* Add BAM to the list */
	list_add_tail(&bam->list, &sps->bams_q);
	*dev_handle = (uintptr_t) bam;

	result = 0;
exit_err:
	mutex_unlock(&sps->lock);

	if (result) {
		if (bam != NULL) {
			if (virt_addr != NULL)
				iounmap(bam->props.virt_addr);
			kfree(bam);
		}

		return result;
	}

	/* If this BAM is attached to a BAM-DMA, init the BAM-DMA device */
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	if ((bam->props.options & SPS_BAM_OPT_BAMDMA)) {
		if (sps_dma_device_init((uintptr_t) bam)) {
			bam->props.options &= ~SPS_BAM_OPT_BAMDMA;
			sps_deregister_bam_device((uintptr_t) bam);
			SPS_ERR(bam, "sps:Fail to init BAM-DMA BAM: phys %pa",
				&bam->props.phys_addr);
			return SPS_ERROR;
		}
	}
#endif /* CONFIG_SPS_SUPPORT_BAMDMA */

	SPS_INFO(bam, "sps:BAM %pa is registered.", &bam->props.phys_addr);

	return 0;
}
EXPORT_SYMBOL(sps_register_bam_device);

/**
 * Deregister a BAM device
 *
 */
int sps_deregister_bam_device(unsigned long dev_handle)
{
	struct sps_bam *bam;
	int n;

	if (dev_handle == 0) {
		SPS_ERR(sps, "sps:%s:device handle should not be 0.\n",
				__func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev_handle);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:did not find a BAM for this handle",
				__func__);
		return SPS_ERROR;
	}

	SPS_DBG3(sps, "sps:%s: SPS deregister BAM: phys %pa.",
		__func__, &bam->props.phys_addr);

	if (bam->props.options & SPS_BAM_HOLD_MEM) {
		for (n = 0; n < BAM_MAX_PIPES; n++)
			if (bam->desc_cache_pointers[n] != NULL)
				kfree(bam->desc_cache_pointers[n]);
	}

	/* If this BAM is attached to a BAM-DMA, init the BAM-DMA device */
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	if ((bam->props.options & SPS_BAM_OPT_BAMDMA)) {
		mutex_lock(&bam->lock);
		(void)sps_dma_device_de_init((uintptr_t) bam);
		bam->props.options &= ~SPS_BAM_OPT_BAMDMA;
		mutex_unlock(&bam->lock);
	}
#endif

	/* Remove the BAM from the registration list */
	mutex_lock(&sps->lock);
	list_del(&bam->list);
	mutex_unlock(&sps->lock);

	/* De-init the BAM and free resources */
	mutex_lock(&bam->lock);
	sps_bam_device_de_init(bam);
	mutex_unlock(&bam->lock);
	ipc_log_context_destroy(bam->ipc_log0);
	ipc_log_context_destroy(bam->ipc_log1);
	ipc_log_context_destroy(bam->ipc_log2);
	ipc_log_context_destroy(bam->ipc_log3);
	ipc_log_context_destroy(bam->ipc_log4);
	if (bam->props.virt_size)
		(void)iounmap(bam->props.virt_addr);

	kfree(bam);

	return 0;
}
EXPORT_SYMBOL(sps_deregister_bam_device);

/**
 * Get processed I/O vector (completed transfers)
 *
 */
int sps_get_iovec(struct sps_pipe *h, struct sps_iovec *iovec)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (iovec == NULL) {
		SPS_ERR(sps, "sps:%s:iovec pointer is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe->pipe_index);

	/* Get an iovec from the BAM pipe descriptor FIFO */
	result = sps_bam_pipe_get_iovec(bam, pipe->pipe_index, iovec);
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_get_iovec);

/**
 * Perform timer control
 *
 */
int sps_timer_ctrl(struct sps_pipe *h,
			struct sps_timer_ctrl *timer_ctrl,
			struct sps_timer_result *timer_result)
{
	struct sps_pipe *pipe = h;
	struct sps_bam *bam;
	int result;

	if (h == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (timer_ctrl == NULL) {
		SPS_ERR(sps, "sps:%s:timer_ctrl pointer is NULL.\n", __func__);
		return SPS_ERROR;
	} else if (timer_result == NULL) {
		SPS_DBG(sps, "sps:%s:no result to return.\n", __func__);
	}

	bam = sps_bam_lock(pipe);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG2(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe->pipe_index);

	/* Perform the BAM pipe timer control operation */
	result = sps_bam_pipe_timer_ctrl(bam, pipe->pipe_index, timer_ctrl,
					 timer_result);
	sps_bam_unlock(bam);

	return result;
}
EXPORT_SYMBOL(sps_timer_ctrl);

/*
 * Reset a BAM pipe
 */
int sps_pipe_reset(unsigned long dev, u32 pipe)
{
	struct sps_bam *bam;

	if (!dev) {
		SPS_ERR(sps, "sps:%s:BAM handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (pipe >= BAM_MAX_PIPES) {
		SPS_ERR(sps, "sps:%s:pipe index is invalid.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG2(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe);

	bam_pipe_reset(&bam->base, pipe);

	return 0;
}
EXPORT_SYMBOL(sps_pipe_reset);

/*
 * Disable a BAM pipe
 */
int sps_pipe_disable(unsigned long dev, u32 pipe)
{
	struct sps_bam *bam;

	if (!dev) {
		SPS_ERR(sps, "sps:%s:BAM handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (pipe >= BAM_MAX_PIPES) {
		SPS_ERR(sps, "sps:%s:pipe index is invalid.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe);

	bam_disable_pipe(&bam->base, pipe);

	return 0;
}
EXPORT_SYMBOL(sps_pipe_disable);

/*
 * Check pending descriptors in the descriptor FIFO
 * of a pipe
 */
int sps_pipe_pending_desc(unsigned long dev, u32 pipe, bool *pending)
{

	struct sps_bam *bam;

	if (!dev) {
		SPS_ERR(sps, "sps:%s:BAM handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (pipe >= BAM_MAX_PIPES) {
		SPS_ERR(sps, "sps:%s:pipe index is invalid.\n", __func__);
		return SPS_ERROR;
	}

	if (!pending) {
		SPS_ERR(sps, "sps:%s:input flag is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe);

	*pending = sps_bam_pipe_pending_desc(bam, pipe);

	return 0;
}
EXPORT_SYMBOL(sps_pipe_pending_desc);

/*
 * Process any pending IRQ of a BAM
 */
int sps_bam_process_irq(unsigned long dev)
{
	struct sps_bam *bam;
	int ret = 0;

	if (!dev) {
		SPS_ERR(sps, "sps:%s:BAM handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG1(bam, "sps:%s; BAM: %pa.\n", __func__, BAM_ID(bam));

	ret = sps_bam_check_irq(bam);

	return ret;
}
EXPORT_SYMBOL(sps_bam_process_irq);

/*
 * Get address info of a BAM
 */
int sps_get_bam_addr(unsigned long dev, phys_addr_t *base,
				u32 *size)
{
	struct sps_bam *bam;

	if (!dev) {
		SPS_ERR(sps, "sps:%s:BAM handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	*base = bam->props.phys_addr;
	*size = bam->props.virt_size;

	SPS_DBG2(bam, "sps:%s; BAM: %pa; base:%pa; size:%d.\n",
		__func__, BAM_ID(bam), base, *size);

	return 0;
}
EXPORT_SYMBOL(sps_get_bam_addr);

/*
 * Inject a ZLT with EOT for a BAM pipe
 */
int sps_pipe_inject_zlt(unsigned long dev, u32 pipe_index)
{
	struct sps_bam *bam;
	int rc;

	if (!dev) {
		SPS_ERR(sps, "sps:%s:BAM handle is NULL.\n", __func__);
		return SPS_ERROR;
	}

	if (pipe_index >= BAM_MAX_PIPES) {
		SPS_ERR(sps, "sps:%s:pipe index is invalid.\n", __func__);
		return SPS_ERROR;
	}

	bam = sps_h2bam(dev);
	if (bam == NULL) {
		SPS_ERR(sps, "sps:%s:BAM is not found by handle.\n", __func__);
		return SPS_ERROR;
	}

	SPS_DBG(bam, "sps:%s; BAM: %pa; pipe index:%d.\n",
		__func__, BAM_ID(bam), pipe_index);

	rc = sps_bam_pipe_inject_zlt(bam, pipe_index);
	if (rc)
		SPS_ERR(bam, "sps:%s:failed to inject a ZLT.\n", __func__);

	return rc;
}
EXPORT_SYMBOL(sps_pipe_inject_zlt);

/**
 * Allocate client state context
 *
 */
struct sps_pipe *sps_alloc_endpoint(void)
{
	struct sps_pipe *ctx = NULL;

	SPS_DBG(sps, "sps:%s.", __func__);

	ctx = kzalloc(sizeof(struct sps_pipe), GFP_KERNEL);
	if (ctx == NULL) {
		SPS_ERR(sps, "sps:%s:Fail to allocate pipe context.",
				__func__);
		return NULL;
	}

	sps_client_init(ctx);

	return ctx;
}
EXPORT_SYMBOL(sps_alloc_endpoint);

/**
 * Free client state context
 *
 */
int sps_free_endpoint(struct sps_pipe *ctx)
{
	int res;

	SPS_DBG(sps, "sps:%s.", __func__);

	if (ctx == NULL) {
		SPS_ERR(sps, "sps:%s:pipe is NULL.\n", __func__);
		return SPS_ERROR;
	}

	res = sps_client_de_init(ctx);

	if (res == 0)
		kfree(ctx);

	return res;
}
EXPORT_SYMBOL(sps_free_endpoint);

/**
 * Platform Driver.
 */
static int get_platform_data(struct platform_device *pdev)
{
	struct resource *resource;
	struct msm_sps_platform_data *pdata;

	SPS_DBG3(sps, "sps:%s.", __func__);

	pdata = pdev->dev.platform_data;

	if (pdata == NULL) {
		SPS_ERR(sps, "sps:%s:inavlid platform data.\n", __func__);
		sps->bamdma_restricted_pipes = 0;
		return -EINVAL;
	} else {
		sps->bamdma_restricted_pipes = pdata->bamdma_restricted_pipes;
		SPS_DBG3(sps, "sps:bamdma_restricted_pipes=0x%x.\n",
			sps->bamdma_restricted_pipes);
	}

	resource  = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 "pipe_mem");
	if (resource) {
		sps->pipemem_phys_base = resource->start;
		sps->pipemem_size = resource_size(resource);
		SPS_DBG3(sps, "sps:pipemem.base=%pa,size=0x%x.\n",
			&sps->pipemem_phys_base,
			sps->pipemem_size);
	}

#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	resource  = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 "bamdma_bam");
	if (resource) {
		sps->bamdma_bam_phys_base = resource->start;
		sps->bamdma_bam_size = resource_size(resource);
		SPS_DBG(sps, "sps:bamdma_bam.base=%pa,size=0x%x.",
			&sps->bamdma_bam_phys_base,
			sps->bamdma_bam_size);
	}

	resource  = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 "bamdma_dma");
	if (resource) {
		sps->bamdma_dma_phys_base = resource->start;
		sps->bamdma_dma_size = resource_size(resource);
		SPS_DBG(sps, "sps:bamdma_dma.base=%pa,size=0x%x.",
			&sps->bamdma_dma_phys_base,
			sps->bamdma_dma_size);
	}

	resource  = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						 "bamdma_irq");
	if (resource) {
		sps->bamdma_irq = resource->start;
		SPS_DBG(sps, "sps:bamdma_irq=%d.", sps->bamdma_irq);
	}
#endif

	return 0;
}

/**
 * Read data from device tree
 */
static int get_device_tree_data(struct platform_device *pdev)
{
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	struct resource *resource;

	SPS_DBG(sps, "sps:%s.", __func__);

	if (of_property_read_u32((&pdev->dev)->of_node,
				"qcom,bam-dma-res-pipes",
				&sps->bamdma_restricted_pipes))
		SPS_DBG(sps,
			"sps:%s:No restricted bamdma pipes on this target.\n",
			__func__);
	else
		SPS_DBG(sps, "sps:bamdma_restricted_pipes=0x%x.",
			sps->bamdma_restricted_pipes);

	resource  = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (resource) {
		sps->bamdma_bam_phys_base = resource->start;
		sps->bamdma_bam_size = resource_size(resource);
		SPS_DBG(sps, "sps:bamdma_bam.base=%pa,size=0x%x.",
			&sps->bamdma_bam_phys_base,
			sps->bamdma_bam_size);
	} else {
		SPS_ERR(sps, "sps:%s:BAM DMA BAM mem unavailable.", __func__);
		return -ENODEV;
	}

	resource  = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (resource) {
		sps->bamdma_dma_phys_base = resource->start;
		sps->bamdma_dma_size = resource_size(resource);
		SPS_DBG(sps, "sps:bamdma_dma.base=%pa,size=0x%x.",
			&sps->bamdma_dma_phys_base,
			sps->bamdma_dma_size);
	} else {
		SPS_ERR(sps, "sps:%s:BAM DMA mem unavailable.", __func__);
		return -ENODEV;
	}

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (resource) {
		imem = true;
		sps->pipemem_phys_base = resource->start;
		sps->pipemem_size = resource_size(resource);
		SPS_DBG(sps, "sps:pipemem.base=%pa,size=0x%x.",
			&sps->pipemem_phys_base,
			sps->pipemem_size);
	} else {
		imem = false;
		SPS_DBG(sps, "sps:%s:No pipe memory on this target.\n",
				__func__);
	}

	resource  = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (resource) {
		sps->bamdma_irq = resource->start;
		SPS_DBG(sps, "sps:bamdma_irq=%d.", sps->bamdma_irq);
	} else {
		SPS_ERR(sps, "sps:%s:BAM DMA IRQ unavailable.", __func__);
		return -ENODEV;
	}
#endif

	if (of_property_read_u32((&pdev->dev)->of_node,
				"qcom,device-type",
				&d_type)) {
		d_type = 3;
		SPS_DBG3(sps, "sps:default device type %d.\n", d_type);
	} else
		SPS_DBG3(sps, "sps:device type is %d.", d_type);

	enhd_pipe = of_property_read_bool((&pdev->dev)->of_node,
			"qcom,pipe-attr-ee");
	SPS_DBG3(sps, "sps:PIPE_ATTR_EE is %s supported.\n",
			(enhd_pipe ? "" : "not"));

	return 0;
}

static struct of_device_id msm_sps_match[] = {
	{	.compatible = "qcom,msm_sps",
		.data = &bam_types[SPS_BAM_NDP]
	},
	{	.compatible = "qcom,msm_sps_4k",
		.data = &bam_types[SPS_BAM_NDP_4K]
	},
	{}
};

static int msm_sps_probe(struct platform_device *pdev)
{
	int ret = -ENODEV;

	SPS_DBG3(sps, "sps:%s.", __func__);

	if (pdev->dev.of_node) {
		const struct of_device_id *match;

		if (get_device_tree_data(pdev)) {
			SPS_ERR(sps,
				"sps:%s:Fail to get data from device tree.",
				__func__);
			return -ENODEV;
		} else
			SPS_DBG(sps, "%s", "sps:get data from device tree.");

		match = of_match_device(msm_sps_match, &pdev->dev);
		if (match) {
			bam_type = *((enum sps_bam_type *)(match->data));
			SPS_DBG3(sps, "sps:BAM type is:%d\n", bam_type);
		} else {
			bam_type = SPS_BAM_NDP;
			SPS_DBG3(sps, "sps:use default BAM type:%d\n",
				bam_type);
		}
	} else {
		d_type = 0;
		if (get_platform_data(pdev)) {
			SPS_ERR(sps, "sps:%s:Fail to get platform data.",
				__func__);
			return -ENODEV;
		} else
			SPS_DBG(sps, "%s", "sps:get platform data.");
		bam_type = SPS_BAM_LEGACY;
	}

	/* Create Device */
	sps->dev_class = class_create(THIS_MODULE, SPS_DRV_NAME);

	ret = alloc_chrdev_region(&sps->dev_num, 0, 1, SPS_DRV_NAME);
	if (ret) {
		SPS_ERR(sps, "sps:%s:alloc_chrdev_region err.", __func__);
		goto alloc_chrdev_region_err;
	}

	sps->dev = device_create(sps->dev_class, NULL, sps->dev_num, sps,
				SPS_DRV_NAME);
	if (IS_ERR(sps->dev)) {
		SPS_ERR(sps, "sps:%s:device_create err.", __func__);
		goto device_create_err;
	}

	if (pdev->dev.of_node)
		sps->dev->of_node = pdev->dev.of_node;

	if (!d_type) {
		sps->pmem_clk = clk_get(sps->dev, "mem_clk");
		if (IS_ERR(sps->pmem_clk)) {
			if (PTR_ERR(sps->pmem_clk) == -EPROBE_DEFER)
				ret = -EPROBE_DEFER;
			else
				SPS_ERR(sps, "sps:%s:fail to get pmem_clk.",
					__func__);
			goto pmem_clk_err;
		} else {
			ret = clk_prepare_enable(sps->pmem_clk);
			if (ret) {
				SPS_ERR(sps,
					"sps:%s:failed to enable pmem_clk.",
					__func__);
				goto pmem_clk_en_err;
			}
		}
	}

#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	sps->dfab_clk = clk_get(sps->dev, "dfab_clk");
	if (IS_ERR(sps->dfab_clk)) {
		if (PTR_ERR(sps->dfab_clk) == -EPROBE_DEFER)
			ret = -EPROBE_DEFER;
		else
			SPS_ERR(sps, "sps:%s:fail to get dfab_clk.", __func__);
		goto dfab_clk_err;
	} else {
		ret = clk_set_rate(sps->dfab_clk, 64000000);
		if (ret) {
			SPS_ERR(sps, "sps:%s:failed to set dfab_clk rate.",
				__func__);
			clk_put(sps->dfab_clk);
			goto dfab_clk_err;
		}
	}

	sps->bamdma_clk = clk_get(sps->dev, "dma_bam_pclk");
	if (IS_ERR(sps->bamdma_clk)) {
		if (PTR_ERR(sps->bamdma_clk) == -EPROBE_DEFER)
			ret = -EPROBE_DEFER;
		else
			SPS_ERR(sps, "sps:%s:fail to get bamdma_clk.",
				__func__);
		clk_put(sps->dfab_clk);
		goto dfab_clk_err;
	} else {
		ret = clk_prepare_enable(sps->bamdma_clk);
		if (ret) {
			SPS_ERR(sps, "sps:failed to enable bamdma_clk. ret=%d",
									ret);
			clk_put(sps->bamdma_clk);
			clk_put(sps->dfab_clk);
			goto dfab_clk_err;
		}
	}

	ret = clk_prepare_enable(sps->dfab_clk);
	if (ret) {
		SPS_ERR(sps, "sps:failed to enable dfab_clk. ret=%d", ret);
		clk_disable_unprepare(sps->bamdma_clk);
		clk_put(sps->bamdma_clk);
		clk_put(sps->dfab_clk);
		goto dfab_clk_err;
	}
#endif
	ret = sps_device_init();
	if (ret) {
		SPS_ERR(sps, "sps:%s:sps_device_init err.", __func__);

#ifdef CONFIG_SPS_SUPPORT_BAMDMA
		clk_disable_unprepare(sps->dfab_clk);
		clk_disable_unprepare(sps->bamdma_clk);
		clk_put(sps->bamdma_clk);
		clk_put(sps->dfab_clk);
#endif
		goto dfab_clk_err;
	}
#ifdef CONFIG_SPS_SUPPORT_BAMDMA
	clk_disable_unprepare(sps->dfab_clk);
	clk_disable_unprepare(sps->bamdma_clk);
#endif
	sps->is_ready = true;

	SPS_INFO(sps, "%s", "sps:sps is ready.\n");

	return 0;
dfab_clk_err:
	if (!d_type)
		clk_disable_unprepare(sps->pmem_clk);
pmem_clk_en_err:
	if (!d_type)
		clk_put(sps->pmem_clk);
pmem_clk_err:
	device_destroy(sps->dev_class, sps->dev_num);
device_create_err:
	unregister_chrdev_region(sps->dev_num, 1);
alloc_chrdev_region_err:
	class_destroy(sps->dev_class);

	return ret;
}

static int msm_sps_remove(struct platform_device *pdev)
{
	SPS_DBG3(sps, "sps:%s.\n", __func__);

	device_destroy(sps->dev_class, sps->dev_num);
	unregister_chrdev_region(sps->dev_num, 1);
	class_destroy(sps->dev_class);
	sps_device_de_init();

	clk_put(sps->dfab_clk);
	if (!d_type)
		clk_put(sps->pmem_clk);
	clk_put(sps->bamdma_clk);

	return 0;
}

static struct platform_driver msm_sps_driver = {
	.probe          = msm_sps_probe,
	.driver		= {
		.name	= SPS_DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = msm_sps_match,
		.suppress_bind_attrs = true,
	},
	.remove		= msm_sps_remove,
};

/**
 * Module Init.
 */
static int __init sps_init(void)
{
	int ret;

#ifdef CONFIG_DEBUG_FS
	sps_debugfs_init();
#endif

	pr_debug("sps:%s.", __func__);

	/* Allocate the SPS driver state struct */
	sps = kzalloc(sizeof(*sps), GFP_KERNEL);
	if (sps == NULL)
		return -ENOMEM;

	sps->ipc_log0 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							"sps_ipc_log0", 0);
	if (!sps->ipc_log0)
		pr_err("Failed to create IPC log0\n");
	sps->ipc_log1 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							"sps_ipc_log1", 0);
	if (!sps->ipc_log1)
		pr_err("Failed to create IPC log1\n");
	sps->ipc_log2 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							"sps_ipc_log2", 0);
	if (!sps->ipc_log2)
		pr_err("Failed to create IPC log2\n");
	sps->ipc_log3 = ipc_log_context_create(SPS_IPC_LOGPAGES,
							"sps_ipc_log3", 0);
	if (!sps->ipc_log3)
		pr_err("Failed to create IPC log3\n");
	sps->ipc_log4 = ipc_log_context_create(SPS_IPC_LOGPAGES *
				SPS_IPC_REG_DUMP_FACTOR, "sps_ipc_log4", 0);
	if (!sps->ipc_log4)
		pr_err("Failed to create IPC log4\n");

	ret = platform_driver_register(&msm_sps_driver);

	return ret;
}

/**
 * Module Exit.
 */
static void __exit sps_exit(void)
{
	pr_debug("sps:%s.", __func__);

	platform_driver_unregister(&msm_sps_driver);

	if (sps != NULL) {
		kfree(sps);
		sps = NULL;
	}

#ifdef CONFIG_DEBUG_FS
	sps_debugfs_exit();
#endif
}

arch_initcall(sps_init);
module_exit(sps_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Smart Peripheral Switch (SPS)");
