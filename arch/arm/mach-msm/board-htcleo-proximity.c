/* arch/arm/mach-msm/board-htcleo-microp.c
 * Copyright (C) 2009 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/earlysuspend.h>
#include <asm/mach-types.h>
#include <mach/board-htcleo-microp.h>
#include <linux/input.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <mach/vreg.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>

#include "board-htcleo.h"

#define INT_PSENSOR	(1<<4)

#define D(x...) pr_debug(x)

static int misc_opened;

static void p_sensor_do_work(struct work_struct *w);
static DECLARE_WORK(p_sensor_work, p_sensor_do_work);

struct wake_lock proximity_wake_lock;

static struct capella_cm3602_data {
	struct input_dev *input_dev;
	struct capella_cm3602_platform_data *pdata;
	int enabled;
	struct workqueue_struct *p_sensor_wq;
} the_data;

static int report_psensor_data(void)
{
	int ret, ps_data = 0;
	uint8_t data[3] = {0, 0, 0};
	D("%s\n", __func__);

	ret = microp_i2c_read(MICROP_I2C_RCMD_GPIO_STATUS, data, 3);
	if (ret < 0)
		pr_err("%s: read data failed\n", __func__);
	else {
		ps_data = (data[1] & 0x1) ? 1 : 0;
		D("proximity %s\n", ps_data ? "FAR" : "NEAR");

		/* 0 is close, 1 is far */
		input_report_abs(the_data.input_dev, ABS_DISTANCE, ps_data);
		input_sync(the_data.input_dev);

		wake_lock_timeout(&proximity_wake_lock, 2*HZ);
	}

	return ret;
}

static int capella_cm3602_enable(struct capella_cm3602_data *data)
{
	int rc;
	if (data->enabled) {
		D("%s: already enabled\n", __func__);
		return 0;
	}

	/* dummy report */
	input_report_abs(data->input_dev, ABS_DISTANCE, -1);
	input_sync(data->input_dev);

	rc = capella_cm3602_power(PS_PWR_ON, 1);
	if (rc < 0)
		return -EIO;

	data->enabled = 1;
	report_psensor_data();

	return rc;
}

static int capella_cm3602_disable(struct capella_cm3602_data *data)
{
	int rc = -EIO;
	if (!data->enabled) {
		D("%s: already disabled\n", __func__);
		return 0;
	}

	rc = capella_cm3602_power(PS_PWR_ON, 0);
	if (rc < 0)
		return -EIO;

	data->enabled = 0;
	return rc;
}

static ssize_t capella_cm3602_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;

	ret = sprintf(buf, "proximity enabled = %d\n", the_data.enabled);

	return ret;
}

static ssize_t capella_cm3602_store(struct device *dev,
			struct device_attribute *attr,
			const char              *buf,
			size_t                  count
			)
{
	ssize_t val;

	val = -1;
	sscanf(buf, "%u", &val);
	if (val < 0 || val > 1)
		return -EINVAL;

	/* Enable capella_cm3602*/
	if (val == 1)
		capella_cm3602_enable(&the_data);

	/* Disable capella_cm3602*/
	if (val == 0)
		capella_cm3602_disable(&the_data);

	return count;
}

static DEVICE_ATTR(proximity, 0644, capella_cm3602_show, capella_cm3602_store);

static int capella_cm3602_open(struct inode *inode, struct file *file)
{
	if (misc_opened)
		return -EBUSY;
	misc_opened = 1;
	return 0;
}

static int capella_cm3602_release(struct inode *inode, struct file *file)
{
	misc_opened = 0;
	return capella_cm3602_disable(&the_data);
}

static long capella_cm3602_ioctl(struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int val;
	D("%s cmd %d\n", __func__, _IOC_NR(cmd));
	switch (cmd) {
	case CAPELLA_CM3602_IOCTL_ENABLE:
		if (get_user(val, (unsigned long __user *)arg))
			return -EFAULT;
		if (val)
			return capella_cm3602_enable(&the_data);
		else
			return capella_cm3602_disable(&the_data);
		break;
	case CAPELLA_CM3602_IOCTL_GET_ENABLED:
		return put_user(the_data.enabled, (unsigned long __user *)arg);
		break;
	default:
		pr_err("%s: invalid cmd %d\n", __func__, _IOC_NR(cmd));
		return -EINVAL;
	}
}

static void p_sensor_do_work(struct work_struct *w)
{
	report_psensor_data();
}

void p_sensor_irq_handler(void)
{
	queue_work(the_data.p_sensor_wq, &p_sensor_work);
}


static struct file_operations capella_cm3602_fops = {
	.owner = THIS_MODULE,
	.open = capella_cm3602_open,
	.release = capella_cm3602_release,
	.unlocked_ioctl = capella_cm3602_ioctl
};

static struct miscdevice capella_cm3602_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cm3602",
	.fops = &capella_cm3602_fops
};

static int capella_cm3602_probe(struct platform_device *pdev)
{
	int rc = -1;
	struct input_dev *input_dev;
	struct capella_cm3602_data *ip;
	struct capella_cm3602_platform_data *pdata;

	struct class  *proximity_attr_class;
	struct device *proximity_attr_dev;

	pr_info("%s: probe\n", __func__);

	pdata = dev_get_platdata(&pdev->dev);

	ip = &the_data;
	platform_set_drvdata(pdev, ip);

	input_dev = input_allocate_device();
	if (!input_dev) {
		pr_err("%s: could not allocate input device\n", __func__);
		rc = -ENOMEM;
		goto done;
	}
	ip->input_dev = input_dev;
	ip->pdata = pdata;
	input_set_drvdata(input_dev, ip);

	input_dev->name = "proximity";

	set_bit(EV_ABS, input_dev->evbit);
	input_set_abs_params(input_dev, ABS_DISTANCE, 0, 1, 0, 0);

	rc = input_register_device(input_dev);
	if (rc < 0) {
		pr_err("%s: could not register input device\n", __func__);
		goto err_free_input_device;
	}

	rc = misc_register(&capella_cm3602_misc);
	if (rc < 0) {
		pr_err("%s: could not register misc device\n", __func__);
		goto err_unregister_input_device;
	}

	wake_lock_init(&proximity_wake_lock, WAKE_LOCK_SUSPEND, "proximity");

	proximity_attr_class = class_create(THIS_MODULE, "sensors");
	if (IS_ERR(proximity_attr_class)) {
		pr_err("%s: class_create failed\n", __func__);
		rc = PTR_ERR(proximity_attr_class);
		proximity_attr_class = NULL;
		goto err_create_class;
	}

	proximity_attr_dev = device_create(proximity_attr_class,
					NULL, 0, "%s", "proximity_sensor");
	if (unlikely(IS_ERR(proximity_attr_dev))) {
		pr_err("%s: device create failed\n", __func__);
		rc = PTR_ERR(proximity_attr_dev);
		proximity_attr_dev = NULL;
		goto err_create_proximity_attr_device;
	}

	rc = device_create_file(proximity_attr_dev, &dev_attr_proximity);
	if (rc) {
		pr_err("%s: device_create_file failed\n", __func__);
		goto err_create_proximity_device_file;
	}

	ip->p_sensor_wq = create_workqueue("p-sensor_microp_wq");
	if (ip->p_sensor_wq == NULL) {
		pr_err("%s: create_workqueue failed\n", __func__);
		goto err_create_workqueue;
	}

	goto done;


err_create_workqueue:
	device_remove_file(proximity_attr_dev, &dev_attr_proximity);
err_create_proximity_device_file:
	device_unregister(proximity_attr_dev);
err_create_proximity_attr_device:
	class_destroy(proximity_attr_class);
err_create_class:
	misc_deregister(&capella_cm3602_misc);
err_unregister_input_device:
	input_unregister_device(input_dev);
err_free_input_device:
	input_free_device(input_dev);
done:
	return rc;
}

static struct platform_driver capella_cm3602_driver = {
	.probe = capella_cm3602_probe,
	.driver = {
		.name = "htcleo-proximity",
		.owner = THIS_MODULE
	},
};

static int __init htcleo_capella_cm3602_init(void)
{
	return platform_driver_register(&capella_cm3602_driver);
}

device_initcall(htcleo_capella_cm3602_init);

MODULE_DESCRIPTION("HTC LEO Touchscreen Support Driver");
MODULE_LICENSE("GPL");
