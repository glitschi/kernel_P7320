/* Copyright (c) 2009-2011, Code Aurora Forum. All rights reserved.
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
/*
 * SSBI driver for Qualcomm MSM platforms
 *
 */
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/remote_spinlock.h>
#include <mach/board.h>
#include <linux/slab.h>
#include <mach/msm_xo.h>

/* SSBI 2.0 controller registers */
#define SSBI2_CMD			0x0008
#define SSBI2_RD			0x0010
#define SSBI2_STATUS			0x0014
#define SSBI2_MODE2			0x001C

/* SSBI_CMD fields */
#define SSBI_CMD_RDWRN			(0x01 << 24)
#define SSBI_CMD_REG_ADDR_SHFT		(0x10)
#define SSBI_CMD_REG_ADDR_MASK		(0xFF << SSBI_CMD_REG_ADDR_SHFT)
#define SSBI_CMD_REG_DATA_SHFT		(0x00)
#define SSBI_CMD_REG_DATA_MASK		(0xFF << SSBI_CMD_REG_DATA_SHFT)

/* SSBI_STATUS fields */
#define SSBI_STATUS_DATA_IN		0x10
#define SSBI_STATUS_RD_CLOBBERED	0x08
#define SSBI_STATUS_RD_READY		0x04
#define SSBI_STATUS_READY		0x02
#define SSBI_STATUS_MCHN_BUSY		0x01

/* SSBI_RD fields */
#define SSBI_RD_RDWRN			0x01000000
#define SSBI_RD_REG_ADDR_SHFT		0x10
#define SSBI_RD_REG_ADDR_MASK		(0xFF << SSBI_RD_REG_ADDR_SHFT)
#define SSBI_RD_REG_DATA_SHFT		(0x00)
#define SSBI_RD_REG_DATA_MASK		(0xFF << SSBI_RD_REG_DATA_SHFT)

/* SSBI_MODE2 fields */
#define SSBI_MODE2_REG_ADDR_15_8_SHFT	0x04
#define SSBI_MODE2_REG_ADDR_15_8_MASK	(0x7F << SSBI_MODE2_REG_ADDR_15_8_SHFT)
#define SSBI_MODE2_ADDR_WIDTH_SHFT	0x01
#define SSBI_MODE2_ADDR_WIDTH_MASK	(0x07 << SSBI_MODE2_ADDR_WIDTH_SHFT)
#define SSBI_MODE2_SSBI2_MODE		0x00000001

#define SSBI_MODE2_REG_ADDR_15_8(MD, AD) \
	(((MD) & 0x0F) | ((((AD) >> 8) << SSBI_MODE2_REG_ADDR_15_8_SHFT) & \
	SSBI_MODE2_REG_ADDR_15_8_MASK))

#define SSBI_MODE2_ADDR_WIDTH(N) \
	((((N) - 8) << SSBI_MODE2_ADDR_WIDTH_SHFT) & SSBI_MODE2_ADDR_WIDTH_MASK)

#define SSBI_TIMEOUT_US			1000

#define SSBI_CMD_READ(AD) \
	(SSBI_CMD_RDWRN | (((AD) & 0xFF) << SSBI_CMD_REG_ADDR_SHFT))

#define SSBI_CMD_WRITE(AD, DT) \
	((((AD) & 0xFF) << SSBI_CMD_REG_ADDR_SHFT) | \
	 (((DT) & 0xFF) << SSBI_CMD_REG_DATA_SHFT))

/* SSBI PMIC Arbiter command registers */
#define SSBI_PA_CMD			0x0000
#define SSBI_PA_RD_STATUS		0x0004

/* SSBI_PA_CMD fields */
#define SSBI_PA_CMD_RDWRN		(0x01 << 24)
#define SSBI_PA_CMD_REG_ADDR_14_8_SHFT	(0x10)
#define SSBI_PA_CMD_REG_ADDR_14_8_MASK	(0x7F << SSBI_PA_CMD_REG_ADDR_14_8_SHFT)
#define SSBI_PA_CMD_REG_ADDR_7_0_SHFT	(0x08)
#define SSBI_PA_CMD_REG_ADDR_7_0_MASK	(0xFF << SSBI_PA_CMD_REG_ADDR_7_0_SHFT)
#define SSBI_PA_CMD_REG_DATA_SHFT	(0x00)
#define SSBI_PA_CMD_REG_DATA_MASK	(0xFF << SSBI_PA_CMD_REG_DATA_SHFT)

#define SSBI_PA_CMD_REG_DATA(DT) \
	(((DT) << SSBI_PA_CMD_REG_DATA_SHFT) & SSBI_PA_CMD_REG_DATA_MASK)

#define SSBI_PA_CMD_REG_ADDR(AD) \
	(((AD) << SSBI_PA_CMD_REG_ADDR_7_0_SHFT) & \
	(SSBI_PA_CMD_REG_ADDR_14_8_MASK|SSBI_PA_CMD_REG_ADDR_7_0_MASK))

/* SSBI_PA_RD_STATUS fields */
#define SSBI_PA_RD_STATUS_TRANS_DONE	(0x01 << 27)
#define SSBI_PA_RD_STATUS_TRANS_DENIED	(0x01 << 26)
#define SSBI_PA_RD_STATUS_REG_DATA_SHFT	(0x00)
#define SSBI_PA_RD_STATUS_REG_DATA_MASK	(0xFF << SSBI_PA_CMD_REG_DATA_SHFT)
#define SSBI_PA_RD_STATUS_TRANS_COMPLETE \
	(SSBI_PA_RD_STATUS_TRANS_DONE|SSBI_PA_RD_STATUS_TRANS_DENIED)

#define SSBI_MSM_NAME			"i2c_ssbi"

MODULE_LICENSE("GPL v2");
MODULE_VERSION("2.0");
MODULE_ALIAS("platform:i2c_ssbi");

struct i2c_ssbi_dev {
	void __iomem		*base;
	struct device           *dev;
	struct i2c_adapter	 adapter;
	unsigned long		 mem_phys_addr;
	size_t			 mem_size;
	bool			 use_rlock;
	remote_spinlock_t	 rspin_lock;
	enum msm_ssbi_controller_type controller_type;
	int (*read)(struct i2c_ssbi_dev *, struct i2c_msg *);
	int (*write)(struct i2c_ssbi_dev *, struct i2c_msg *);
};

#define SSBI_TIMEOUT_RETRY	5

struct msm_xo_voter *ssbi_reset_voter;

/* Send a request to the RPM to reset the SSBI modules in the MSM. */
static void rpm_reset_ssbi(struct i2c_ssbi_dev *ssbi)
{
	dev_err(ssbi->dev, "%s: sending command to RPM to reset SSBI\n",
		__func__);
	msm_xo_mode_vote(ssbi_reset_voter, MSM_XO_MODE_ON);
	msm_xo_mode_vote(ssbi_reset_voter, MSM_XO_MODE_OFF);
	dev_err(ssbi->dev, "%s: SSBI reset complete\n",
	__func__);
}

static inline int
i2c_ssbi_poll_for_device_ready(struct i2c_ssbi_dev *ssbi)
{
	u32 timeout = SSBI_TIMEOUT_US;

	while (!(readl(ssbi->base + SSBI2_STATUS) & SSBI_STATUS_READY)) {
		if (--timeout == 0) {
			dev_err(ssbi->dev, "%s: timeout, status %x\n", __func__,
				readl(ssbi->base + SSBI2_STATUS));
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	return 0;
}

static inline int
i2c_ssbi_poll_for_read_completed(struct i2c_ssbi_dev *ssbi)
{
	u32 timeout = SSBI_TIMEOUT_US;

	while (!(readl(ssbi->base + SSBI2_STATUS) & SSBI_STATUS_RD_READY)) {
		if (--timeout == 0) {
			dev_err(ssbi->dev, "%s: timeout, status %x\n", __func__,
				readl(ssbi->base + SSBI2_STATUS));
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	return 0;
}

static inline int
i2c_ssbi_poll_for_transfer_completed(struct i2c_ssbi_dev *ssbi)
{
	u32 timeout = SSBI_TIMEOUT_US;

	while ((readl(ssbi->base + SSBI2_STATUS) & SSBI_STATUS_MCHN_BUSY)) {
		if (--timeout == 0) {
			dev_err(ssbi->dev, "%s: timeout, status %x\n", __func__,
				readl(ssbi->base + SSBI2_STATUS));
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	return 0;
}

static int
i2c_ssbi_read_bytes(struct i2c_ssbi_dev *ssbi, struct i2c_msg *msg)
{
	int ret = 0;
	u8 *buf = msg->buf;
	u16 len = msg->len;
	u16 addr = msg->addr;
	u32 read_cmd = SSBI_CMD_READ(addr);

	if (ssbi->controller_type == MSM_SBI_CTRL_SSBI2) {
		u32 mode2 = readl(ssbi->base + SSBI2_MODE2);
		writel(SSBI_MODE2_REG_ADDR_15_8(mode2, addr),
				ssbi->base + SSBI2_MODE2);
	}

	while (len) {
		ret = i2c_ssbi_poll_for_device_ready(ssbi);
		if (ret)
			goto read_failed;

		writel(read_cmd, ssbi->base + SSBI2_CMD);

		ret = i2c_ssbi_poll_for_read_completed(ssbi);
		if (ret)
			goto read_failed;

		*buf++ = readl(ssbi->base + SSBI2_RD) & SSBI_RD_REG_DATA_MASK;
		len--;
	}

read_failed:
	return ret;
}

static int
i2c_ssbi_write_bytes(struct i2c_ssbi_dev *ssbi, struct i2c_msg *msg)
{
	int ret = 0;
	u8 *buf = msg->buf;
	u16 len = msg->len;
	u16 addr = msg->addr;

	if (ssbi->controller_type == MSM_SBI_CTRL_SSBI2) {
		u32 mode2 = readl(ssbi->base + SSBI2_MODE2);
		writel(SSBI_MODE2_REG_ADDR_15_8(mode2, addr),
				ssbi->base + SSBI2_MODE2);
	}

	while (len) {
		ret = i2c_ssbi_poll_for_device_ready(ssbi);
		if (ret)
			goto write_failed;

		writel(SSBI_CMD_WRITE(addr, *buf++), ssbi->base + SSBI2_CMD);

		ret = i2c_ssbi_poll_for_transfer_completed(ssbi);
		if (ret)
			goto write_failed;

		len--;
	}

write_failed:
	return ret;
}


void __iomem *PA1_SSBI2_CFG_BASE = NULL;

static void DumpRegisterState(struct i2c_ssbi_dev *ssbi)
{
	int i=0;
	
	if (PA1_SSBI2_CFG_BASE == NULL) {
		PA1_SSBI2_CFG_BASE = ioremap_nocache(0x00300000, 0xA7);
	}
	
	if (PA1_SSBI2_CFG_BASE == NULL) {
		dev_err(ssbi->dev,"SUJEEV : ioremap failed?\n");
		return;
	}
	
	#if 0
	printk(KERN_ERR, "CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE));
	printk(KERN_ERR, "INT_ENABLE [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x04));
	printk(KERN_ERR, "PRIORITIES [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x08));
	printk(KERN_ERR, "BANKS_2_3_CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x0C));
	printk(KERN_ERR, "ILLEGAL_STATUS [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x10));
	printk(KERN_ERR, "TEST_BUS_CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x14));
	#else
	dev_err(ssbi->dev, "CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE));
	dev_err(ssbi->dev, "INT_ENABLE [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x04));
	dev_err(ssbi->dev, "PRIORITIES [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x08));
	dev_err(ssbi->dev, "BANKS_2_3_CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x0C));
	dev_err(ssbi->dev, "ILLEGAL_STATUS [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x10));
	dev_err(ssbi->dev, "TEST_BUS_CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0x14));
	#endif
	
	for (i=0;i<=6;i++) {
		dev_err(ssbi->dev, "SSBI2_VIT%d [%x] \n", i, readl(PA1_SSBI2_CFG_BASE + (0x20 + (0x4*i))));
	}
	dev_err(ssbi->dev, "PVC_CTL [%x]\n", readl(PA1_SSBI2_CFG_BASE + 0xA0)); 
}

static inline int
i2c_ssbi_pa_transfer(struct i2c_ssbi_dev *ssbi, u32 cmd, u8 *data)
{
	u32 rd_status;
	int timeout_retry = SSBI_TIMEOUT_RETRY;
	u32 timeout;
	u32 PrevStatus;

begin_transaction:
	timeout = SSBI_TIMEOUT_US;
	
	PrevStatus=readl(ssbi->base+SSBI_PA_RD_STATUS);

	writel(cmd, ssbi->base + SSBI_PA_CMD);
	rd_status = readl(ssbi->base + SSBI_PA_RD_STATUS);

	while ((rd_status & (SSBI_PA_RD_STATUS_TRANS_COMPLETE)) == 0) {

		if (--timeout == 0) {
			dev_err(ssbi->dev, "%s: timeout, status %x\n",
					__func__, rd_status);
			if (--timeout_retry > 0) {
				rpm_reset_ssbi(ssbi);
				goto begin_transaction;
			}
			DumpRegisterState(ssbi);
			dev_err(ssbi->dev, "PrevStatus [%x] \n",PrevStatus);
			return -ETIMEDOUT;
		}
		udelay(1);
		rd_status = readl(ssbi->base + SSBI_PA_RD_STATUS);
	}

	if (rd_status & SSBI_PA_RD_STATUS_TRANS_DENIED) {
		dev_err(ssbi->dev, "%s: transaction denied, status %x\n",
				__func__, rd_status);
		DumpRegisterState(ssbi);
		dev_err(ssbi->dev, "PrevStatus [%x] \n",PrevStatus);
		return -EPERM;
	}

	if (data)
		*data = (rd_status & SSBI_PA_RD_STATUS_REG_DATA_MASK) >>
			SSBI_PA_CMD_REG_DATA_SHFT;
	return 0;
}

static int
i2c_ssbi_pa_read_bytes(struct i2c_ssbi_dev *ssbi, struct i2c_msg *msg)
{
	int ret = 0;
	u8  data;
	u8 *buf = msg->buf;
	u16 len = msg->len;
	u32 read_cmd = (SSBI_PA_CMD_RDWRN | SSBI_PA_CMD_REG_ADDR(msg->addr));

	while (len) {

		ret = i2c_ssbi_pa_transfer(ssbi, read_cmd, &data);
		if (ret)
			goto read_failed;

		*buf++ = data;
		len--;
	}

read_failed:
	return ret;
}

static int
i2c_ssbi_pa_write_bytes(struct i2c_ssbi_dev *ssbi, struct i2c_msg *msg)
{
	int ret = 0;
	u8 *buf = msg->buf;
	u16 len = msg->len;
	u32 addr = SSBI_PA_CMD_REG_ADDR(msg->addr);

	while (len) {

		u32 write_cmd = addr | (*buf++ & SSBI_PA_CMD_REG_DATA_MASK);

		ret = i2c_ssbi_pa_transfer(ssbi, write_cmd, NULL);
		if (ret)
			goto write_failed;
		len--;
	}

write_failed:
	return ret;
}

static int
i2c_ssbi_transfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	int ret = 0;
	int rem = num;
	unsigned long flags = 0;
	struct i2c_ssbi_dev *ssbi = i2c_get_adapdata(adap);

	if (ssbi->use_rlock)
		remote_spin_lock_irqsave(&ssbi->rspin_lock, flags);

	while (rem) {
		if (msgs->flags & I2C_M_RD) {
			ret = ssbi->read(ssbi, msgs);
			if (ret)
				goto transfer_failed;
		} else {
			ret = ssbi->write(ssbi, msgs);
			if (ret)
				goto transfer_failed;
		}

		msgs++;
		rem--;
	}

	if (ssbi->use_rlock)
		remote_spin_unlock_irqrestore(&ssbi->rspin_lock, flags);

	return num;

transfer_failed:
	if (ssbi->use_rlock)
		remote_spin_unlock_irqrestore(&ssbi->rspin_lock, flags);
	return ret;
}

static u32 i2c_ssbi_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm msm_i2c_algo = {
	.master_xfer	= i2c_ssbi_transfer,
	.functionality	= i2c_ssbi_i2c_func,
};

static int __init i2c_ssbi_probe(struct platform_device *pdev)
{
	int			 ret = 0;
	struct resource		*ssbi_res;
	struct i2c_ssbi_dev	*ssbi;
	const struct msm_i2c_ssbi_platform_data *pdata;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		ret = -ENXIO;
		dev_err(&pdev->dev, "platform data not initialized\n");
		goto err_probe_exit;
	}

	ssbi = kzalloc(sizeof(struct i2c_ssbi_dev), GFP_KERNEL);
	if (!ssbi) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "allocation failed\n");
		goto err_probe_exit;
	}

	ssbi_res = platform_get_resource_byname(pdev,
						IORESOURCE_MEM, "ssbi_base");
	if (!ssbi_res) {
		ret = -ENXIO;
		dev_err(&pdev->dev, "get_resource_byname failed\n");
		goto err_probe_res;
	}

	ssbi->mem_phys_addr = ssbi_res->start;
	ssbi->mem_size = resource_size(ssbi_res);
	if (!request_mem_region(ssbi->mem_phys_addr, ssbi->mem_size,
				SSBI_MSM_NAME)) {
		ret = -ENXIO;
		dev_err(&pdev->dev, "request_mem_region failed\n");
		goto err_probe_reqmem;
	}

	ssbi->base = ioremap(ssbi->mem_phys_addr, ssbi->mem_size);
	if (!ssbi->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		goto err_probe_ioremap;
	}

	ssbi->dev = &pdev->dev;
	platform_set_drvdata(pdev, ssbi);

	ssbi->controller_type = pdata->controller_type;
	if (ssbi->controller_type == MSM_SBI_CTRL_PMIC_ARBITER) {
		ssbi->read = i2c_ssbi_pa_read_bytes;
		ssbi->write = i2c_ssbi_pa_write_bytes;
	} else {
		ssbi->read = i2c_ssbi_read_bytes;
		ssbi->write = i2c_ssbi_write_bytes;
	}

	i2c_set_adapdata(&ssbi->adapter, ssbi);
	ssbi->adapter.algo = &msm_i2c_algo;
	strlcpy(ssbi->adapter.name,
		"MSM SSBI adapter",
		sizeof(ssbi->adapter.name));

	if (pdata->rsl_id) {
		ret = remote_spin_lock_init(&ssbi->rspin_lock, pdata->rsl_id);
		if (ret) {
			dev_err(&pdev->dev, "remote spinlock init failed\n");
			goto err_remote_spinlock_init_failed;
		}
		ssbi->use_rlock = 1;
	}

	ssbi->adapter.nr = pdev->id;
	ret = i2c_add_numbered_adapter(&ssbi->adapter);
	if (ret) {
		dev_err(&pdev->dev, "i2c_add_numbered_adapter failed\n");
		goto err_add_adapter_failed;
	}
	return 0;

err_add_adapter_failed:
err_remote_spinlock_init_failed:
	iounmap(ssbi->base);
	platform_set_drvdata(pdev, NULL);
err_probe_ioremap:
	release_mem_region(ssbi->mem_phys_addr, ssbi->mem_size);
err_probe_reqmem:
err_probe_res:
	kfree(ssbi);
err_probe_exit:
	return ret;
}

static int __devexit i2c_ssbi_remove(struct platform_device *pdev)
{
	struct i2c_ssbi_dev *ssbi = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	i2c_del_adapter(&ssbi->adapter);
	iounmap(ssbi->base);
	release_mem_region(ssbi->mem_phys_addr, ssbi->mem_size);
	kfree(ssbi);
	return 0;
}

static struct platform_driver i2c_ssbi_driver = {
	.driver		= {
		.name	= "i2c_ssbi",
		.owner	= THIS_MODULE,
	},
	.remove		= __exit_p(i2c_ssbi_remove),
};

static int __init i2c_ssbi_init(void)
{
	ssbi_reset_voter = msm_xo_get(MSM_XO_RESET_SSBI, "i2c_ssbi");
	return platform_driver_probe(&i2c_ssbi_driver, i2c_ssbi_probe);
}
arch_initcall(i2c_ssbi_init);

static void __exit i2c_ssbi_exit(void)
{
	msm_xo_put(ssbi_reset_voter);
	platform_driver_unregister(&i2c_ssbi_driver);
}
module_exit(i2c_ssbi_exit);
