#include "../pmucal_common.h"
#include "../pmucal_cpu.h"
#include "../pmucal_local.h"
#include "../pmucal_rae.h"
#include "../pmucal_system.h"
#include "../pmucal_powermode.h"

#include "flexpmu_cal_cpu_gs101.h"
#include "flexpmu_cal_local_gs101.h"
#include "flexpmu_cal_p2vmap_gs101.h"
#include "flexpmu_cal_system_gs101.h"
#include "flexpmu_cal_powermode_gs101.h"
#include "flexpmu_cal_define_gs101.h"

#include "cmucal-node.c"
#include "cmucal-qch.c"
#include "cmucal-sfr.c"
#include "cmucal-vclk.c"
#include "cmucal-vclklut.c"

#include "clkout_gs101.c"

#include "acpm_dvfs_gs101.h"

#include "asv_gs101.h"

#include "../ra.h"

#include <soc/google/cmu_ewf.h>

extern unsigned int fin_hz_var;
void __iomem *gpio_alive;

#define GPIO_ALIVE_BASE		(0x174d0000)
#define GPA1_DAT		(0x24)

void gs101_cal_data_init(void)
{
	pr_info("%s: cal data init\n", __func__);

	/* cpu inform sfr initialize */
	pmucal_sys_powermode[SYS_SICD] = CPU_INFORM_SICD;
	pmucal_sys_powermode[SYS_SLEEP] = CPU_INFORM_SLEEP;
	pmucal_sys_powermode[SYS_SLEEP_SLCMON] = CPU_INFORM_SLEEP_SLCMON;

	cpu_inform_c2 = CPU_INFORM_C2;
	cpu_inform_cpd = CPU_INFORM_CPD;

	gpio_alive = ioremap(GPIO_ALIVE_BASE, SZ_4K);
	if (!gpio_alive) {
		pr_err("%s: gpio_alive ioremap failed\n", __func__);
		BUG();
	}

	/* check DEBUG_SEL and determine FIN src */
	if (__raw_readl(gpio_alive + GPA1_DAT) & (1 << 6))
		fin_hz_var = FIN_HZ_26M;
	else
		fin_hz_var = 24576000;
}

void (*cal_data_init)(void) = gs101_cal_data_init;
int (*wa_set_cmuewf)(unsigned int index, unsigned int en, void *cmu_cmu, int *ewf_refcnt) = NULL;
void (*cal_set_cmu_smpl_warn)(void) = NULL;
