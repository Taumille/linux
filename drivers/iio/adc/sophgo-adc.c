// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Sophgo SARADC Driver
 *
 *  (C) 2012 by Bootlin
 *  Author: Thomas Bonnefille <thomas.bonnefille@bootlin.com>
 *  All rights reserved.
 */

#include "linux/irqreturn.h"
#include "linux/printk.h"
#include "linux/wait.h"
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/iio/iio.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>

#define SOPHGO_SARADC_CTRL 0x04

#define REG_SARADC_EN 0
#define REG_SARADC_SEL(x) (x+4)

#define SOPHGO_SARADC_STATUS 0x08

#define SOPHGO_SARADC_CYC_SET 0x0C
#define SOPHGO_SARADC_CH_RESULT(x) 0x10+4*x

#define SARADC_CH_RESULT(x) x&0xfff
#define SARADC_CH_VALID(x) x&&BIT(15)

#define SOPHGO_SARADC_INTR_EN 0x20
#define SOPHGO_SARADC_INTR_CLR 0x24
#define SOPHGO_SARADC_INTR_STA 0x28
#define SOPHGO_SARADC_INTR_RAW 0x2C

#define SOPHGO_SARADC_CHANNEL(index)					\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = index,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.scan_index = index,				\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = 12,					\
		},							\
	}

#define reg_read(saradc, reg) readl(saradc->regs + reg)
#define reg_write(saradc, val, reg) writel(val, saradc->regs + reg)

static const struct iio_chan_spec sophgo_channels[] = {
	SOPHGO_SARADC_CHANNEL(1),
	SOPHGO_SARADC_CHANNEL(2),
	SOPHGO_SARADC_CHANNEL(3),
};

struct sophgo_saradc {
	void __iomem *regs;
	wait_queue_head_t wait;
	bool measure_taken;
	struct iio_info info;
	struct iio_trigger *trig;
};

static void set_bit_reg(struct sophgo_saradc *saradc, u8 bit, unsigned int reg){
	u32 buff;
	buff = reg_read(saradc, reg);
	buff |= 1<<bit;
	reg_write(saradc, buff, reg);
}

static int sophgo_saradc_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask){
	u32 meas_result;
	struct sophgo_saradc *saradc = iio_priv(indio_dev);
	reg_write(saradc, 0, SOPHGO_SARADC_CTRL);
	set_bit_reg(saradc, REG_SARADC_SEL(chan->scan_index), SOPHGO_SARADC_CTRL);
	set_bit_reg(saradc, REG_SARADC_EN, SOPHGO_SARADC_CTRL);
	wait_event_interruptible(saradc->wait, saradc->measure_taken);
	saradc->measure_taken = 0;
	meas_result = reg_read(saradc, SOPHGO_SARADC_CH_RESULT(chan->scan_index));

	switch (mask) {
		case IIO_CHAN_INFO_RAW:
			if(SARADC_CH_VALID(meas_result)){
				*val = SARADC_CH_RESULT(meas_result);
				return IIO_VAL_INT;
			}
			return -ENODATA;
			break;
		case IIO_CHAN_INFO_SCALE:
			*val = 3300;
			*val2 = 12;
			return IIO_VAL_FRACTIONAL_LOG2;
			break;
		default:
			return -EINVAL;
			break;
	}
}

static irqreturn_t sophgo_saradc_interrupt_handler(int irq, void *dev_id){
	struct sophgo_saradc *saradc = dev_id;
	reg_write(saradc, 1, SOPHGO_SARADC_INTR_CLR);
	saradc->measure_taken = 1;
	wake_up(&saradc->wait);
	return IRQ_HANDLED;
}

static int sophgo_saradc_probe(struct platform_device *pdev){
	int ret,irq;
	struct iio_dev *indio_dev;
	struct sophgo_saradc *saradc;
	struct clk *clk;


	clk = clk_get(&pdev->dev, "saradc_clk");
	if (IS_ERR(clk))
		return -EINVAL;
	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*saradc));
	saradc = iio_priv(indio_dev);
	indio_dev->name = "sgsaradc";
	indio_dev->info = &saradc->info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->num_channels = 3;
	indio_dev->channels = sophgo_channels;

	saradc->regs = devm_platform_ioremap_resource(pdev, 0);
	saradc->measure_taken = 0;
	init_waitqueue_head(&saradc->wait);
	saradc->info.read_raw = &sophgo_saradc_read_raw;

	irq = platform_get_irq(pdev, 0);
	if (irq<0)
		return irq;

	pr_info("IRQ nb : %d",irq);
	ret = devm_request_irq(&pdev->dev, irq, sophgo_saradc_interrupt_handler, IRQF_SHARED, "SGSARADC", saradc);
	if (ret){
		return ret;
	}

	reg_write(saradc, 1, SOPHGO_SARADC_INTR_EN);
	pr_info("Registering iio dev");
	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id sophgo_saradc_match[] = {
	{ .compatible = "sophgo,sophgo-saradc", },
	{ },
};
MODULE_DEVICE_TABLE(of, sophgo_saradc_match);

static struct platform_driver sophgo_saradc_driver = {
	.driver	= {
		.name		= "sophgo-saradc",
		.of_match_table	= sophgo_saradc_match,
	},
	.probe	= sophgo_saradc_probe,
};
module_platform_driver(sophgo_saradc_driver);

MODULE_AUTHOR("Thomas Bonnefille <thomas.bonnefille@bootlin.com>");
MODULE_DESCRIPTION("Sophgo SARADC driver");
MODULE_LICENSE("GPL");
