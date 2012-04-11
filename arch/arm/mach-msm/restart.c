/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/mfd/pmic8058.h>
#include <linux/mfd/pmic8901.h>

#include <mach/msm_iomap.h>
#include <mach/restart.h>
#include <mach/scm-io.h>
#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/scm.h>
#ifdef CONFIG_SEC_DEBUG
#include <mach/sec_debug.h> 
#endif

#define TCSR_WDT_CFG 0x30

#define WDT0_RST       (MSM_TMR0_BASE + 0x38)
#define WDT0_EN        (MSM_TMR0_BASE + 0x40)
#define WDT0_BARK_TIME (MSM_TMR0_BASE + 0x4C)
#define WDT0_BITE_TIME (MSM_TMR0_BASE + 0x5C)

#define PSHOLD_CTL_SU (MSM_TLMM_BASE + 0x820)

#define IMEM_BASE           0x2A05F000

#define RESTART_REASON_ADDR 0x2A05F65C

#define DLOAD_MODE_ADDR     0x0

#define SCM_IO_DISABLE_PMIC_ARBITER	1

static int restart_mode;

#ifdef CONFIG_MSM_DLOAD_MODE
static int in_panic;
static int reset_detection;
static void *dload_mode_addr;

#ifndef CONFIG_SEC_DEBUG


// P5 KOR : sh1.back 11. 11. 21  - add commercial dump fuctionality
#if defined(CONFIG_TARGET_LOCALE_KOR_SKT) || defined(CONFIG_TARGET_LOCALE_KOR_KT) || defined(CONFIG_TARGET_LOCALE_KOR_LGU)	
#ifdef CONFIG_SEC_DEBUG
#define RESTART_SECDEBUG_MODE   	0x776655EE
#define RESTART_SECCOMMDEBUG_MODE   	0x7766DEAD
#endif
#endif

/* Download mode master kill-switch */
static int dload_set(const char *val, struct kernel_param *kp);
static int download_mode = 1;
module_param_call(download_mode, dload_set, param_get_int,
			&download_mode, 0644);
#endif

static int panic_prep_restart(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	if(!sec_debug_is_enabled()) 
	{		
		printk(KERN_NOTICE "panic_prep_restart\n");	
		return NOTIFY_DONE;	
	}	

	printk(KERN_NOTICE "panic_prep_restart, in_panic = 1\n");

	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call	= panic_prep_restart,
};

void set_dload_mode(int on)
{
	if (dload_mode_addr) {
		writel(on ? 0xE47B337D : 0, dload_mode_addr);
		writel(on ? 0xCE14091A : 0,
		       dload_mode_addr + sizeof(unsigned int));
		mb();
	}
}

#ifndef CONFIG_SEC_DEBUG
static int dload_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = download_mode;

	ret = param_set_int(val, kp);

	if (ret)
		return ret;

	/* If download_mode is not zero or one, ignore. */
	if (download_mode >> 1) {
		download_mode = old_val;
		return -EINVAL;
	}

	set_dload_mode(download_mode);

	return 0;
}
#endif
#else
#define set_dload_mode(x) do {} while (0)
#endif

void msm_set_restart_mode(int mode)
{
	restart_mode = mode;
}
EXPORT_SYMBOL(msm_set_restart_mode);

static void __msm_power_off(int lower_pshold)
{
	printk(KERN_CRIT "Powering off the SoC - 1\n");
#ifdef CONFIG_MSM_DLOAD_MODE
	set_dload_mode(0);
#endif
	pm8058_reset_pwr_off(0);
	printk(KERN_CRIT "Powering off the SoC - 2\n");
	pm8901_reset_pwr_off(0);
	printk(KERN_CRIT "Powering off the SoC - 3\n");
	mdelay(8);
	printk(KERN_CRIT "Powering off the SoC - 4\n");

	if (lower_pshold) {
	writel(0, PSHOLD_CTL_SU);
	mdelay(10000);
	printk(KERN_ERR "Powering off has failed\n");
	}
	printk(KERN_CRIT "Powering off the SoC - 5\n");
	
	return;
}

static void msm_power_off(void)
{
	/* MSM initiated power off, lower ps_hold */
	__msm_power_off(1);
}

static void cpu_power_off(void *data)
{
	int rc;
	
	pr_err("PMIC Initiated shutdown %s cpu=%d\n", __func__,
						smp_processor_id());
	if (smp_processor_id() == 0) {
		/*
		 * PMIC initiated power off, do not lower ps_hold, pmic will
		 * shut msm down
		 */
		__msm_power_off(0);
		/* call secure manager to disable arbiter and never return */
		rc = scm_call_atomic1(SCM_SVC_PWR,
						SCM_IO_DISABLE_PMIC_ARBITER, 1);
		pr_err("SCM returned even when asked to busy loop rc = %d"
				"waiting on pmic to shut msm down", rc);
	}

	preempt_disable();
	while (1)
		;
}

static irqreturn_t resout_irq_handler(int irq, void *dev_id)
{
	pr_warn("%s PMIC Initiated shutdown\n", __func__);
	oops_in_progress = 1;
	smp_call_function_many(cpu_online_mask, cpu_power_off, NULL, 0);
	if (smp_processor_id() == 0)
		cpu_power_off(NULL);
	preempt_disable();
	while (1)
		;
	return IRQ_HANDLED;
}

extern void* restart_reason;

int pmic_reset_irq;

void arch_reset(char mode, const char *cmd)
{
#ifndef CONFIG_SEC_DEBUG
#ifdef CONFIG_MSM_DLOAD_MODE
	/* This looks like a normal reboot at this point. */
	set_dload_mode(0);

	/* Write download mode flags if we're panic'ing */
	set_dload_mode(in_panic);

	/* Write download mode flags if restart_mode says so */
	if (restart_mode == RESTART_DLOAD)
		set_dload_mode(1);

	/* Kill download mode if master-kill switch is set */
	if (!download_mode)
		set_dload_mode(0);
#endif
#endif

	printk(KERN_NOTICE "Going down for restart now\n");

	pm8058_reset_pwr_off(1);

	if ( !restart_reason) {
		restart_reason = ioremap_nocache(RESTART_REASON_ADDR, SZ_4K);
	}

	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)) {
			writel(0x77665500, restart_reason);
		} else if (!strncmp(cmd, "recovery", 8)) {
			writel(0x77665502, restart_reason);
		} else if (!strncmp(cmd, "download", 8)) {
			writel(0x776655FF, restart_reason);
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned long code;
			strict_strtoul(cmd + 4, 16, &code);
			code = code & 0xff;
			writel(0x6f656d00 | code, restart_reason);
#ifdef CONFIG_SEC_DEBUG
		} else if (!strncmp(cmd, "sec_debug_hw_reset", 18)) {
			writel(0x776655ee, restart_reason);
// P5 KOR : sh1.back 11. 11. 21  - add commercial dump fuctionality
#if defined(CONFIG_TARGET_LOCALE_KOR_SKT) || defined(CONFIG_TARGET_LOCALE_KOR_KT) || defined(CONFIG_TARGET_LOCALE_KOR_LGU)			
		} else if (!strncmp(cmd, "sec_debug_comm_dump", 19)) {
			writel(0x7766DEAD, restart_reason);
#endif
#endif
		} else if (!strncmp(cmd, "arm11_fota", 10)) {
			writel(0x77665503, restart_reason);
		} else if(!strncmp(cmd, "NO SIM", 6)) {
			writel(0x12345678, restart_reason);
		}
		else {
			writel(0x12345678, restart_reason);
		}
	}
#ifdef CONFIG_SEC_DEBUG
	else {
		printk(KERN_NOTICE "%s : clear reset flag\r\n", __func__);
		writel(0x12345678, restart_reason);    /* clear abnormal reset flag */
	}
#endif

	writel(0, WDT0_EN);
	if (!(machine_is_msm8x60_charm_surf() ||
	      machine_is_msm8x60_charm_ffa() ||
	      machine_is_p5_lte() ||
	      machine_is_p4_lte() ||
	      machine_is_p8_lte())) {
		dsb();
		writel(0, PSHOLD_CTL_SU); /* Actually reset the chip */
		mdelay(5000);
		pr_notice("PS_HOLD didn't work, falling back to watchdog\n");
	}

/* 	MDM dump corruption
	Give time to MDM to prepare Upload mode*/
//------------------
	__raw_writel(0, WDT0_EN);
	mdelay(1000);
//------------------

	__raw_writel(1, WDT0_RST);
	__raw_writel(5*0x31F3, WDT0_BARK_TIME);
	__raw_writel(0x31F3, WDT0_BITE_TIME);
	__raw_writel(3, WDT0_EN);
	secure_writel(3, MSM_TCSR_BASE + TCSR_WDT_CFG);

	mdelay(10000);
	printk(KERN_ERR "Restarting has failed\n");
}

#ifdef CONFIG_SEC_DEBUG 
static int dload_mode_normal_reboot_handler(struct notifier_block *nb,
				unsigned long l, void *p)
{
	set_dload_mode(0);

	return 0;
}

static struct notifier_block dload_reboot_block = {
	.notifier_call = dload_mode_normal_reboot_handler
};
#endif

static int __init msm_restart_init(void)
{
	int rc;	

#ifdef CONFIG_MSM_DLOAD_MODE
	dload_mode_addr = ioremap_nocache(0x2A05F000, SZ_4K);

	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);

#ifdef CONFIG_SEC_DEBUG 
	register_reboot_notifier(&dload_reboot_block);
#endif

	/* Reset detection is switched on below.*/
	set_dload_mode(1);
#endif

	pm_power_off = msm_power_off;

	if (pmic_reset_irq != 0) {
		rc = request_any_context_irq(pmic_reset_irq,
					resout_irq_handler, IRQF_TRIGGER_HIGH,
					"restart_from_pmic", NULL);
		if (rc < 0)
			pr_err("pmic restart irq fail rc = %d\n", rc);
	} else {
		pr_warn("no pmic restart interrupt specified\n");
	}

	
	return 0;
}

late_initcall(msm_restart_init);
