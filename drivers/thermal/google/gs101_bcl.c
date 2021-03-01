// SPDX-License-Identifier: GPL-2.0
/*
 * gs101_bcl.c gsoc101 bcl driver
 *
 * Copyright (c) 2020, Google LLC. All rights reserved.
 *
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/mfd/samsung/s2mpg10.h>
#include <linux/mfd/samsung/s2mpg10-register.h>
#include <linux/mfd/samsung/s2mpg11.h>
#include <linux/mfd/samsung/s2mpg11-register.h>
#include <linux/regulator/pmic_class.h>
#include <soc/google/exynos-pm.h>
#include <soc/google/exynos-pmu-if.h>
#if IS_ENABLED(CONFIG_DEBUG_FS)
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif
#include "../thermal_core.h"

/* This driver determines if HW was throttled due to SMPL/OCP */

#define CPUCL0_BASE (0x20c00000)
#define CPUCL1_BASE (0x20c10000)
#define CPUCL2_BASE (0x20c20000)
#define G3D_BASE (0x1c400000)
#define TPU_BASE (0x1cc00000)
#define SYSREG_CPUCL0_BASE (0x20c40000)
#define CLUSTER0_GENERAL_CTRL_64 (0x1404)
#define CLKDIVSTEP (0x830)
#define CPUCL0_CLKDIVSTEP_STAT (0x83c)
#define CPUCL0_CLKDIVSTEP_CON (0x838)
#define CPUCL12_CLKDIVSTEP_STAT (0x848)
#define CPUCL12_CLKDIVSTEP_CON_HEAVY (0x840)
#define CPUCL12_CLKDIVSTEP_CON_LIGHT (0x844)
#define G3D_CLKDIVSTEP_STAT (0x854)
#define TPU_CLKDIVSTEP_STAT (0x850)
#define CLUSTER0_MPMM (0x1408)
#define CLUSTER0_PPM (0x140c)
#define MPMMEN_MASK (0xF << 21)
#define PPMEN_MASK (0x3 << 8)
#define PPMCTL_MASK (0xFF)
#define OCP_WARN_MASK (0x1F)
#define B3M_UPPER_LIMIT (7000)
#define B3M_LOWER_LIMIT (1688)
#define B3M_STEP (166)
#define B2M_UPPER_LIMIT (12000)
#define B2M_LOWER_LIMIT (4000)
#define B2M_STEP (250)
#define B10M_UPPER_LIMIT (10500)
#define B10M_LOWER_LIMIT (2500)
#define B10M_STEP (250)
#define B2S_UPPER_LIMIT (13200)
#define B2S_LOWER_LIMIT (5200)
#define B2S_STEP (250)
#define SMPL_BATTERY_VOLTAGE (4200)
#define SMPL_UPPER_LIMIT (3300)
#define SMPL_LOWER_LIMIT (2600)
#define SMPL_STEP (100)
#define SMPL_NUM_LVL (32)
#define THERMAL_IRQ_COUNTER_LIMIT (5)
#define THERMAL_HYST_LEVEL (100)
#define ACTIVE_HIGH (0x1)
#define ACTIVE_LOW (0x0)
#define THERMAL_DELAY_INIT_MS 5000
#define PMIC_OVERHEAT_UPPER_LIMIT (2000)
#define PMIC_120C_UPPER_LIMIT (1200)
#define PMIC_140C_UPPER_LIMIT (1400)
#define BUF_SIZE 192
#define PMU_ALIVE_CPU0_OUT (0x1CA0)
#define PMU_ALIVE_CPU1_OUT (0x1D20)
#define PMU_ALIVE_CPU2_OUT (0x1DA0)
#define PMU_ALIVE_TPU_OUT (0x2920)
#define PMU_ALIVE_GPU_OUT (0x1E20)

#define BCL_DEBUG_ATTRIBUTE(name, fn_read, fn_write) \
static const struct file_operations name = {	\
	.open	= simple_open,			\
	.llseek	= no_llseek,			\
	.read	= fn_read,			\
	.write	= fn_write,			\
}

enum PMIC_THERMAL_SENSOR {
	PMIC_SOC,
	PMIC_120C,
	PMIC_140C,
	PMIC_OVERHEAT,
	PMIC_THERMAL_SENSOR_MAX,
};

enum IRQ_SOURCE_S2MPG10 {
	IRQ_SMPL_WARN,
	IRQ_OCP_WARN_CPUCL1,
	IRQ_OCP_WARN_CPUCL2,
	IRQ_SOFT_OCP_WARN_CPUCL1,
	IRQ_SOFT_OCP_WARN_CPUCL2,
	IRQ_OCP_WARN_TPU,
	IRQ_SOFT_OCP_WARN_TPU,
	IRQ_PMIC_120C,
	IRQ_PMIC_140C,
	IRQ_PMIC_OVERHEAT,
	IRQ_SOURCE_S2MPG10_MAX,
};

enum IRQ_SOURCE_S2MPG11 {
	IRQ_OCP_WARN_GPU,
	IRQ_SOFT_OCP_WARN_GPU,
	IRQ_SOURCE_S2MPG11_MAX,
};

enum sys_throttling_switch {
	SYS_THROTTLING_DISABLED,
	SYS_THROTTLING_ENABLED,
	SYS_THROTTLING_GEAR0,
	SYS_THROTTLING_GEAR1,
	SYS_THROTTLING_GEAR2,
	SYS_THROTTLING_MAX,
};

enum PMIC_REG { S2MPG10, S2MPG11 };

static const unsigned int sys_throttling_settings[SYS_THROTTLING_MAX] = {
	[SYS_THROTTLING_DISABLED] = 0x1F,
	[SYS_THROTTLING_ENABLED] = 0x10,
	[SYS_THROTTLING_GEAR0] = 0x10,
	[SYS_THROTTLING_GEAR1] = 0x15,
	[SYS_THROTTLING_GEAR2] = 0x1A};

static const char * const s2mpg10_source[] = {
	[IRQ_SMPL_WARN] = "smpl_warn",
	[IRQ_OCP_WARN_CPUCL1] = "ocp_cpu1",
	[IRQ_OCP_WARN_CPUCL2] = "ocp_cpu2",
	[IRQ_SOFT_OCP_WARN_CPUCL1] = "soft_ocp_cpu1",
	[IRQ_SOFT_OCP_WARN_CPUCL2] = "soft_ocp_cpu2",
	[IRQ_OCP_WARN_TPU] = "ocp_tpu",
	[IRQ_SOFT_OCP_WARN_TPU] = "soft_ocp_tpu",
	[IRQ_PMIC_120C] = "pmic_120c",
	[IRQ_PMIC_140C] = "pmic_140c",
	[IRQ_PMIC_OVERHEAT] = "pmic_overheat"};

static const char * const s2mpg11_source[] = {
	[IRQ_OCP_WARN_GPU] = "ocp_gpu",
	[IRQ_SOFT_OCP_WARN_GPU] = "soft_ocp_gpu"};

static const char * const clk_ratio_source[] = {
	"cpu0", "cpu1_heavy", "cpu2_heavy", "tpu_heavy", "gpu_heavy",
	"cpu1_light", "cpu2_light", "tpu_light", "gpu_light"
};

static const char * const clk_stats_source[] = {
	"cpu0", "cpu1", "cpu2", "tpu", "gpu"
};

static const unsigned int subsystem_pmu[] = {
	PMU_ALIVE_CPU0_OUT,
	PMU_ALIVE_CPU1_OUT,
	PMU_ALIVE_CPU2_OUT,
	PMU_ALIVE_TPU_OUT,
	PMU_ALIVE_GPU_OUT
};

struct ocpsmpl_stats {
	ktime_t _time;
	int capacity;
	int voltage;
};

struct gs101_bcl_dev {
	struct device *device;
	struct dentry *debug_entry;
	void __iomem *base_mem[5];
	void __iomem *sysreg_cpucl0;
	struct power_supply *batt_psy;

	struct notifier_block psy_nb;
	struct delayed_work soc_eval_work;
	struct delayed_work mfd_init;

	void *iodev;

	int trip_high_temp;
	int trip_low_temp;
	int trip_val;
	struct mutex state_trans_lock;
	struct mutex ratio_lock;
	struct thermal_zone_device *soc_tzd;
	struct thermal_zone_of_device_ops soc_ops;
	struct mutex s2mpg10_irq_lock[IRQ_SOURCE_S2MPG10_MAX];
	struct mutex s2mpg11_irq_lock[IRQ_SOURCE_S2MPG11_MAX];
	struct delayed_work s2mpg10_irq_work[IRQ_SOURCE_S2MPG10_MAX];
	struct delayed_work s2mpg11_irq_work[IRQ_SOURCE_S2MPG11_MAX];
	struct thermal_zone_device *s2mpg10_tz_irq[IRQ_SOURCE_S2MPG10_MAX];
	struct thermal_zone_device *s2mpg11_tz_irq[IRQ_SOURCE_S2MPG11_MAX];

	unsigned int s2mpg10_lvl[IRQ_SOURCE_S2MPG10_MAX];
	unsigned int s2mpg11_lvl[IRQ_SOURCE_S2MPG11_MAX];
	unsigned int s2mpg10_irq[IRQ_SOURCE_S2MPG10_MAX];
	unsigned int s2mpg11_irq[IRQ_SOURCE_S2MPG11_MAX];
	int s2mpg10_counter[IRQ_SOURCE_S2MPG10_MAX];
	int s2mpg11_counter[IRQ_SOURCE_S2MPG11_MAX];
	int s2mpg10_pin[IRQ_SOURCE_S2MPG10_MAX];
	int s2mpg11_pin[IRQ_SOURCE_S2MPG11_MAX];
	atomic_t s2mpg10_cnt[IRQ_SOURCE_S2MPG10_MAX];
	struct ocpsmpl_stats s2mpg10_stats[IRQ_SOURCE_S2MPG10_MAX];
	atomic_t s2mpg11_cnt[IRQ_SOURCE_S2MPG11_MAX];
	struct ocpsmpl_stats s2mpg11_stats[IRQ_SOURCE_S2MPG11_MAX];

	struct s2mpg10_dev *s2mpg10;
	struct s2mpg11_dev *s2mpg11;

	struct i2c_client *s2mpg10_i2c;
	struct i2c_client *s2mpg11_i2c;

	unsigned int s2mpg10_triggered_irq[IRQ_SOURCE_S2MPG10_MAX];
	unsigned int s2mpg11_triggered_irq[IRQ_SOURCE_S2MPG11_MAX];

};


static const struct platform_device_id google_gs101_id_table[] = {
	{.name = "google_mitigation",},
	{},
};

DEFINE_MUTEX(sysreg_lock);

static bool is_subsystem_on(unsigned int addr)
{
	unsigned int value;

	exynos_pmu_read(addr, &value);
	return ((value & 0xF) == 0x4);
}

static int s2mpg10_read_level(void *data, int *val, int id)
{
	struct gs101_bcl_dev *gs101_bcl_device = data;

	if ((gs101_bcl_device->s2mpg10_counter[id] != 0) &&
	    (gs101_bcl_device->s2mpg10_counter[id] < THERMAL_IRQ_COUNTER_LIMIT)) {
		*val = gs101_bcl_device->s2mpg10_lvl[id] +
				THERMAL_HYST_LEVEL;
		gs101_bcl_device->s2mpg10_counter[id] += 1;
	} else {
		*val = gs101_bcl_device->s2mpg10_lvl[id];
		gs101_bcl_device->s2mpg10_counter[id] = 0;
	}
	return 0;
}

static int s2mpg11_read_level(void *data, int *val, int id)
{
	struct gs101_bcl_dev *gs101_bcl_device = data;

	if (gs101_bcl_device->s2mpg11_counter[id] != 0) {
		*val = gs101_bcl_device->s2mpg11_lvl[id] +
				THERMAL_HYST_LEVEL;
	} else {
		*val = gs101_bcl_device->s2mpg11_lvl[id];
	}
	return 0;
}

static void irq_work(struct gs101_bcl_dev *gs101_bcl_device, u8 active_pull, u8 idx, u8 pmic)
{
	int state = !active_pull;

	if (pmic == S2MPG10) {
		mutex_lock(&gs101_bcl_device->s2mpg10_irq_lock[idx]);
		state = gpio_get_value(gs101_bcl_device->s2mpg10_pin[idx]);
		if (state == active_pull) {
			gs101_bcl_device->s2mpg10_triggered_irq[idx] = 1;
			queue_delayed_work(system_wq, &gs101_bcl_device->s2mpg10_irq_work[idx],
			 msecs_to_jiffies(300));
		} else {
			gs101_bcl_device->s2mpg10_triggered_irq[idx] = 0;
			gs101_bcl_device->s2mpg10_counter[idx] = 0;
			enable_irq(gs101_bcl_device->s2mpg10_irq[idx]);
		}
		mutex_unlock(&gs101_bcl_device->s2mpg10_irq_lock[idx]);
	} else {
		mutex_lock(&gs101_bcl_device->s2mpg11_irq_lock[idx]);
		state = gpio_get_value(gs101_bcl_device->s2mpg11_pin[idx]);
		if (state == active_pull) {
			gs101_bcl_device->s2mpg11_triggered_irq[idx] = 1;
			queue_delayed_work(system_wq, &gs101_bcl_device->s2mpg11_irq_work[idx],
			 msecs_to_jiffies(300));
		} else {
			gs101_bcl_device->s2mpg11_triggered_irq[idx] = 0;
			gs101_bcl_device->s2mpg11_counter[idx] = 0;
			enable_irq(gs101_bcl_device->s2mpg11_irq[idx]);
		}
		mutex_unlock(&gs101_bcl_device->s2mpg11_irq_lock[idx]);
	}
}

static struct power_supply *google_gs101_get_power_supply(struct gs101_bcl_dev *bcl_dev)
{
	static struct power_supply *psy[2];
	static struct power_supply *batt_psy;
	int err = 0;

	batt_psy = NULL;
	err = power_supply_get_by_phandle_array(bcl_dev->device->of_node,
						"google,power-supply", psy,
						ARRAY_SIZE(psy));
	if (err > 0)
		batt_psy = psy[0];
	return batt_psy;
}

static void ocpsmpl_read_stats(struct ocpsmpl_stats *dst, struct power_supply *psy)
{
	union power_supply_propval ret = {0};
	int err = 0;

	dst->_time = ktime_to_ms(ktime_get());
	err = power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &ret);
	if (err < 0)
		dst->capacity = -1;
	else
		dst->capacity = ret.intval;
	err = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &ret);
	if (err < 0)
		dst->voltage = -1;
	else
		dst->voltage = ret.intval;

}

static irqreturn_t irq_handler(int irq, void *data, u8 pmic, u8 idx, u8 active_pull)
{
	struct gs101_bcl_dev *gs101_bcl_device = data;

	if (pmic == S2MPG10) {
		mutex_lock(&gs101_bcl_device->s2mpg10_irq_lock[idx]);
		atomic_inc(&gs101_bcl_device->s2mpg10_cnt[idx]);
		ocpsmpl_read_stats(&gs101_bcl_device->s2mpg10_stats[idx],
				   gs101_bcl_device->batt_psy);
		gs101_bcl_device->s2mpg10_triggered_irq[idx] = 1;
		disable_irq_nosync(gs101_bcl_device->s2mpg10_irq[idx]);
		queue_delayed_work(system_wq, &gs101_bcl_device->s2mpg10_irq_work[idx],
				   msecs_to_jiffies(300));
		mutex_unlock(&gs101_bcl_device->s2mpg10_irq_lock[idx]);
		pr_info_ratelimited("S2MPG10 IRQ : %d triggered\n", irq);
		if (gs101_bcl_device->s2mpg10_counter[idx] == 0) {
			gs101_bcl_device->s2mpg10_counter[idx] += 1;

			/* Minimize the amount of thermal update by only triggering
			 * update every THERMAL_IRQ_COUNTER_LIMIT IRQ triggered.
			 */
			if (gs101_bcl_device->s2mpg10_tz_irq[idx])
				thermal_zone_device_update(
						gs101_bcl_device->s2mpg10_tz_irq[idx],
						THERMAL_EVENT_UNSPECIFIED);
		}
	} else {
		mutex_lock(&gs101_bcl_device->s2mpg11_irq_lock[idx]);
		atomic_inc(&gs101_bcl_device->s2mpg11_cnt[idx]);
		ocpsmpl_read_stats(&gs101_bcl_device->s2mpg11_stats[idx],
				   gs101_bcl_device->batt_psy);
		gs101_bcl_device->s2mpg11_triggered_irq[idx] = 1;
		disable_irq_nosync(gs101_bcl_device->s2mpg11_irq[idx]);
		queue_delayed_work(system_wq, &gs101_bcl_device->s2mpg11_irq_work[idx],
				   msecs_to_jiffies(300));
		mutex_unlock(&gs101_bcl_device->s2mpg11_irq_lock[idx]);
		pr_info_ratelimited("S2MPG11 IRQ : %d triggered\n", irq);
		if (gs101_bcl_device->s2mpg11_counter[idx] == 0) {
			gs101_bcl_device->s2mpg11_counter[idx] = 1;

			/* Minimize the amount of thermal update by only triggering
			 * update every THERMAL_IRQ_COUNTER_LIMIT IRQ triggered.
			 */
			if (gs101_bcl_device->s2mpg11_tz_irq[idx])
				thermal_zone_device_update(
						gs101_bcl_device->s2mpg11_tz_irq[idx],
						THERMAL_EVENT_UNSPECIFIED);
		}
	}
	return IRQ_HANDLED;
}

static irqreturn_t gs101_smpl_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_SMPL_WARN, ACTIVE_LOW);
}

static void gs101_smpl_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
	    container_of(work, struct gs101_bcl_dev, s2mpg10_irq_work[IRQ_SMPL_WARN].work);

	irq_work(gs101_bcl_device, ACTIVE_LOW, IRQ_SMPL_WARN, S2MPG10);
}

static int smpl_warn_read_voltage(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_SMPL_WARN);
}

static const struct thermal_zone_of_device_ops gs101_smpl_warn_ops = {
	.get_temp = smpl_warn_read_voltage,
};

static void gs101_cpu1_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_OCP_WARN_CPUCL1].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_OCP_WARN_CPUCL1, S2MPG10);
}

static irqreturn_t gs101_cpu1_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_OCP_WARN_CPUCL1, ACTIVE_HIGH);
}

static int ocp_cpu1_read_current(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_OCP_WARN_CPUCL1);
}

static const struct thermal_zone_of_device_ops gs101_ocp_cpu1_ops = {
	.get_temp = ocp_cpu1_read_current,
};

static void gs101_cpu2_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_OCP_WARN_CPUCL2].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_OCP_WARN_CPUCL2, S2MPG10);
}

static irqreturn_t gs101_cpu2_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_OCP_WARN_CPUCL2, ACTIVE_HIGH);
}

static int ocp_cpu2_read_current(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_OCP_WARN_CPUCL2);
}

static const struct thermal_zone_of_device_ops gs101_ocp_cpu2_ops = {
	.get_temp = ocp_cpu2_read_current,
};

static void gs101_soft_cpu1_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_CPUCL1].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_SOFT_OCP_WARN_CPUCL1, S2MPG10);
}

static irqreturn_t gs101_soft_cpu1_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_SOFT_OCP_WARN_CPUCL1, ACTIVE_HIGH);
}

static int soft_ocp_cpu1_read_current(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_SOFT_OCP_WARN_CPUCL1);
}

static const struct thermal_zone_of_device_ops gs101_soft_ocp_cpu1_ops = {
	.get_temp = soft_ocp_cpu1_read_current,
};

static void gs101_soft_cpu2_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_CPUCL2].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_SOFT_OCP_WARN_CPUCL2, S2MPG10);
}

static irqreturn_t gs101_soft_cpu2_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_SOFT_OCP_WARN_CPUCL2, ACTIVE_HIGH);
}

static int soft_ocp_cpu2_read_current(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_SOFT_OCP_WARN_CPUCL2);
}

static const struct thermal_zone_of_device_ops gs101_soft_ocp_cpu2_ops = {
	.get_temp = soft_ocp_cpu2_read_current,
};

static void gs101_tpu_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_OCP_WARN_TPU].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_OCP_WARN_TPU, S2MPG10);
}

static irqreturn_t gs101_tpu_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_OCP_WARN_TPU, ACTIVE_HIGH);
}

static int ocp_tpu_read_current(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_OCP_WARN_TPU);
}

static const struct thermal_zone_of_device_ops gs101_ocp_tpu_ops = {
	.get_temp = ocp_tpu_read_current,
};

static void gs101_soft_tpu_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_TPU].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_SOFT_OCP_WARN_TPU, S2MPG10);
}

static irqreturn_t gs101_soft_tpu_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_SOFT_OCP_WARN_TPU, ACTIVE_HIGH);
}

static int soft_ocp_tpu_read_current(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_SOFT_OCP_WARN_TPU);
}

static const struct thermal_zone_of_device_ops gs101_soft_ocp_tpu_ops = {
	.get_temp = soft_ocp_tpu_read_current,
};

static void gs101_gpu_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_OCP_WARN_GPU].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_OCP_WARN_GPU, S2MPG11);
}

static irqreturn_t gs101_gpu_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG11, IRQ_OCP_WARN_GPU, ACTIVE_HIGH);
}

static int ocp_gpu_read_current(void *data, int *val)
{
	return s2mpg11_read_level(data, val, IRQ_OCP_WARN_GPU);
}

static const struct thermal_zone_of_device_ops gs101_ocp_gpu_ops = {
	.get_temp = ocp_gpu_read_current,
};

static void gs101_soft_gpu_warn_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_GPU].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_SOFT_OCP_WARN_GPU, S2MPG11);
}

static irqreturn_t gs101_soft_gpu_ocp_warn_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG11, IRQ_SOFT_OCP_WARN_GPU, ACTIVE_HIGH);
}

static int soft_ocp_gpu_read_current(void *data, int *val)
{
	return s2mpg11_read_level(data, val, IRQ_SOFT_OCP_WARN_GPU);
}

static const struct thermal_zone_of_device_ops gs101_soft_ocp_gpu_ops = {
	.get_temp = soft_ocp_gpu_read_current,
};

static void gs101_pmic_120c_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_PMIC_120C].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_PMIC_120C, S2MPG10);
}

static irqreturn_t gs101_pmic_120c_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_PMIC_120C, ACTIVE_HIGH);
}

static int pmic_120c_read_temp(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_PMIC_120C);
}

static const struct thermal_zone_of_device_ops gs101_pmic_120c_ops = {
	.get_temp = pmic_120c_read_temp,
};

static void gs101_pmic_140c_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_PMIC_140C].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_PMIC_140C, S2MPG10);
}

static irqreturn_t gs101_pmic_140c_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_PMIC_140C, ACTIVE_HIGH);
}

static int pmic_140c_read_temp(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_PMIC_140C);
}

static const struct thermal_zone_of_device_ops gs101_pmic_140c_ops = {
	.get_temp = pmic_140c_read_temp,
};

static void gs101_pmic_overheat_work(struct work_struct *work)
{
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(work, struct gs101_bcl_dev,
		   s2mpg10_irq_work[IRQ_PMIC_OVERHEAT].work);

	irq_work(gs101_bcl_device, ACTIVE_HIGH, IRQ_PMIC_OVERHEAT, S2MPG10);
}

static irqreturn_t gs101_tsd_overheat_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_HANDLED;

	return irq_handler(irq, data, S2MPG10, IRQ_PMIC_OVERHEAT, ACTIVE_HIGH);
}

static int tsd_overheat_read_temp(void *data, int *val)
{
	return s2mpg10_read_level(data, val, IRQ_PMIC_OVERHEAT);
}

static const struct thermal_zone_of_device_ops gs101_pmic_overheat_ops = {
	.get_temp = tsd_overheat_read_temp,
};

static int gs101_bcl_set_soc(void *data, int low, int high)
{
	struct gs101_bcl_dev *gs101_bcl_device = data;

	if (high == gs101_bcl_device->trip_high_temp)
		return 0;

	mutex_lock(&gs101_bcl_device->state_trans_lock);
	gs101_bcl_device->trip_low_temp = low;
	gs101_bcl_device->trip_high_temp = high;
	schedule_delayed_work(&gs101_bcl_device->soc_eval_work, 0);

	mutex_unlock(&gs101_bcl_device->state_trans_lock);
	return 0;
}

static int gs101_bcl_read_soc(void *data, int *val)
{
	struct gs101_bcl_dev *bcl_dev = data;
	static struct power_supply *batt_psy;
	union power_supply_propval ret = {
		0,
	};
	int err = 0;

	*val = 100;
	if (!batt_psy)
		batt_psy = power_supply_get_by_name("battery");
	if (batt_psy) {
		err = power_supply_get_property(
			batt_psy, POWER_SUPPLY_PROP_CAPACITY, &ret);
		if (err) {
			dev_err(bcl_dev->device, "battery percentage read error:%d\n", err);
			return err;
		}
		*val = 100 - ret.intval;
	}
	pr_debug("soc:%d\n", *val);

	return err;
}

static void gs101_bcl_evaluate_soc(struct work_struct *work)
{
	int battery_percentage_reverse;
	struct gs101_bcl_dev *gs101_bcl_device =
	    container_of(work, struct gs101_bcl_dev, soc_eval_work.work);

	if (gs101_bcl_read_soc(gs101_bcl_device, &battery_percentage_reverse))
		return;

	mutex_lock(&gs101_bcl_device->state_trans_lock);
	if ((battery_percentage_reverse < gs101_bcl_device->trip_high_temp) &&
		(battery_percentage_reverse > gs101_bcl_device->trip_low_temp))
		goto eval_exit;

	gs101_bcl_device->trip_val = battery_percentage_reverse;
	mutex_unlock(&gs101_bcl_device->state_trans_lock);
	thermal_zone_device_update(gs101_bcl_device->soc_tzd,
				   THERMAL_EVENT_UNSPECIFIED);

	return;
eval_exit:
	mutex_unlock(&gs101_bcl_device->state_trans_lock);
}

static int battery_supply_callback(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct power_supply *psy = data;
	struct gs101_bcl_dev *gs101_bcl_device =
			container_of(nb, struct gs101_bcl_dev, psy_nb);

	if (strcmp(psy->desc->name, "battery") == 0)
		schedule_delayed_work(&gs101_bcl_device->soc_eval_work, 0);

	return NOTIFY_OK;
}

static int gs101_bcl_soc_remove(struct gs101_bcl_dev *gs101_bcl_device)
{
	power_supply_unreg_notifier(&gs101_bcl_device->psy_nb);
	if (gs101_bcl_device->soc_tzd)
		thermal_zone_of_sensor_unregister(gs101_bcl_device->device,
						  gs101_bcl_device->soc_tzd);

	return 0;
}

static ssize_t triggered_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_device = platform_get_drvdata(pdev);
	int i, len = 0;

	len = scnprintf(buf, PAGE_SIZE, "%-10s\t%s\t%s\t%s\t%s\n",
			"Source", "Count", "Last Triggered", "Last SOC", "Last Voltage");

	for (i = 0; i < IRQ_SOURCE_S2MPG10_MAX; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"%-15s\t%d\t%lld\t\t%d\t\t%d\n",
				s2mpg10_source[i],
				atomic_read(&bcl_device->s2mpg10_cnt[i]),
				bcl_device->s2mpg10_stats[i]._time,
				bcl_device->s2mpg10_stats[i].capacity,
				bcl_device->s2mpg10_stats[i].voltage);
	}
	for (i = 0; i < IRQ_SOURCE_S2MPG11_MAX; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"%-15s\t%d\t%lld\t\t%d\t\t%d\n",
				s2mpg11_source[i],
				atomic_read(&bcl_device->s2mpg11_cnt[i]),
				bcl_device->s2mpg11_stats[i]._time,
				bcl_device->s2mpg11_stats[i].capacity,
				bcl_device->s2mpg11_stats[i].voltage);
	}

	return len;
}

static DEVICE_ATTR_RO(triggered_stats);

static void __iomem *get_addr_by_rail(struct gs101_bcl_dev *bcl_dev, const char *rail_name)
{
	int i = 0, idx;

	for (i = 0; i < 9; i++) {
		if (strcmp(rail_name, clk_ratio_source[i]) == 0) {
			idx = i > 4 ? i - 4 : i;
			if (is_subsystem_on(subsystem_pmu[idx])) {
				if (idx == 0)
					return bcl_dev->base_mem[0] + CPUCL0_CLKDIVSTEP_CON;
				if (i > 4)
					return bcl_dev->base_mem[idx] +
							CPUCL12_CLKDIVSTEP_CON_LIGHT;
				else
					return bcl_dev->base_mem[idx] +
							CPUCL12_CLKDIVSTEP_CON_HEAVY;
			} else
				return NULL;
		}
	}

	return NULL;
}

static ssize_t clk_ratio_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	int len = 0, i;
	unsigned int reg[8];
	void __iomem *addr;

	len = scnprintf(buf, PAGE_SIZE, "%-10s\t%-10s\t%-10s\t%-10s\t%-10s\t%-10s\n",
			"Source", "Reg", "Step Count[31:20]", "Step Size[19:14]",
			"Numerator [8:6]", "Denominator [2:0]");

	for (i = 0; i < 9; i++) {
		addr = get_addr_by_rail(bcl_dev, clk_ratio_source[i]);
		if (addr != NULL) {
			reg[i] = __raw_readl(addr);
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%-15s\t0x%-15x\t0x%-15x\t0x%-15x\t0x%-10x\t0x%-10x\n",
					 clk_ratio_source[i], reg[i], (reg[i] >> 20) & 0xFF,
					 (reg[i] >> 14) & 0x3F, (reg[i] >> 6) & 0x3F,
					 reg[i] & 0x3F);
		} else
			len += scnprintf(buf + len, PAGE_SIZE - len, "%-15s\t is off\n",
					 clk_ratio_source[i]);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "echo (source) (data) > clk_ratio\n");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "i.e. echo cpu1_heavy 0xF041C3 > clk_ratio\n");

	return len;
}

static ssize_t clk_ratio_store(struct device *dev,
			       struct device_attribute *attr, const char *buf, size_t size)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	char rail_name[11];
	void __iomem *addr;
	int ret;
	unsigned int value;

	/* Possible Rail name:
	 * cpu1_heavy, cpu1_light, cpu2_heavy, cpu2_light,
	 * tpu_heavy, tpu_light, gpu_heavy, gpu_light
	 */

	ret = sscanf(buf, "%s 0x%x", &rail_name, &value);
	if (ret != 2)
		return -EINVAL;

	if ((value & 0x3F) == 0) {
		dev_err(bcl_dev->device, "Denominator cannot be zero\n");
		return -EINVAL;
	}

	addr = get_addr_by_rail(bcl_dev, rail_name);

	if (addr == NULL) {
		dev_err(bcl_dev->device, "Address is NULL\n");
		return -EINVAL;
	}

	mutex_lock(&bcl_dev->ratio_lock);
	__raw_writel(value, addr);
	mutex_unlock(&bcl_dev->ratio_lock);

	return size;
}

static DEVICE_ATTR_RW(clk_ratio);

static void __iomem *get_addr_by_subsystem(struct gs101_bcl_dev *bcl_dev,
					   const char *subsystem, bool is_store)
{
	int i = 0;

	for (i = 0; i < 5; i++) {
		if (strcmp(subsystem, clk_stats_source[i]) == 0) {
			if (is_subsystem_on(subsystem_pmu[i])) {
				if (i == 0)
					return is_store ? bcl_dev->base_mem[i] + CLKDIVSTEP :
							bcl_dev->base_mem[i] +
							CPUCL0_CLKDIVSTEP_STAT;
				return is_store ? bcl_dev->base_mem[i] + CLKDIVSTEP :
						bcl_dev->base_mem[i] + CPUCL12_CLKDIVSTEP_STAT;
			} else
				return NULL;
		}
	}
	return NULL;
}

static ssize_t clk_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	int len = 0, i;
	unsigned int reg[5];
	void __iomem *addr;


	len = scnprintf(buf, PAGE_SIZE, "%s\n%-10s\t%-10s\n",
			"Configuration", "Source", "Reg");
	for (i = 0; i < 5; i++) {
		addr = get_addr_by_subsystem(bcl_dev, clk_stats_source[i], true);
		if (addr == NULL) {
			len += scnprintf(buf + len, PAGE_SIZE - len, "%-15s\t is off\n",
					 clk_stats_source[i]);
			continue;
		}
		reg[i] = __raw_readl(addr);
		len += scnprintf(buf + len, PAGE_SIZE - len, "%-15s\t0x%-15x\n",
				 clk_stats_source[i], reg[i]);
	}

	len += scnprintf(buf + len, PAGE_SIZE - len, "\nStatistics:\n");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "%-10s\t%-10s\t%-10s\t%-10s\t%-10s\t%-10s\t%-10s\n",
			 "Source", "Reg", "Step run[31]", "Trig CNT OFL[29]",
			 "Trig CNT [27:16]", "Numerator [11:8]", "Denominator [3:0]");

	for (i = 0; i < 5; i++) {
		addr = get_addr_by_subsystem(bcl_dev, clk_stats_source[i], false);
		if (addr == NULL) {
			len += scnprintf(buf + len, PAGE_SIZE - len, "%-15s\t is off\n",
					 clk_stats_source[i]);
			continue;
		}
		reg[i] = __raw_readl(addr);
		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%-15s\t0x%-15x\t0x%-10x\t0x%-10x\t0x%-10x\t0x%-10x\t0x%-10x\n",
				 clk_stats_source[i], reg[i], (reg[i] >> 31) & 0x1,
				 (reg[i] >> 29) & 0x1, (reg[i] >> 16) & 0xFFF,
				 (reg[i] >> 8) & 0x3F, reg[i] & 0xF);
	}

	len += scnprintf(buf + len, PAGE_SIZE - len, "echo (source) (data) > clk_stats\n");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "i.e. echo cpu1 0x3 > clk_stats\n");
	return len;
}

static ssize_t clk_stats_store(struct device *dev,
			       struct device_attribute *attr, const char *buf, size_t size)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	char subsystem[4];
	void __iomem *addr;
	int ret;
	unsigned int value;

	/* Possible Subsystem:
	 * cpu0, cpu1, cpu2, tpu, gpu
	 */

	ret = sscanf(buf, "%s 0x%x", &subsystem, &value);
	if (ret != 2)
		return -EINVAL;

	addr = get_addr_by_subsystem(bcl_dev, subsystem, true);

	if (addr == NULL) {
		dev_err(bcl_dev->device, "Address is NULL\n");
		return -EINVAL;
	}

	mutex_lock(&bcl_dev->ratio_lock);
	__raw_writel(value, addr);
	mutex_unlock(&bcl_dev->ratio_lock);

	return size;
}

static DEVICE_ATTR_RW(clk_stats);

static int get_smpl_lvl(void *data, u64 *val)
{
	struct gs101_bcl_dev *bcl_dev = data;
	u8 value = 0;
	unsigned int smpl_warn_lvl;

	if (!bcl_dev->s2mpg10_i2c) {
		dev_err(bcl_dev->device, "S2MPG10 I2C not found.");
		return 0;
	}
	if (s2mpg10_read_reg(bcl_dev->s2mpg10_i2c,
			     S2MPG10_PM_SMPL_WARN_CTRL, &value)) {
		dev_err(bcl_dev->device, "S2MPG10 read SMPL_WARN_CTRL failed.");
		return 0;
	}
	value >>= S2MPG10_SMPL_WARN_LVL_SHIFT;
	smpl_warn_lvl = value * 100 + SMPL_LOWER_LIMIT;
	*val = smpl_warn_lvl;
	return 0;
}

static int set_smpl_lvl(void *data, u64 val)
{
	struct gs101_bcl_dev *bcl_dev = data;
	u8 value;
	int ret;

	if (val < SMPL_LOWER_LIMIT || val > SMPL_UPPER_LIMIT) {
		dev_err(bcl_dev->device, "SMPL_WARN LEVEL %d outside of range %d - %d mV.", val,
			SMPL_LOWER_LIMIT, SMPL_UPPER_LIMIT);
		return -1;
	}

	if (s2mpg10_read_reg(bcl_dev->s2mpg10_i2c,
			     S2MPG10_PM_SMPL_WARN_CTRL, &value)) {
		dev_err(bcl_dev->device, "S2MPG10 read 0x%x failed.", S2MPG10_PM_SMPL_WARN_CTRL);
	}
	value |= ((val - SMPL_LOWER_LIMIT) / 100) << S2MPG10_SMPL_WARN_LVL_SHIFT;
	ret = s2mpg10_write_reg(bcl_dev->s2mpg10_i2c,
				S2MPG10_PM_SMPL_WARN_CTRL, value);

	if (ret)
		dev_err(bcl_dev->device, "i2c write error setting smpl_warn\n");
	else {
		bcl_dev->s2mpg10_lvl[IRQ_SMPL_WARN] = SMPL_BATTERY_VOLTAGE - val -
				THERMAL_HYST_LEVEL;
		ret = bcl_dev->s2mpg10_tz_irq[IRQ_SMPL_WARN]->ops->set_trip_temp(
				bcl_dev->s2mpg10_tz_irq[IRQ_SMPL_WARN], 0,
				SMPL_BATTERY_VOLTAGE - val);
		if (ret)
			dev_err(bcl_dev->device, "Fail to set smpl_warn trip temp\n");

	}

	return ret;

}

DEFINE_SIMPLE_ATTRIBUTE(smpl_lvl_fops, get_smpl_lvl, set_smpl_lvl, "%d\n");

static int get_ocp_lvl(void *data, u64 *val, u8 addr,
		       u8 pmic, u8 mask, u16 limit,
		       u8 step)
{
	struct gs101_bcl_dev *bcl_dev = data;
	u8 value = 0;
	unsigned int ocp_warn_lvl;

	if (pmic == S2MPG10) {
		if (s2mpg10_read_reg(bcl_dev->s2mpg10_i2c, addr, &value)) {
			dev_err(bcl_dev->device, "S2MPG10 read 0x%x failed.", addr);
			return -1;
		}
	} else {
		if (s2mpg11_read_reg(bcl_dev->s2mpg11_i2c, addr, &value)) {
			dev_err(bcl_dev->device, "S2MPG11 read 0x%x failed.", addr);
			return -1;
		}
	}
	value &= mask;
	ocp_warn_lvl = limit - value * step;
	*val = ocp_warn_lvl;
	return 0;
}

static int set_ocp_lvl(void *data, u64 val, u8 addr, u8 pmic, u8 mask,
		       u16 llimit, u16 ulimit, u8 step, u8 id)
{
	struct gs101_bcl_dev *bcl_dev = data;
	u8 value;
	int ret;

	if (val < llimit || val > ulimit) {
		dev_err(bcl_dev->device, "OCP_WARN LEVEL %d outside of range %d - %d mA.", val,
		       llimit, ulimit);
		return -1;
	}
	if (pmic == S2MPG10) {
		mutex_lock(&bcl_dev->s2mpg10_irq_lock[id]);
		if (s2mpg10_read_reg(bcl_dev->s2mpg10_i2c, addr, &value)) {
			dev_err(bcl_dev->device, "S2MPG10 read 0x%x failed.", addr);
			mutex_unlock(&bcl_dev->s2mpg10_irq_lock[id]);
			return -1;
		}
		value &= ~(OCP_WARN_MASK) << S2MPG10_OCP_WARN_LVL_SHIFT;
		value |= ((ulimit - val) / step) << S2MPG10_OCP_WARN_LVL_SHIFT;
		ret = s2mpg10_write_reg(bcl_dev->s2mpg10_i2c, addr, value);
		if (!ret) {
			bcl_dev->s2mpg10_lvl[id] = val - THERMAL_HYST_LEVEL;
			ret = bcl_dev->s2mpg10_tz_irq[id]->ops->set_trip_temp(
					bcl_dev->s2mpg10_tz_irq[id], 0,
					val);
			if (ret)
				dev_err(bcl_dev->device, "Fail to set ocp_warn trip temp\n");
		}
		mutex_unlock(&bcl_dev->s2mpg10_irq_lock[id]);
	} else {
		mutex_lock(&bcl_dev->s2mpg11_irq_lock[id]);
		if (s2mpg11_read_reg(bcl_dev->s2mpg11_i2c, addr, &value)) {
			dev_err(bcl_dev->device, "S2MPG11 read 0x%x failed.", addr);
			mutex_unlock(&bcl_dev->s2mpg11_irq_lock[id]);
			return -1;
		}
		value &= ~(OCP_WARN_MASK) << S2MPG10_OCP_WARN_LVL_SHIFT;
		value |= ((ulimit - val) / step) << S2MPG11_OCP_WARN_LVL_SHIFT;
		ret = s2mpg11_write_reg(bcl_dev->s2mpg11_i2c, addr, value);
		if (!ret) {
			bcl_dev->s2mpg11_lvl[id] = val - THERMAL_HYST_LEVEL;
			ret = bcl_dev->s2mpg11_tz_irq[id]->ops->set_trip_temp(
					bcl_dev->s2mpg11_tz_irq[id], 0,
					val);
			if (ret)
				dev_err(bcl_dev->device, "Fail to set ocp_warn trip temp\n");
		}
		mutex_unlock(&bcl_dev->s2mpg11_irq_lock[id]);
	}

	if (ret)
		dev_err(bcl_dev->device, "i2c write error setting smpl_warn\n");

	return ret;
}

static int get_soft_cpu1_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG10_PM_B3M_SOFT_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B3M_UPPER_LIMIT, B3M_STEP);
}

static int set_soft_cpu1_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG10_PM_B3M_SOFT_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B3M_LOWER_LIMIT, B3M_UPPER_LIMIT,
			   B3M_STEP, IRQ_SOFT_OCP_WARN_CPUCL1);
}

DEFINE_SIMPLE_ATTRIBUTE(soft_cpu1_lvl_fops, get_soft_cpu1_lvl, set_soft_cpu1_lvl, "%d\n");

static int get_soft_cpu2_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG10_PM_B2M_SOFT_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B2M_UPPER_LIMIT, B2M_STEP);
}

static int set_soft_cpu2_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG10_PM_B2M_SOFT_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B2M_LOWER_LIMIT, B2M_UPPER_LIMIT,
			   B2M_STEP, IRQ_SOFT_OCP_WARN_CPUCL2);
}

DEFINE_SIMPLE_ATTRIBUTE(soft_cpu2_lvl_fops, get_soft_cpu2_lvl, set_soft_cpu2_lvl, "%d\n");

static int get_cpu1_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG10_PM_B3M_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B3M_UPPER_LIMIT, B3M_STEP);
}

static int set_cpu1_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG10_PM_B3M_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B3M_LOWER_LIMIT, B3M_UPPER_LIMIT,
			   B3M_STEP, IRQ_OCP_WARN_CPUCL1);
}

DEFINE_SIMPLE_ATTRIBUTE(cpu1_lvl_fops, get_cpu1_lvl, set_cpu1_lvl, "%d\n");

static int get_cpu2_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG10_PM_B2M_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B2M_UPPER_LIMIT, B2M_STEP);
}

static int set_cpu2_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG10_PM_B2M_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B2M_LOWER_LIMIT, B2M_UPPER_LIMIT,
			   B2M_STEP, IRQ_OCP_WARN_CPUCL2);
}

DEFINE_SIMPLE_ATTRIBUTE(cpu2_lvl_fops, get_cpu2_lvl, set_cpu2_lvl, "%d\n");

static int get_tpu_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG10_PM_B10M_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B10M_UPPER_LIMIT, B10M_STEP);
}

static int set_tpu_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG10_PM_B10M_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B10M_LOWER_LIMIT, B10M_UPPER_LIMIT,
			   B10M_STEP, IRQ_OCP_WARN_TPU);
}

DEFINE_SIMPLE_ATTRIBUTE(tpu_lvl_fops, get_tpu_lvl, set_tpu_lvl, "%d\n");

static int get_soft_tpu_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG10_PM_B10M_SOFT_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B10M_UPPER_LIMIT, B10M_STEP);
}

static int set_soft_tpu_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG10_PM_B10M_SOFT_OCP_WARN, S2MPG10,
			   OCP_WARN_MASK, B10M_LOWER_LIMIT, B10M_UPPER_LIMIT,
			   B10M_STEP, IRQ_SOFT_OCP_WARN_TPU);
}

DEFINE_SIMPLE_ATTRIBUTE(soft_tpu_lvl_fops, get_soft_tpu_lvl, set_soft_tpu_lvl, "%d\n");

static int get_gpu_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG11_PM_B2S_OCP_WARN, S2MPG11,
			   OCP_WARN_MASK, B2S_UPPER_LIMIT, B2S_STEP);
}

static int set_gpu_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG11_PM_B2S_OCP_WARN, S2MPG11,
			   OCP_WARN_MASK, B2S_LOWER_LIMIT, B2S_UPPER_LIMIT,
			   B2S_STEP, IRQ_OCP_WARN_GPU);
}

DEFINE_SIMPLE_ATTRIBUTE(gpu_lvl_fops, get_gpu_lvl, set_gpu_lvl, "%d\n");

static int get_soft_gpu_lvl(void *data, u64 *val)
{
	return get_ocp_lvl(data, val, S2MPG11_PM_B2S_SOFT_OCP_WARN, S2MPG11,
			   OCP_WARN_MASK, B2S_UPPER_LIMIT, B2S_STEP);
}

static int set_soft_gpu_lvl(void *data, u64 val)
{
	return set_ocp_lvl(data, val, S2MPG11_PM_B2S_SOFT_OCP_WARN, S2MPG11,
			   OCP_WARN_MASK, B2S_LOWER_LIMIT, B2S_UPPER_LIMIT,
			   B2S_STEP, IRQ_SOFT_OCP_WARN_GPU);
}

DEFINE_SIMPLE_ATTRIBUTE(soft_gpu_lvl_fops, get_soft_gpu_lvl, set_soft_gpu_lvl, "%d\n");

static void gs101_set_ppm_throttling(struct gs101_bcl_dev *gs101_bcl_device,
				     enum sys_throttling_switch throttle_switch)
{
	unsigned int reg, mask;
	void __iomem *addr;

	if (!gs101_bcl_device->sysreg_cpucl0) {
		dev_err(gs101_bcl_device->device, "sysreg_cpucl0 ioremap not mapped\n");
		return;
	}
	mutex_lock(&sysreg_lock);
	addr = gs101_bcl_device->sysreg_cpucl0 + CLUSTER0_PPM;
	reg = __raw_readl(addr);
	mask = BIT(8);
	/* 75% dispatch reduction */
	if (throttle_switch == SYS_THROTTLING_ENABLED) {
		reg |= mask;
		reg |= PPMCTL_MASK;
	} else {
		reg &= ~mask;
		reg &= ~(PPMCTL_MASK);
	}
	__raw_writel(reg, addr);
	mutex_unlock(&sysreg_lock);
}

static ssize_t mpmm_settings_store(struct device *dev,
				   struct device_attribute *attr, const char *buf, size_t size)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	void __iomem *addr;
	int value;
	int ret;

	ret = sscanf(buf, "0x%x", &value);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&sysreg_lock);
	addr = bcl_dev->sysreg_cpucl0 + CLUSTER0_MPMM;
	__raw_writel(value, addr);
	mutex_unlock(&sysreg_lock);

	return size;
}

static const char *mpmm_gear_parse(unsigned int state)
{
	switch (state) {
	case 0x0:
		return "GEAR 0";
	case 0x1:
		return "GEAR 1";
	case 0x2:
		return "GEAR 2";
	case 0x3:
	default:
		return "DISABLED";
	}
}

static ssize_t mpmm_settings_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	unsigned int reg = 0;
	unsigned int state_big7, state_big6;
	void __iomem *addr;

	if (!bcl_dev->sysreg_cpucl0)
		return scnprintf(buf, BUF_SIZE, "sysreg_cpucl0 memory is not accessible.\n\n");

	mutex_lock(&sysreg_lock);
	addr = bcl_dev->sysreg_cpucl0 + CLUSTER0_MPMM;
	reg = __raw_readl(addr);
	mutex_unlock(&sysreg_lock);
	state_big7 = (reg >> 2) & 0x3;
	state_big6  = reg & 0x3;
	return scnprintf(buf, BUF_SIZE,
			 "Reg: 0x%x\n%s: 0x%x\n%s: 0x%x,%s\n%s: 0x%x,%s\n%s:\n%s\n\n",
			 reg, "CAPTURE_ENABLE [4]", reg >> 4,
			 "MPMMSTATE_BIG7 [3:2]", state_big7, mpmm_gear_parse(state_big7),
			 "MPMMSTATE_BIG6 [1:0]", state_big6, mpmm_gear_parse(state_big6),
			 "To set", "echo 0x(value) > mpmm_settings");
}

static DEVICE_ATTR_RW(mpmm_settings);

static void
gs101_set_mpmm_throttling(struct gs101_bcl_dev *gs101_bcl_device,
			  enum sys_throttling_switch throttle_switch)
{
	unsigned int reg;
	void __iomem *addr;
	unsigned int settings;

	if (!gs101_bcl_device->sysreg_cpucl0) {
		dev_err(gs101_bcl_device->device, "sysreg_cpucl0 ioremap not mapped\n");
		return;
	}
	mutex_lock(&sysreg_lock);
	addr = gs101_bcl_device->sysreg_cpucl0 + CLUSTER0_MPMM;
	reg = __raw_readl(addr);

	if ((throttle_switch < 0) || (throttle_switch > SYS_THROTTLING_GEAR2))
		settings = 0xF;
	else
		settings = sys_throttling_settings[throttle_switch];

	reg &= ~0x1F;
	reg |= settings;
	__raw_writel(reg, addr);
	mutex_unlock(&sysreg_lock);
}

static ssize_t ppm_settings_store(struct device *dev,
				  struct device_attribute *attr, const char *buf, size_t size)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	void __iomem *addr;
	int value;
	int ret;

	ret = sscanf(buf, "0x%x", &value);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&sysreg_lock);
	addr = bcl_dev->sysreg_cpucl0 + CLUSTER0_PPM;
	__raw_writel(value, addr);
	mutex_unlock(&sysreg_lock);

	return size;
}

static ssize_t ppm_settings_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gs101_bcl_dev *bcl_dev = platform_get_drvdata(pdev);
	unsigned int reg = 0;
	unsigned int ppmctl7, ppmctl6, ocp_en_big;
	void __iomem *addr;

	if (!bcl_dev->sysreg_cpucl0)
		return scnprintf(buf, BUF_SIZE, "sysreg_cpucl0 memory is not accessible.\n\n");

	mutex_lock(&sysreg_lock);
	addr = bcl_dev->sysreg_cpucl0 + CLUSTER0_PPM;
	reg = __raw_readl(addr);
	mutex_unlock(&sysreg_lock);
	ocp_en_big = test_bit(8, (unsigned long *)&reg);
	ppmctl7 = (reg >> 4) & 0xF;
	ppmctl6 = reg & 0xF;
	return scnprintf(buf, BUF_SIZE,
			 "0x%x\n%s: %d\n%s: 0x%x\n%s: 0x%x\n%s:\n%s\n\n",
			 reg, "PPMOCP_EN_BIG [8]", ocp_en_big,
			 "PPMCTL7 BIG1 [7:4]", ppmctl7, "PPMCTL6 BIG0 [3:0]", ppmctl6,
			 "To set", "echo 0x(value) > ppm_settings");

}

static DEVICE_ATTR_RW(ppm_settings);

static int gs101_bcl_register_pmic_irq(struct gs101_bcl_dev *gs101_bcl_device,
				  int id, int sensor_id, irq_handler_t thread_fn,
				  struct device *dev,
				  const struct thermal_zone_of_device_ops *ops,
				  const char *devname, u8 pmic, u32 intr_flag)
{
	int ret = 0;

	ret = devm_request_threaded_irq(gs101_bcl_device->device,
					gs101_bcl_device->s2mpg10_irq[id],
					NULL, thread_fn,
					intr_flag | IRQF_ONESHOT,
					devname, gs101_bcl_device);
	if (ret < 0) {
		dev_err(gs101_bcl_device->device, "Failed to request IRQ: %d: %d\n",
			gs101_bcl_device->s2mpg10_irq[id], ret);
		return ret;
	}
	if (ops) {
		gs101_bcl_device->s2mpg10_tz_irq[id] =
				thermal_zone_of_sensor_register(dev, sensor_id,
					gs101_bcl_device,
								ops);
		if (IS_ERR(gs101_bcl_device->s2mpg10_tz_irq[id])) {
			dev_err(gs101_bcl_device->device,
				"PMIC TZ register failed. %d, err:%ld\n", id,
				PTR_ERR(gs101_bcl_device->s2mpg10_tz_irq[id]));
		} else {
			thermal_zone_device_enable(gs101_bcl_device->s2mpg10_tz_irq[id]);
			thermal_zone_device_update(gs101_bcl_device->s2mpg10_tz_irq[id],
			   THERMAL_DEVICE_UP);
		}
	}
	return ret;
}

static int gs101_bcl_register_irq(struct gs101_bcl_dev *gs101_bcl_device,
				  int id, irq_handler_t thread_fn,
				  struct device *dev,
				  const struct thermal_zone_of_device_ops *ops,
				  const char *devname, u8 pmic, u32 intr_flag)
{
	int ret = 0;

	if (pmic == S2MPG10) {
		ret = devm_request_threaded_irq(gs101_bcl_device->device,
						gs101_bcl_device->s2mpg10_irq[id],
						NULL, thread_fn,
						intr_flag | IRQF_ONESHOT,
						devname, gs101_bcl_device);
		if (ret < 0) {
			dev_err(gs101_bcl_device->device,
				"Failed to request IRQ: %d: %d\n",
				gs101_bcl_device->s2mpg10_irq[id], ret);
			return ret;
		}
		if (ops) {
			gs101_bcl_device->s2mpg10_tz_irq[id] =
					thermal_zone_of_sensor_register(dev, id,
									gs101_bcl_device,
									ops);
			if (IS_ERR(gs101_bcl_device->s2mpg10_tz_irq[id])) {
				dev_err(gs101_bcl_device->device,
					"TZ register failed. %d, err:%ld\n", id,
					PTR_ERR(gs101_bcl_device->s2mpg10_tz_irq[id]));
			} else {
				thermal_zone_device_enable(gs101_bcl_device->s2mpg10_tz_irq[id]);
				thermal_zone_device_update(gs101_bcl_device->s2mpg10_tz_irq[id],
				   THERMAL_DEVICE_UP);
			}
		}
	} else {
		ret = devm_request_threaded_irq(gs101_bcl_device->device,
						gs101_bcl_device->s2mpg11_irq[id],
						NULL, thread_fn,
						intr_flag | IRQF_ONESHOT,
						devname, gs101_bcl_device);
		if (ret < 0) {
			dev_err(gs101_bcl_device->device,
				"Failed to request IRQ: %d: %d\n",
				gs101_bcl_device->s2mpg11_irq[id], ret);
			return ret;
		}
		if (ops) {
			gs101_bcl_device->s2mpg11_tz_irq[id] =
					thermal_zone_of_sensor_register(dev, id,
									gs101_bcl_device,
									ops);
			if (IS_ERR(gs101_bcl_device->s2mpg11_tz_irq[id])) {
				dev_err(gs101_bcl_device->device,
					"TZ register failed. %d, err:%ld\n", id,
					PTR_ERR(gs101_bcl_device->s2mpg11_tz_irq[id]));
			} else {
				thermal_zone_device_enable(gs101_bcl_device->s2mpg11_tz_irq[id]);
				thermal_zone_device_update(gs101_bcl_device->s2mpg11_tz_irq[id],
				   THERMAL_DEVICE_UP);
			}
		}
	}
	return ret;
}

static int google_gs101_set_sub_pmic(struct gs101_bcl_dev *bcl_dev)
{
	struct s2mpg11_platform_data *pdata_s2mpg11;
	struct device_node *p_np;
	struct device_node *np = bcl_dev->device->of_node;
	struct s2mpg11_dev *s2mpg11 = NULL;
	struct i2c_client *i2c;
	u8 val = 0;
	int ret;

	p_np = of_parse_phandle(np, "google,sub-power", 0);
	if (p_np) {
		i2c = of_find_i2c_device_by_node(p_np);
		if (!i2c) {
			dev_err(bcl_dev->device, "Cannot find sub-power I2C\n");
			return -ENODEV;
		}
		s2mpg11 = i2c_get_clientdata(i2c);
	}
	of_node_put(p_np);
	if (!s2mpg11) {
		dev_err(bcl_dev->device, "S2MPG11 device not found\n");
		return -ENODEV;
	}
	pdata_s2mpg11 = dev_get_platdata(s2mpg11->dev);
	bcl_dev->s2mpg11_i2c = s2mpg11->pmic;
	bcl_dev->s2mpg11_lvl[IRQ_OCP_WARN_GPU] = B2S_UPPER_LIMIT - THERMAL_HYST_LEVEL -
			(pdata_s2mpg11->b2_ocp_warn_lvl * B2S_STEP);
	bcl_dev->s2mpg11_lvl[IRQ_SOFT_OCP_WARN_GPU] = B2S_UPPER_LIMIT - THERMAL_HYST_LEVEL -
			(pdata_s2mpg11->b2_soft_ocp_warn_lvl * B2S_STEP);
	bcl_dev->s2mpg11_pin[IRQ_OCP_WARN_GPU] = pdata_s2mpg11->b2_ocp_warn_pin;
	bcl_dev->s2mpg11_pin[IRQ_SOFT_OCP_WARN_GPU] = pdata_s2mpg11->b2_soft_ocp_warn_pin;
	bcl_dev->s2mpg11_irq[IRQ_OCP_WARN_GPU] = gpio_to_irq(pdata_s2mpg11->b2_ocp_warn_pin);
	bcl_dev->s2mpg11_irq[IRQ_SOFT_OCP_WARN_GPU] =
			gpio_to_irq(pdata_s2mpg11->b2_soft_ocp_warn_pin);
	if (s2mpg11_read_reg(bcl_dev->s2mpg11_i2c, S2MPG11_COMMON_CHIPID, &val)) {
		dev_err(bcl_dev->device, "Failed to read S2MPG11 chipid.\n");
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_OCP_WARN_GPU,
				     gs101_gpu_ocp_warn_irq_handler,
				     s2mpg11->dev,
				     &gs101_ocp_gpu_ops, "GPU_OCP_IRQ",
				     S2MPG11, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_OCP_WARN_GPU);
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_SOFT_OCP_WARN_GPU,
				     gs101_soft_gpu_ocp_warn_irq_handler,
				     s2mpg11->dev,
				     &gs101_soft_ocp_gpu_ops, "SOFT_GPU_OCP_IRQ",
				     S2MPG11, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_SOFT_OCP_WARN_GPU);
		return -ENODEV;
	}
	debugfs_create_file("soft_ocp_gpu_lvl", 0644,
			    bcl_dev->debug_entry,
			    bcl_dev, &soft_gpu_lvl_fops);

	debugfs_create_file("ocp_gpu_lvl", 0644,
			    bcl_dev->debug_entry,
			    bcl_dev, &gpu_lvl_fops);
	return 0;
}

static int google_gs101_set_main_pmic(struct gs101_bcl_dev *bcl_dev)
{
	struct s2mpg10_platform_data *pdata_s2mpg10;
	struct device_node *p_np;
	struct device_node *np = bcl_dev->device->of_node;
	struct s2mpg10_dev *s2mpg10 = NULL;
	struct i2c_client *i2c;
	bool bypass_smpl_warn = false;
	u8 val = 0;
	int ret;

	p_np = of_parse_phandle(np, "google,main-power", 0);
	if (p_np) {
		i2c = of_find_i2c_device_by_node(p_np);
		if (!i2c) {
			dev_err(bcl_dev->device, "Cannot find main-power I2C\n");
			return -ENODEV;
		}
		s2mpg10 = i2c_get_clientdata(i2c);
	}
	of_node_put(p_np);
	if (!s2mpg10) {
		dev_err(bcl_dev->device, "S2MPG10 device not found\n");
		return -ENODEV;
	}
	pdata_s2mpg10 = dev_get_platdata(s2mpg10->dev);
	/* request smpl_warn interrupt */
	if (!gpio_is_valid(pdata_s2mpg10->smpl_warn_pin)) {
		dev_err(bcl_dev->device, "smpl_warn GPIO NOT VALID\n");
		devm_free_irq(bcl_dev->device, bcl_dev->s2mpg10_irq[IRQ_SMPL_WARN], bcl_dev);
		bypass_smpl_warn = true;
	}
	bcl_dev->s2mpg10_i2c = s2mpg10->pmic;
	bcl_dev->s2mpg10_irq[IRQ_SMPL_WARN] = gpio_to_irq(pdata_s2mpg10->smpl_warn_pin);
	irq_set_status_flags(bcl_dev->s2mpg10_irq[IRQ_SMPL_WARN], IRQ_DISABLE_UNLAZY);
	bcl_dev->s2mpg10_pin[IRQ_SMPL_WARN] = pdata_s2mpg10->smpl_warn_pin;
	bcl_dev->s2mpg10_lvl[IRQ_SMPL_WARN] = SMPL_BATTERY_VOLTAGE -
			(pdata_s2mpg10->smpl_warn_lvl * SMPL_STEP + SMPL_LOWER_LIMIT);
	bcl_dev->s2mpg10_lvl[IRQ_OCP_WARN_CPUCL1] = B3M_UPPER_LIMIT -
			THERMAL_HYST_LEVEL - (pdata_s2mpg10->b3_ocp_warn_lvl * B3M_STEP);
	bcl_dev->s2mpg10_lvl[IRQ_SOFT_OCP_WARN_CPUCL1] = B3M_UPPER_LIMIT -
			THERMAL_HYST_LEVEL - (pdata_s2mpg10->b3_soft_ocp_warn_lvl * B3M_STEP);
	bcl_dev->s2mpg10_lvl[IRQ_OCP_WARN_CPUCL2] = B2M_UPPER_LIMIT -
			THERMAL_HYST_LEVEL - (pdata_s2mpg10->b2_ocp_warn_lvl * B2M_STEP);
	bcl_dev->s2mpg10_lvl[IRQ_SOFT_OCP_WARN_CPUCL2] = B2M_UPPER_LIMIT -
			THERMAL_HYST_LEVEL - (pdata_s2mpg10->b2_soft_ocp_warn_lvl * B2M_STEP);
	bcl_dev->s2mpg10_lvl[IRQ_OCP_WARN_TPU] = B10M_UPPER_LIMIT -
			THERMAL_HYST_LEVEL - (pdata_s2mpg10->b10_ocp_warn_lvl * B10M_STEP);
	bcl_dev->s2mpg10_lvl[IRQ_SOFT_OCP_WARN_TPU] = B10M_UPPER_LIMIT -
			THERMAL_HYST_LEVEL - (pdata_s2mpg10->b10_soft_ocp_warn_lvl * B10M_STEP);
	bcl_dev->s2mpg10_lvl[IRQ_PMIC_120C] = PMIC_120C_UPPER_LIMIT - THERMAL_HYST_LEVEL;
	bcl_dev->s2mpg10_lvl[IRQ_PMIC_140C] = PMIC_140C_UPPER_LIMIT - THERMAL_HYST_LEVEL;
	bcl_dev->s2mpg10_lvl[IRQ_PMIC_OVERHEAT] = PMIC_OVERHEAT_UPPER_LIMIT - THERMAL_HYST_LEVEL;
	bcl_dev->s2mpg10_pin[IRQ_OCP_WARN_CPUCL1] = pdata_s2mpg10->b3_ocp_warn_pin;
	bcl_dev->s2mpg10_pin[IRQ_OCP_WARN_CPUCL2] = pdata_s2mpg10->b2_ocp_warn_pin;
	bcl_dev->s2mpg10_pin[IRQ_SOFT_OCP_WARN_CPUCL1] = pdata_s2mpg10->b3_soft_ocp_warn_pin;
	bcl_dev->s2mpg10_pin[IRQ_SOFT_OCP_WARN_CPUCL2] = pdata_s2mpg10->b2_soft_ocp_warn_pin;
	bcl_dev->s2mpg10_pin[IRQ_OCP_WARN_TPU] = pdata_s2mpg10->b10_ocp_warn_pin;
	bcl_dev->s2mpg10_pin[IRQ_SOFT_OCP_WARN_TPU] = pdata_s2mpg10->b10_soft_ocp_warn_pin;
	bcl_dev->s2mpg10_irq[IRQ_OCP_WARN_CPUCL1] = gpio_to_irq(pdata_s2mpg10->b3_ocp_warn_pin);
	bcl_dev->s2mpg10_irq[IRQ_OCP_WARN_CPUCL2] = gpio_to_irq(pdata_s2mpg10->b2_ocp_warn_pin);
	bcl_dev->s2mpg10_irq[IRQ_SOFT_OCP_WARN_CPUCL1] =
			gpio_to_irq(pdata_s2mpg10->b3_soft_ocp_warn_pin);
	bcl_dev->s2mpg10_irq[IRQ_SOFT_OCP_WARN_CPUCL2] =
			gpio_to_irq(pdata_s2mpg10->b2_soft_ocp_warn_pin);
	bcl_dev->s2mpg10_irq[IRQ_OCP_WARN_TPU] = gpio_to_irq(pdata_s2mpg10->b10_ocp_warn_pin);
	bcl_dev->s2mpg10_irq[IRQ_SOFT_OCP_WARN_TPU] =
			gpio_to_irq(pdata_s2mpg10->b10_soft_ocp_warn_pin);
	bcl_dev->s2mpg10_irq[IRQ_PMIC_120C] = pdata_s2mpg10->irq_base + S2MPG10_IRQ_120C_INT3;
	bcl_dev->s2mpg10_irq[IRQ_PMIC_140C] = pdata_s2mpg10->irq_base + S2MPG10_IRQ_140C_INT3;
	bcl_dev->s2mpg10_irq[IRQ_PMIC_OVERHEAT] = pdata_s2mpg10->irq_base + S2MPG10_IRQ_TSD_INT3;
	if (s2mpg10_read_reg(bcl_dev->s2mpg10_i2c, S2MPG10_COMMON_CHIPID, &val)) {
		dev_err(bcl_dev->device, "Failed to read S2MPG10 chipid.\n");
		return -ENODEV;
	}
	if (!bypass_smpl_warn) {
		ret = gs101_bcl_register_irq(bcl_dev,
					     IRQ_SMPL_WARN,
					     gs101_smpl_warn_irq_handler,
					     s2mpg10->dev,
					     &gs101_smpl_warn_ops,
					     "SMPL_WARN_IRQ",
					     S2MPG10, IRQF_TRIGGER_LOW);
		if (ret < 0) {
			dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_SMPL_WARN);
			return -ENODEV;
		}
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_OCP_WARN_CPUCL1,
				     gs101_cpu1_ocp_warn_irq_handler,
				     s2mpg10->dev,
				     &gs101_ocp_cpu1_ops, "CPU1_OCP_IRQ",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_OCP_WARN_CPUCL1);
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_OCP_WARN_CPUCL2,
				     gs101_cpu2_ocp_warn_irq_handler,
				     s2mpg10->dev,
				     &gs101_ocp_cpu2_ops, "CPU2_OCP_IRQ",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_OCP_WARN_CPUCL2);
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_SOFT_OCP_WARN_CPUCL1,
				     gs101_soft_cpu1_ocp_warn_irq_handler,
				     s2mpg10->dev,
				     &gs101_soft_ocp_cpu1_ops, "SOFT_CPU1_OCP_IRQ",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_SOFT_OCP_WARN_CPUCL1);
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_SOFT_OCP_WARN_CPUCL2,
				     gs101_soft_cpu2_ocp_warn_irq_handler,
				     s2mpg10->dev,
				     &gs101_soft_ocp_cpu2_ops, "SOFT_CPU2_OCP_IRQ",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_SOFT_OCP_WARN_CPUCL2);
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_OCP_WARN_TPU,
				     gs101_tpu_ocp_warn_irq_handler,
				     s2mpg10->dev,
				     &gs101_ocp_tpu_ops, "TPU_OCP_IRQ",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_OCP_WARN_TPU);
		return -ENODEV;
	}
	ret = gs101_bcl_register_irq(bcl_dev,
				     IRQ_SOFT_OCP_WARN_TPU,
				     gs101_soft_tpu_ocp_warn_irq_handler,
				     s2mpg10->dev,
				     &gs101_soft_ocp_tpu_ops, "SOFT_TPU_OCP_IRQ",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail:%d\n", IRQ_SOFT_OCP_WARN_TPU);
		return -ENODEV;
	}
	ret = gs101_bcl_register_pmic_irq(bcl_dev,
				     IRQ_PMIC_120C, PMIC_120C,
				     gs101_pmic_120c_irq_handler,
				     bcl_dev->device,
				     &gs101_pmic_120c_ops, "PMIC_120C",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_pmic_register fail:%d\n", IRQ_PMIC_120C);
		return -ENODEV;
	}
	ret = gs101_bcl_register_pmic_irq(bcl_dev,
				     IRQ_PMIC_140C, PMIC_140C,
				     gs101_pmic_140c_irq_handler,
				     bcl_dev->device,
				     &gs101_pmic_140c_ops, "PMIC_140C",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_pmic_register fail:%d\n", IRQ_PMIC_140C);
		return -ENODEV;
	}
	ret = gs101_bcl_register_pmic_irq(bcl_dev,
				     IRQ_PMIC_OVERHEAT, PMIC_OVERHEAT,
				     gs101_tsd_overheat_irq_handler,
				     bcl_dev->device,
				     &gs101_pmic_overheat_ops, "THERMAL_OVERHEAT",
				     S2MPG10, IRQF_TRIGGER_HIGH);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_pmic_register fail:%d\n", IRQ_PMIC_OVERHEAT);
		return -ENODEV;
	}
	debugfs_create_file("smpl_lvl", 0644, bcl_dev->debug_entry, bcl_dev, &smpl_lvl_fops);
	debugfs_create_file("soft_ocp_cpu1_lvl", 0644, bcl_dev->debug_entry,
			    bcl_dev, &soft_cpu1_lvl_fops);
	debugfs_create_file("soft_ocp_cpu2_lvl", 0644, bcl_dev->debug_entry,
			    bcl_dev, &soft_cpu2_lvl_fops);
	debugfs_create_file("soft_ocp_tpu_lvl", 0644, bcl_dev->debug_entry,
			    bcl_dev, &soft_tpu_lvl_fops);
	debugfs_create_file("ocp_cpu1_lvl", 0644, bcl_dev->debug_entry, bcl_dev, &cpu1_lvl_fops);
	debugfs_create_file("ocp_cpu2_lvl", 0644, bcl_dev->debug_entry, bcl_dev, &cpu2_lvl_fops);
	debugfs_create_file("ocp_tpu_lvl", 0644, bcl_dev->debug_entry, bcl_dev, &tpu_lvl_fops);

	return 0;

}

static int google_gs101_bcl_probe(struct platform_device *pdev)
{
	unsigned int reg;
	int ret = 0, i;
	struct gs101_bcl_dev *bcl_dev;
	struct dentry *root;

	bcl_dev = devm_kzalloc(&pdev->dev, sizeof(*bcl_dev), GFP_KERNEL);
	if (!bcl_dev)
		return -ENOMEM;
	bcl_dev->device = &pdev->dev;
	bcl_dev->iodev = dev_get_drvdata(pdev->dev.parent);
	platform_set_drvdata(pdev, bcl_dev);
	bcl_dev->psy_nb.notifier_call = battery_supply_callback;
	ret = power_supply_reg_notifier(&bcl_dev->psy_nb);
	if (ret < 0) {
		dev_err(bcl_dev->device, "soc notifier registration error. defer. err:%d\n", ret);
		ret = -EPROBE_DEFER;
		goto bcl_soc_probe_exit;
	}
	bcl_dev->soc_tzd = thermal_zone_of_sensor_register(
		bcl_dev->device, PMIC_SOC, bcl_dev,
		&bcl_dev->soc_ops);
	if (IS_ERR(bcl_dev->soc_tzd)) {
		dev_err(bcl_dev->device, "soc TZ register failed. err:%ld\n",
			PTR_ERR(bcl_dev->soc_tzd));
		ret = PTR_ERR(bcl_dev->soc_tzd);
		bcl_dev->soc_tzd = NULL;
		ret = -EPROBE_DEFER;
		goto bcl_soc_probe_exit;
	}
	INIT_DELAYED_WORK(&bcl_dev->soc_eval_work, gs101_bcl_evaluate_soc);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_SMPL_WARN], gs101_smpl_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_OCP_WARN_CPUCL1], gs101_cpu1_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_CPUCL1],
			  gs101_soft_cpu1_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_OCP_WARN_CPUCL2], gs101_cpu2_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_CPUCL2],
			  gs101_soft_cpu2_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_OCP_WARN_TPU], gs101_tpu_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_SOFT_OCP_WARN_TPU],
			  gs101_soft_tpu_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_PMIC_120C], gs101_pmic_120c_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_PMIC_140C], gs101_pmic_140c_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg10_irq_work[IRQ_PMIC_OVERHEAT], gs101_pmic_overheat_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg11_irq_work[IRQ_OCP_WARN_GPU], gs101_gpu_warn_work);
	INIT_DELAYED_WORK(&bcl_dev->s2mpg11_irq_work[IRQ_SOFT_OCP_WARN_GPU],
			  gs101_soft_gpu_warn_work);
	root = debugfs_lookup("gs101-bcl", NULL);
	if (!root) {
		bcl_dev->debug_entry = debugfs_create_dir("gs101-bcl", 0);
		if (IS_ERR_OR_NULL(bcl_dev->debug_entry)) {
			bcl_dev->debug_entry = NULL;
			return -EINVAL;
		}
	} else
		bcl_dev->debug_entry = root;
	bcl_dev->base_mem[0] = ioremap(CPUCL0_BASE, SZ_8K);
	if (!bcl_dev->base_mem[0]) {
		dev_err(bcl_dev->device, "cpu0_mem ioremap failed\n");
		ret = -EIO;
		goto bcl_soc_probe_exit;
	}
	bcl_dev->base_mem[1] = ioremap(CPUCL1_BASE, SZ_8K);
	if (!bcl_dev->base_mem[1]) {
		dev_err(bcl_dev->device, "cpu1_mem ioremap failed\n");
		ret = -EIO;
		goto bcl_soc_probe_exit;
	}
	bcl_dev->base_mem[2] = ioremap(CPUCL2_BASE, SZ_8K);
	if (!bcl_dev->base_mem[2]) {
		dev_err(bcl_dev->device, "cpu2_mem ioremap failed\n");
		ret = -EIO;
		goto bcl_soc_probe_exit;
	}
	bcl_dev->base_mem[3] = ioremap(TPU_BASE, SZ_8K);
	if (!bcl_dev->base_mem[3]) {
		dev_err(bcl_dev->device, "tpu_mem ioremap failed\n");
		ret = -EIO;
		goto bcl_soc_probe_exit;
	}
	bcl_dev->base_mem[4] = ioremap(G3D_BASE, SZ_8K);
	if (!bcl_dev->base_mem[4]) {
		dev_err(bcl_dev->device, "gpu_mem ioremap failed\n");
		ret = -EIO;
		goto bcl_soc_probe_exit;
	}
	bcl_dev->sysreg_cpucl0 = ioremap(SYSREG_CPUCL0_BASE, SZ_8K);
	if (!bcl_dev->sysreg_cpucl0) {
		dev_err(bcl_dev->device, "sysreg_cpucl0 ioremap failed\n");
		ret = -EIO;
		goto bcl_soc_probe_exit;
	}


	mutex_lock(&sysreg_lock);
	reg = __raw_readl(bcl_dev->sysreg_cpucl0 + CLUSTER0_GENERAL_CTRL_64);
	reg |= MPMMEN_MASK;
	__raw_writel(reg, bcl_dev->sysreg_cpucl0 + CLUSTER0_GENERAL_CTRL_64);
	reg = __raw_readl(bcl_dev->sysreg_cpucl0 + CLUSTER0_PPM);
	reg |= PPMEN_MASK;
	__raw_writel(reg, bcl_dev->sysreg_cpucl0 + CLUSTER0_PPM);

	mutex_unlock(&sysreg_lock);
	gs101_set_ppm_throttling(bcl_dev, SYS_THROTTLING_DISABLED);
	gs101_set_mpmm_throttling(bcl_dev, SYS_THROTTLING_DISABLED);
	mutex_init(&bcl_dev->state_trans_lock);
	mutex_init(&bcl_dev->ratio_lock);
	bcl_dev->soc_ops.get_temp = gs101_bcl_read_soc;
	bcl_dev->soc_ops.set_trips = gs101_bcl_set_soc;
	for (i = 0; i < IRQ_SOURCE_S2MPG10_MAX; i++) {
		bcl_dev->s2mpg10_counter[i] = 0;
		bcl_dev->s2mpg10_triggered_irq[i] = 0;
		atomic_set(&bcl_dev->s2mpg10_cnt[i], 0);
		mutex_init(&bcl_dev->s2mpg10_irq_lock[i]);
	}
	for (i = 0; i < IRQ_SOURCE_S2MPG11_MAX; i++) {
		bcl_dev->s2mpg11_counter[i] = 0;
		bcl_dev->s2mpg11_triggered_irq[i] = 0;
		atomic_set(&bcl_dev->s2mpg11_cnt[i], 0);
		mutex_init(&bcl_dev->s2mpg11_irq_lock[i]);
	}
	thermal_zone_device_update(bcl_dev->soc_tzd, THERMAL_DEVICE_UP);
	schedule_delayed_work(&bcl_dev->soc_eval_work, 0);
	bcl_dev->batt_psy = google_gs101_get_power_supply(bcl_dev);
	google_gs101_set_main_pmic(bcl_dev);
	google_gs101_set_sub_pmic(bcl_dev);
	bcl_dev->device = pmic_device_create(bcl_dev, "mitigation");
	ret = device_create_file(bcl_dev->device, &dev_attr_triggered_stats);
	if (ret) {
		dev_err(bcl_dev->device, "gs101_bcl: failed to create device file, %s\n",
			dev_attr_triggered_stats.attr.name);
	}
	ret = device_create_file(bcl_dev->device, &dev_attr_mpmm_settings);
	if (ret) {
		dev_err(bcl_dev->device, "gs101_bcl: failed to create device file, %s\n",
			dev_attr_mpmm_settings.attr.name);
	}
	ret = device_create_file(bcl_dev->device, &dev_attr_ppm_settings);
	if (ret) {
		dev_err(bcl_dev->device, "gs101_bcl: failed to create device file, %s\n",
			dev_attr_ppm_settings.attr.name);
	}
	ret = device_create_file(bcl_dev->device, &dev_attr_clk_ratio);
	if (ret) {
		dev_err(bcl_dev->device, "gs101_bcl: failed to create device file, %s\n",
			dev_attr_clk_ratio.attr.name);
	}
	ret = device_create_file(bcl_dev->device, &dev_attr_clk_stats);
	if (ret) {
		dev_err(bcl_dev->device, "gs101_bcl: failed to create device file, %s\n",
			dev_attr_clk_stats.attr.name);
	}

	return 0;

bcl_soc_probe_exit:
	gs101_bcl_soc_remove(bcl_dev);
	return ret;
}

static int google_gs101_bcl_remove(struct platform_device *pdev)
{
	struct gs101_bcl_dev *gs101_bcl_device = platform_get_drvdata(pdev);

	pmic_device_destroy(gs101_bcl_device->device->devt);
	gs101_bcl_soc_remove(gs101_bcl_device);
	debugfs_remove(gs101_bcl_device->debug_entry);

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "google,gs101-bcl"},
	{},
};

static struct platform_driver gs101_bcl_driver = {
	.probe  = google_gs101_bcl_probe,
	.remove = google_gs101_bcl_remove,
	.id_table = google_gs101_id_table,
	.driver = {
		.name           = "google_mitigation",
		.owner          = THIS_MODULE,
		.of_match_table = match_table,
	},
};

module_platform_driver(gs101_bcl_driver);
MODULE_DESCRIPTION("Google Battery Current Limiter");
MODULE_AUTHOR("George Lee <geolee@google.com>");
MODULE_LICENSE("GPL");
