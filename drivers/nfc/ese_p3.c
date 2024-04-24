/*
 * Copyright (C) 2015 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/spi/spi.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/ioctl.h>
#include <linux/of_gpio.h>
#include <linux/platform_data/spi-s3c64xx.h>
#include <linux/pm_runtime.h>
#include <linux/spi/spidev.h>
#include <linux/clk.h>
#include <linux/wakelock.h>
#include <linux/string.h>

/* #include <linux/spi/spi-geni-qcom.h> */
#include "ese_p3.h"

#define SPI_DEFAULT_SPEED 8000000L

/* size of maximum read/write buffer supported by driver */
#define MAX_BUFFER_SIZE   259U

/* Different driver debug lever */
enum P3_DEBUG_LEVEL {
	P3_DEBUG_OFF,
	P3_FULL_DEBUG
};

/* Variable to store current debug level request by ioctl */
static unsigned char debug_level = P3_FULL_DEBUG;

#define P3_DBG_MSG(msg...) do { \
		switch (debug_level) { \
		case P3_DEBUG_OFF: \
			break; \
		case P3_FULL_DEBUG: \
			pr_info("[ESE-P3] :  " msg); \
			break; \
			 /*fallthrough*/ \
		default: \
			pr_err("[ESE-P3] : debug level %d", debug_level);\
			break; \
		}; \
	} while (0)

#define P3_ERR_MSG(msg...) pr_err("[ESE-P3] : " msg)
#define P3_INFO_MSG(msg...) pr_info("[ESE-P3] : " msg)

static DEFINE_MUTEX(device_list_lock);

/* Device specific macro and structure */
struct p3_data {
	wait_queue_head_t read_wq; /* wait queue for read interrupt */
	struct mutex buffer_mutex; /* buffer mutex */
	struct spi_device *spi;  /* spi device structure */
	struct miscdevice p3_device; /* char device as misc driver */

	unsigned int users;

	bool device_opened;
#ifdef FEATURE_ESE_WAKELOCK
	struct wake_lock ese_lock;
#endif
	unsigned long speed;
	const char *vdd_1p8;
	int ese_pvdd_en;
#ifdef CONFIG_ESE_SECURE
	struct clk *ese_spi_pclk;
	struct clk *ese_spi_sclk;
#endif
	const char *ap_vendor;
	unsigned char *buf;
};

/* TODO pinctrl need to be done soon for nonTZ*/
#if 0//  ndef CONFIG_ESE_SECURE
static void p3_pinctrl_config(struct device *dev, bool onoff)
{
	struct pinctrl *pinctrl = NULL;

	P3_INFO_MSG("%s: pinctrol - %s\n", __func__, onoff ? "on" : "off");

	if (onoff) {
		/* ON */
		pinctrl = devm_pinctrl_get_select(dev, "ese_active");
		if (IS_ERR_OR_NULL(pinctrl))
			P3_ERR_MSG("%s: Failed to configure ese pin\n", __func__);
		else
			devm_pinctrl_put(pinctrl);
	} else {
		/* OFF */
		pinctrl = devm_pinctrl_get_select(dev, "ese_suspend");
		if (IS_ERR_OR_NULL(pinctrl))
			P3_ERR_MSG("%s: Failed to configure ese pin\n", __func__);
		else
			devm_pinctrl_put(pinctrl);
	}
}
#endif

/*Qcom TZ seems it can control spi clk by itself */
#ifdef CONFIG_ESE_SECURE
/**
 * ese_spi_clk_max_rate: finds the nearest lower rate for a clk
 * @clk the clock for which to find nearest lower rate
 * @rate clock frequency in Hz
 * @return nearest lower rate or negative error value
 *
 * Public clock API extends clk_round_rate which is a ceiling function. This
 * function is a floor function implemented as a binary search using the
 * ceiling function.
 */
static long p3_spi_clk_max_rate(struct clk *clk, unsigned long rate)
{
	long lowest_available, nearest_low, step_size, cur;
	long step_direction = -1;
	long guess = rate;
	int  max_steps = 10;

	cur =  clk_round_rate(clk, rate);
	if (cur == rate)
		return rate;

	/* if we got here then: cur > rate */
	lowest_available =  clk_round_rate(clk, 0);
	if (lowest_available > rate)
		return -EINVAL;

	step_size = (rate - lowest_available) >> 1;
	nearest_low = lowest_available;

	while (max_steps-- && step_size) {
		guess += step_size * step_direction;

		cur =  clk_round_rate(clk, guess);

		if ((cur < rate) && (cur > nearest_low))
		        nearest_low = cur;

		/*
		 * if we stepped too far, then start stepping in the other
		 * direction with half the step size
		 */
		if (((cur > rate) && (step_direction > 0))
		 || ((cur < rate) && (step_direction < 0))) {
			step_direction = -step_direction;
			step_size >>= 1;
		}
	}
	return nearest_low;
}

static void p3_spi_clock_set(struct p3_data *data, unsigned long speed)
{
	long rate;
	if (!strcmp(data->ap_vendor, "qualcomm")) {
		/* finds the nearest lower rate for a clk */
		rate = p3_spi_clk_max_rate(data->ese_spi_sclk, speed);
		if (rate < 0) {
			pr_err("%s: no match found for requested clock: %lu",
				__func__, speed);
			return;
		}
		speed = rate;
		/*pr_info("%s speed:%lu \n", __func__, speed);*/
	} else if (!strcmp(data->ap_vendor, "slsi")) {
		/* There is half-multiplier */
		speed =  speed * 2;
	}

	clk_set_rate(data->ese_spi_sclk, speed);
}

static int p3_clk_control(struct p3_data *data, bool onoff)
{
	static bool old_value;

	if (old_value == onoff)
		return 0;

	if (onoff == true) {
		p3_spi_clock_set(data, SPI_DEFAULT_SPEED);
		clk_prepare_enable(data->ese_spi_pclk);
		clk_prepare_enable(data->ese_spi_sclk);
		usleep_range(5000, 5100);
		P3_DBG_MSG("%s clock:%lu\n", __func__, clk_get_rate(data->ese_spi_sclk));
	} else {
		clk_disable_unprepare(data->ese_spi_pclk);
		clk_disable_unprepare(data->ese_spi_sclk);
	}

	old_value = onoff;

	P3_INFO_MSG("clock %s\n", onoff ? "enabled" : "disabled");
	return 0;
}

static int p3_clk_setup(struct device *dev, struct p3_data *data)
{
	data->ese_spi_pclk = clk_get(dev, "pclk");
	if (IS_ERR(data->ese_spi_pclk)) {
		P3_ERR_MSG("Can't get %s\n", "pclk");
		data->ese_spi_pclk = NULL;
		goto err_pclk_get;
	}

	data->ese_spi_sclk = clk_get(dev, "sclk");
	if (IS_ERR(data->ese_spi_sclk)) {
		P3_ERR_MSG("Can't get %s\n", "sclk");
		data->ese_spi_sclk = NULL;
		goto err_sclk_get;
	}

	return 0;
err_sclk_get:
	clk_put(data->ese_spi_pclk);
err_pclk_get:
	return -EPERM;
}
#endif

static int p3_regulator_onoff(struct p3_data *data, int onoff)
{
	int rc = 0;
	struct regulator *regulator_vdd_1p8;

	if (!data->vdd_1p8) {
		pr_err("%s No vdd LDO name!\n", __func__);
		return -ENODEV;
	}

	regulator_vdd_1p8 = regulator_get(NULL, data->vdd_1p8);
	pr_err("%s %s\n", __func__, data->vdd_1p8);

	if (IS_ERR(regulator_vdd_1p8) || regulator_vdd_1p8 == NULL) {
		P3_ERR_MSG("%s - vdd_1p8 regulator_get fail\n", __func__);
		return -ENODEV;
	}

	P3_DBG_MSG("%s - onoff = %d\n", __func__, onoff);
	if (onoff == 1) {
		rc = regulator_enable(regulator_vdd_1p8);
		if (rc) {
			P3_ERR_MSG("%s - enable vdd_1p8 failed, rc=%d\n",
				__func__, rc);
			goto done;
		}
		msleep(20);
	} else {
		rc = regulator_disable(regulator_vdd_1p8);
		if (rc) {
			P3_ERR_MSG("%s - disable vdd_1p8 failed, rc=%d\n",
				__func__, rc);
			goto done;
		}
	}

	/*data->regulator_is_enable = (u8)onoff;*/

done:
	regulator_put(regulator_vdd_1p8);

	return rc;
}

#ifndef CONFIG_ESE_SECURE
static int p3_xfer(struct p3_data *p3_device, struct p3_ioctl_transfer *tr)
{
	int status = 0;
#if 0 /*For SDM845/linux4.9: need to change stack to dynamic memory */
	struct spi_message m;
	struct spi_transfer t;
	unsigned char tx_buffer[MAX_BUFFER_SIZE] = {0x0, };
	unsigned char rx_buffer[MAX_BUFFER_SIZE] = {0x0, };

	P3_DBG_MSG("%s\n", __func__);

	if (p3_device == NULL || tr == NULL)
		return -EFAULT;

	if (tr->len > DEFAULT_BUFFER_SIZE || !tr->len)
		return -EMSGSIZE;

	if (tr->tx_buffer != NULL) {
		if (copy_from_user(tx_buffer,
				tr->tx_buffer, tr->len) != 0)
			return -EFAULT;
	}

	spi_message_init(&m);
	memset(&t, 0, sizeof(t));

	t.tx_buf = tx_buffer;
	t.rx_buf = rx_buffer;
	t.len = tr->len;

	spi_message_add_tail(&t, &m);

	status = spi_sync(p3_device->spi, &m);
	if (status == 0) {
		if (tr->rx_buffer != NULL) {
			unsigned int missing = 0;

			missing = (unsigned int)copy_to_user(tr->rx_buffer,
					       rx_buffer, tr->len);

			if (missing != 0)
				tr->len = tr->len - missing;
		}
	}
	pr_debug("%s, length=%d\n", __func__, tr->len);
	return status;
#else
	P3_DBG_MSG("%s exit\n", __func__);

	return status;
#endif

} /* vfsspi_xfer */

static int p3_rw_spi_message(struct p3_data *p3_device,
				 unsigned long arg)
{
	struct p3_ioctl_transfer   *dup = NULL;
	int err = 0;

	dup = kmalloc(sizeof(struct p3_ioctl_transfer), GFP_KERNEL);
	if (dup == NULL)
		return -ENOMEM;

	if (copy_from_user(dup, (void *)arg,
			   sizeof(struct p3_ioctl_transfer)) != 0) {
		kfree(dup);
		return -EFAULT;
	}

	err = p3_xfer(p3_device, dup);
	if (err != 0) {
		kfree(dup);
		P3_ERR_MSG("%s xfer failed!\n", __func__);
		return err;
	}

	/*P3_ERR_MSG("%s len:%u\n", __func__, dup->len);*/
	if (copy_to_user((void *)arg, dup,
			 sizeof(struct p3_ioctl_transfer)) != 0)
		return -EFAULT;
	kfree(dup);
	return 0;
}

#if 0 //def CONFIG_COMPAT
static int p3_rw_spi_message_32(struct p3_data *p3_device, unsigned long arg)
{
	struct p3_ioctl_transfer dup;
	struct spip3_ioc_transfer_32 p3transfr_32;
	int err = 0;

	if (__copy_from_user(&p3transfr_32, (void __user *)arg,
				sizeof(struct spip3_ioc_transfer_32))) {
		P3_ERR_MSG("%s, failed to copy from user\n", __func__);
		return -EFAULT;
	}

	dup.tx_buffer = (unsigned char *)(unsigned long)(p3transfr_32.tx_buffer);
	dup.rx_buffer = (unsigned char *)(unsigned long)(p3transfr_32.rx_buffer);
	dup.len = p3transfr_32.len;

	err = p3_xfer(p3_device, &dup);
	if (err != 0) {
		P3_ERR_MSG("%s xfer failed!\n", __func__);
		return err;
	}
	P3_DBG_MSG("%s len:%u\n", __func__, dup.len);

	return 0;
}
#endif
#endif

static int spip3_open(struct inode *inode, struct file *filp)
{
	struct p3_data *p3_dev = container_of(filp->private_data,
			struct p3_data, p3_device);
	int ret = 0;

	/* for defence MULTI-OPEN */
	if (p3_dev->device_opened) {
		P3_ERR_MSG("%s - ALREADY opened!\n", __func__);
		return -EBUSY;
	}

	mutex_lock(&device_list_lock);
	p3_dev->device_opened = true;
	P3_INFO_MSG("open\n");

#ifdef FEATURE_ESE_WAKELOCK
	wake_lock(&p3_dev->ese_lock);
#endif

#ifdef CONFIG_ESE_SECURE
	/*Qcom TZ seems it can control spi clk by itself */
	p3_clk_control(p3_dev, true);
#else
	// TODO pinctrl need to be done here for nonTZ
	//;pinctrl_config(p3_dev->p3_device.parent, true);
#endif

	if (of_get_property(p3_dev->p3_device.parent->of_node, "ese,ldo_control", NULL)) {
		ret = p3_regulator_onoff(p3_dev, 1);
		if (ret < 0)
			P3_ERR_MSG(" test: failed to turn on LDO()\n");
	} else {
		gpio_direction_output(p3_dev->ese_pvdd_en, 1);
		P3_INFO_MSG("%s pvdd-gpio has set.\n", __func__);
	}

	usleep_range(2000, 2500);

	filp->private_data = p3_dev;

	p3_dev->users++;
	mutex_unlock(&device_list_lock);

	return 0;
}

static int spip3_release(struct inode *inode, struct file *filp)
{
	struct p3_data *p3_dev = filp->private_data;
	int ret = 0;

	if (!p3_dev->device_opened) {
		P3_ERR_MSG("%s - was NOT opened....\n", __func__);
		return 0;
	}

	mutex_lock(&device_list_lock);

#ifdef FEATURE_ESE_WAKELOCK
	if (wake_lock_active(&p3_dev->ese_lock))
		wake_unlock(&p3_dev->ese_lock);
#endif

	filp->private_data = p3_dev;

	p3_dev->users--;
	if (!p3_dev->users) {
		p3_dev->device_opened = false;

		if (of_get_property(p3_dev->p3_device.parent->of_node, "ese,ldo_control", NULL)) {
			ret = p3_regulator_onoff(p3_dev, 0);
			if (ret < 0)
				P3_ERR_MSG(" test: failed to turn off LDO()\n");
		} else {
			gpio_direction_output(p3_dev->ese_pvdd_en, 0);
		}

#ifdef CONFIG_ESE_SECURE
	/*Qcom TZ seems it can control spi clk by itself */
	p3_clk_control(p3_dev, false);
	usleep_range(1000, 1500);
#else
	// TODO need to set pinctrl soon
	//p3_pinctrl_config(p3_dev->p3_device.parent, false);
#endif
	}
	mutex_unlock(&device_list_lock);

	P3_DBG_MSG("%s, users:%d, Major Minor No:%d %d\n", __func__,
			p3_dev->users, imajor(inode), iminor(inode));
	return 0;
}

static long spip3_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	int ret = 0;
	struct p3_data *data = NULL;

	if (_IOC_TYPE(cmd) != P3_MAGIC) {
		P3_ERR_MSG("%s invalid magic. cmd=0x%X Received=0x%X Expected=0x%X\n",
				__func__, cmd, _IOC_TYPE(cmd), P3_MAGIC);
		return -ENOTTY;
	}

	data = filp->private_data;

	mutex_lock(&data->buffer_mutex);
	switch (cmd) {
	case P3_SET_DBG:
		debug_level = (unsigned char)arg;
		P3_DBG_MSG(KERN_INFO"[NXP-P3] -  Debug level %d", debug_level);
		break;
	case P3_ENABLE_SPI_CLK:
		P3_DBG_MSG("%s P3_ENABLE_SPI_CLK\n", __func__);
#ifdef CONFIG_ESE_SECURE
		ret = p3_clk_control(data, true);
		if (ret < 0)
			P3_ERR_MSG("%s: Unable to enable spi clk\n", __func__);
#endif
		break;
	case P3_DISABLE_SPI_CLK:
		P3_DBG_MSG("%s P3_DISABLE_SPI_CLK\n", __func__);
#ifdef CONFIG_ESE_SECURE
		ret = p3_clk_control(data, false);
		if (ret < 0)
			P3_ERR_MSG("%s: couldn't disable spi clks\n", __func__);
#endif
		break;
#ifndef CONFIG_ESE_SECURE
	case P3_RW_SPI_DATA: /*Not using actually */
		ret = p3_rw_spi_message(data, arg);
		if (ret < 0)
			P3_ERR_MSG("%s P3_RW_SPI_DATA failed [%d].\n",
					__func__, ret);
		break;
#endif
#if 0 //def CONFIG_COMPAT
	case P3_RW_SPI_DATA_32:
		ret = p3_rw_spi_message_32(data, arg);
		if (ret < 0)
			P3_ERR_MSG("%s P3_RW_SPI_DATA_32 failed [%d].\n",
					__func__, ret);
		break;
#endif

	case P3_SET_PWR:
	case P3_SET_POLL:
	case P3_SET_SPI_CLK:
	case P3_ENABLE_SPI_CS:
	case P3_DISABLE_SPI_CS:
	case P3_ENABLE_CLK_CS:
	case P3_DISABLE_CLK_CS:
	case P3_SWING_CS:
		P3_ERR_MSG("%s deprecated IOCTL:0x%X\n", __func__, cmd);
		break;

	default:
		P3_DBG_MSG("%s no matching ioctl! 0x%X\n", __func__, cmd);
		ret = -EINVAL;
	}
	mutex_unlock(&data->buffer_mutex);

	return ret;
}

#ifndef CONFIG_ESE_SECURE
static ssize_t spip3_write(struct file *filp, const char *buf, size_t count,
		loff_t *offset)
{
	int ret = -1;
	struct p3_data *p3_dev;
	int gpio_pvdd_en = 0;
	int i;
#ifdef FEATURE_ESE_SPI_DUMMY_ENABLE
	int dummy = 0;
#endif

	p3_dev = filp->private_data;

	mutex_lock(&p3_dev->buffer_mutex);
	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	/*memset(p3_dev->buf, 0, count+1);*/
	if (copy_from_user(p3_dev->buf, &buf[0], count)) {
		P3_ERR_MSG("%s: failed to copy from user space\n", __func__);
		mutex_unlock(&p3_dev->buffer_mutex);
		return -EFAULT;
	}

#ifdef FEATURE_ESE_SPI_DUMMY_ENABLE
	/* if data size is not aligned, it makes spi clk gap.
         * and it leads spi read failure. 
	 * so, if data size is not algined, add dummy data.
	 */
	if (!strcmp(p3_dev->ap_vendor, "qualcomm")) {
		int SPI_MAX_BYTES_PER_WORD = 4;

		if (count % SPI_MAX_BYTES_PER_WORD) {
			dummy = SPI_MAX_BYTES_PER_WORD - count % SPI_MAX_BYTES_PER_WORD;
			if (count + dummy < MAX_BUFFER_SIZE) {
				memset(p3_dev->buf + count, 0, dummy);
				count +=  dummy;

				P3_INFO_MSG("%s: %d byte dummy is added. count is changed to %zu\n",
						__func__, dummy, count);
			}
		}
	}
#endif

	/* Write data */
	ret = spi_write(p3_dev->spi, p3_dev->buf, count);
#ifdef FEATURE_ESE_SPI_DUMMY_ENABLE
	count -= dummy;
#endif
	if (ret < 0)
		ret = -EIO;
	else
		ret = count;

	mutex_unlock(&p3_dev->buffer_mutex);

	if (p3_dev->ese_pvdd_en > 0)
		gpio_pvdd_en = gpio_get_value(p3_dev->ese_pvdd_en);

	P3_INFO_MSG("%s: count:%zu, ret:%d, pvdd_en:%d\n",
		__func__, count, ret, gpio_pvdd_en);

	pr_cont("[ESE-P3] : %s: write data :", __func__);
	for (i = 0; i < count; i++)
		pr_cont("%02x ", p3_dev->buf[i]);
	pr_cont("\n");

	return ret;
}

static ssize_t spip3_read(struct file *filp, char *buf, size_t count,
		loff_t *offset)
{
	int ret = -EIO;
	struct p3_data *p3_dev = filp->private_data;
	int gpio_pvdd_en = 0;
	int i;

	mutex_lock(&p3_dev->buffer_mutex);

	/*memset(p3_dev->buf, 0, count+1);*/

	/* Read the available data along with one byte LRC */
	ret = spi_read(p3_dev->spi, (void *)p3_dev->buf, count);
	if (ret < 0) {
		P3_ERR_MSG("spi_read failed\n");
		ret = -EIO;
		goto fail;
	}

	if (copy_to_user(buf, p3_dev->buf, count)) {
		P3_ERR_MSG("%s : failed to copy to user space\n", __func__);
		ret = -EFAULT;
		goto fail;
	}

	if (p3_dev->ese_pvdd_en > 0)
		gpio_pvdd_en = gpio_get_value(p3_dev->ese_pvdd_en);

	P3_INFO_MSG("%s: count:%zu, ret:%d, pvdd_en:%d\n",
		__func__, count, ret, gpio_pvdd_en);

	pr_cont("[ESE-P3] : %s: read data :", __func__);
	for (i = 0; i < count; i++)
		pr_cont("%02x ", p3_dev->buf[i]);
	pr_cont("\n");

	ret = count;

	mutex_unlock(&p3_dev->buffer_mutex);

	return ret;

fail:
	P3_ERR_MSG("Error %s ret %d Exit\n", __func__, ret);
	mutex_unlock(&p3_dev->buffer_mutex);
	return ret;
}
#endif
/* possible fops on the p3 device */
static const struct file_operations spip3_dev_fops = {
	.owner = THIS_MODULE,
#ifndef CONFIG_ESE_SECURE
	.read = spip3_read,
	.write = spip3_write,
#endif
	.open = spip3_open,
	.release = spip3_release,
	.unlocked_ioctl = spip3_ioctl,
};

static int p3_parse_dt(struct device *dev, struct p3_data *data)
{
	struct device_node *np = dev->of_node;
	int ret = 0;

	if (of_get_property(dev->of_node, "ese,ldo_control", NULL)) {
		if (of_property_read_string(np, "ese,1p8_pvdd",
					&data->vdd_1p8) < 0) {
			pr_err("%s - getting vdd_1p8 error\n", __func__);
			data->vdd_1p8 = NULL;
			ret = -1;
		} else
			pr_info("%s success vdd:%s\n", __func__, data->vdd_1p8);
	
	} else { /* GPIO*/
		data->ese_pvdd_en = of_get_named_gpio(np, "ese,pvdd_gpio", 0);
		if (data->ese_pvdd_en < 0) {
			pr_err("%s - fail get nfc_ese_pwr_req\n", __func__);
			ret = -1;
		} else {
			ret = gpio_request(data->ese_pvdd_en, "ese_pvdd_en");
			if (ret) {
				P3_ERR_MSG("%s - failed to request ese_pvdd_en\n", __func__);
				ret = -1;
			}
		}
	}

	/*Vdata->pvdd = of_get_named_gpio(np, "ese,pvdd-gpio", 0);
	 * if (data->pvdd < 0) {
	 *	pr_info("%s : pvdd-gpio is not set.", __func__);
	 *	data->pvdd = 0;
	 *}
	 */

	if (!of_property_read_string(np, "ese,ap_vendor",
		&data->ap_vendor)) {
		pr_info("%s: ap_vendor - %s\n", __func__, data->ap_vendor);
	} else
		data->ap_vendor = "default";

	return ret;
}

static int spip3_probe(struct spi_device *spi)
{
	int ret = -1;
	struct p3_data *data = NULL;
#ifdef CONFIG_SPI_QCOM_GENI /*SDM845 Only*/
	struct spi_geni_qcom_ctrl_data *delay_params = NULL;
#endif
	P3_INFO_MSG("%s chip select : %d , bus number = %d\n",
		__func__, spi->chip_select, spi->master->bus_num);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		P3_ERR_MSG("failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	ret = p3_parse_dt(&spi->dev, data);
	if (ret) {
		P3_ERR_MSG("%s - Failed to parse DT\n", __func__);
		goto p3_parse_dt_failed;
	}

	if (!strncmp(data->ap_vendor, "qualcomm", 7)) {/* "default" has 7 bit*/
#ifdef CONFIG_SPI_QCOM_GENI /*SDM845 Only*/
		delay_params = spi->controller_data;
		if (spi->controller_data)
			pr_err("%s ctrl data is not empty\n", __func__);

		delay_params = devm_kzalloc(&spi->dev, sizeof(struct spi_geni_qcom_ctrl_data),
			GFP_KERNEL);
		pr_info("%s success alloc ctrl_data!\n", __func__);
		delay_params->spi_cs_clk_delay = 35; /*clock cycles*/
		delay_params->spi_inter_words_delay = 0;
		spi->controller_data = delay_params;
#endif
	}
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	spi->max_speed_hz = SPI_DEFAULT_SPEED;
#ifndef CONFIG_ESE_SECURE
	ret = spi_setup(spi);
	if (ret < 0) {
		P3_ERR_MSG("failed to do spi_setup()\n");
		goto p3_parse_dt_failed;
	}
#else
	pr_info("%s: eSE Secured system\n", __func__);
	ret = p3_clk_setup(&spi->dev, data);
	if (ret)
		P3_ERR_MSG("%s - Failed to do clk_setup\n", __func__);
#endif

	data->speed = SPI_DEFAULT_SPEED;
	data->spi = spi;
	data->p3_device.minor = MISC_DYNAMIC_MINOR;
	data->p3_device.name = "p3";
	data->p3_device.fops = &spip3_dev_fops;
	data->p3_device.parent = &spi->dev;

	dev_set_drvdata(&spi->dev, data);

	/* init mutex and queues */
	init_waitqueue_head(&data->read_wq);
	mutex_init(&data->buffer_mutex);
#ifdef FEATURE_ESE_WAKELOCK
	wake_lock_init(&data->ese_lock,
		WAKE_LOCK_SUSPEND, "ese_wake_lock");
#endif

	data->device_opened = false;

	ret = misc_register(&data->p3_device);
	if (ret < 0) {
		P3_ERR_MSG("misc_register failed! %d\n", ret);
		goto err_misc_regi;
	}

#ifdef CONFIG_ESE_SECURE
#else
//todo : need to set soon
	//_pinctrl_config(&spi->dev, false);
#endif

	data->buf = kzalloc(sizeof(unsigned char) * MAX_BUFFER_SIZE, GFP_KERNEL);
	if (data->buf == NULL) {
		P3_ERR_MSG("failed to allocate for spi buf\n");
		ret = -ENOMEM;
		goto err_ldo_off;
	}

	P3_INFO_MSG("%s finished...\n", __func__);
	return ret;

err_ldo_off:
	misc_deregister(&data->p3_device);
err_misc_regi:
#ifdef FEATURE_ESE_WAKELOCK
	wake_lock_destroy(&data->ese_lock);
#endif
	mutex_destroy(&data->buffer_mutex);
p3_parse_dt_failed:
	kfree(data);
err_exit:
	P3_DBG_MSG("ERROR: Exit : %s ret %d\n", __func__, ret);
	return ret;
}

static int spip3_remove(struct spi_device *spi)
{
	struct p3_data *p3_dev = dev_get_drvdata(&spi->dev);

	P3_DBG_MSG("Entry : %s\n", __func__);
	if (p3_dev == NULL) {
		P3_ERR_MSG("%s p3_dev is null!\n", __func__);
		return 0;
	}

#ifdef FEATURE_ESE_WAKELOCK
	wake_lock_destroy(&p3_dev->ese_lock);
#endif
	mutex_destroy(&p3_dev->buffer_mutex);
	misc_deregister(&p3_dev->p3_device);
	kfree(p3_dev->buf);
	kfree(p3_dev);
	P3_DBG_MSG("Exit : %s\n", __func__);
	return 0;
}

static const struct of_device_id p3_match_table[] = {
	{ .compatible = "ese_p3",},
	{},
};

static struct spi_driver spip3_driver = {
	.driver = {
		.name = "p3",
		.bus = &spi_bus_type,
		.owner = THIS_MODULE,
		.of_match_table = p3_match_table,
	},
	.probe =  spip3_probe,
	.remove = spip3_remove,
};

static int __init spip3_dev_init(void)
{
	debug_level = P3_FULL_DEBUG;

	P3_INFO_MSG("Entry : %s\n", __func__);
#if (!defined(CONFIG_ESE_FACTORY_ONLY) || defined(CONFIG_SEC_FACTORY))
	return spi_register_driver(&spip3_driver);
#else
	return -1;
#endif
}

static void __exit spip3_dev_exit(void)
{
	P3_INFO_MSG("Entry : %s\n", __func__);
	spi_unregister_driver(&spip3_driver);
}

module_init(spip3_dev_init);
module_exit(spip3_dev_exit);

MODULE_AUTHOR("Sec");
MODULE_DESCRIPTION("ese SPI driver");
MODULE_LICENSE("GPL");

/** @} */
