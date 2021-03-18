/*
 * drivers/input/touchscreen/onetap2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 * Copyright (c) 2015, Vineeth Raj <contact.twn@openmailbox.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/hrtimer.h>
#include <asm-generic/cputime.h>
#include <linux/input/onetap2wake.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#ifdef CONFIG_POCKETMOD
#include <linux/pocket_mod.h>
#endif

/* uncomment since no touchscreen defines android touch, do that here */
//#define ANDROID_TOUCH_DECLARED

#define WAKE_HOOKS_DEFINED

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
#include <linux/lcd_notify.h>
#else
#include <linux/earlysuspend.h>
#endif
#endif

/* if Sweep2Wake is compiled it will already have taken care of this */
#ifdef CONFIG_TOUCHSCREEN_SWEEP2WAKE
#define ANDROID_TOUCH_DECLARED
#endif

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Tanish <tanish2k09.dev@gmail.com>"
#define DRIVER_DESCRIPTION "Onetap2wake for almost any device"
#define DRIVER_VERSION "2.0"
#define LOGTAG "[onetap2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv3");

/* Tuneables */
#define OT2W_DEBUG         0
#define OT2W_DEFAULT       1

#define OT2W_PWRKEY_DUR   20
#define OT2W_OFF 		  0
#define OT2W_ON 		  1

/* Wake Gestures */
//#define WAKE_GESTURE		0x0b
//#define TRIGGER_TIMEOUT		50

static struct input_dev *gesture_dev;
//extern int gestures_switch;

/* Resources */
int ot2w_switch = OT2W_DEFAULT;
static cputime64_t tap_time_pre = 0;
static int touch_x = 0, touch_y = 0, touch_nr = 0, x_pre = 0, y_pre = 0;
static bool touch_x_called = false, touch_y_called = false, touch_cnt = true;
bool ot2w_scr_suspended = false;
bool in_phone_call = false;
//static unsigned long pwrtrigger_time[2] = {0, 0};
#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
static struct notifier_block ot2w_lcd_notif;
#endif
#endif
static struct input_dev * onetap2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static struct workqueue_struct *ot2w_input_wq;
static struct work_struct ot2w_input_work;


/* Read cmdline for ot2w */
static int __init read_ot2w_cmdline(char *ot2w)
{
	if (strcmp(ot2w, "1") == 0) {
		pr_info("[cmdline_ot2w]: OneTap2Wake enabled. | ot2w='%s'\n", ot2w);
		ot2w_switch = 1;
	} else if (strcmp(ot2w, "0") == 0) {
		pr_info("[cmdline_ot2w]: OneTap2Wake disabled. | ot2w='%s'\n", ot2w);
		ot2w_switch = 0;
	} else {
		pr_info("[cmdline_ot2w]: No valid input found. Going with default: | ot2w='%u'\n", ot2w_switch);
	}
	return 1;
}
__setup("ot2w=", read_ot2w_cmdline);

/* Wake Gestures */
void gestures_setdev(struct input_dev *input_device)
{
	gesture_dev = input_device;
	return;
}

/* PowerKey work func */
static void onetap2wake_presspwr(struct work_struct * onetap2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
		return;
	input_event(onetap2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(onetap2wake_pwrdev, EV_SYN, 0, 0);
	msleep(OT2W_PWRKEY_DUR);
	input_event(onetap2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(onetap2wake_pwrdev, EV_SYN, 0, 0);
	msleep(OT2W_PWRKEY_DUR);
	mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(onetap2wake_presspwr_work, onetap2wake_presspwr);

/* PowerKey trigger */
static void onetap2wake_pwrtrigger(void) {
	schedule_work(&onetap2wake_presspwr_work);
	return;
}

static void ot2w_input_callback(struct work_struct *unused) {

#ifdef CONFIG_POCKETMOD
  	if (device_is_pocketed()){
       onetap2wake_pwrtrigger();
       return;
  	}
#endif

	onetap2wake_pwrtrigger();  // Queue the screen wake
	return;
}

static void ot2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
#if OT2W_DEBUG
	pr_info("onetap2wake: code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		((code==ABS_MT_TRACKING_ID)||
			(code==330)) ? "ID" : "undef"), code, value);
#endif

	if (in_phone_call){
		return;
    }

	if (!(ot2w_scr_suspended)){
    	return;
    }
	if (code == ABS_MT_SLOT) {
		return;
	}

	/*
	 * '330'? Many touch panels are 'broken' in the sense of not following the
	 * multi-touch protocol given in Documentation/input/multi-touch-protocol.txt.
	 * According to the docs, touch panels using the type B protocol must send in
	 * a ABS_MT_TRACKING_ID event after lifting the contact in the first slot.
	 * This should in the flow of events, help us set the necessary onetap2wake
	 * variable and proceed as per the algorithm.
	 *
	 * This however is not the case with various touch panel drivers, and hence
	 * there is no reliable way of tracking ABS_MT_TRACKING_ID on such panels.
	 * Some of the panels however do track the lifting of contact, but with a
	 * different event code, and a different event value.
	 *
	 * So, add checks for those event codes and values to keep the algo flow.
	 *
	 * synaptics_s3203 => code: 330; val: 0
	 *
	 * Note however that this is not possible with panels like the CYTTSP3 panel
	 * where there are no such events being reported for the lifting of contacts
	 * though i2c data has a ABS_MT_TRACKING_ID or equivalent event variable
	 * present. In such a case, make sure the touch_cnt variable is publicly
	 * available for modification.
	 *
	 */
	if ((code == ABS_MT_TRACKING_ID && value == -1) || (code == 330 && value == 0)) {
		return;
	}

	if (code == ABS_MT_POSITION_X) {
		touch_x = value;
		touch_x_called = true;
	}

	if (code == ABS_MT_POSITION_Y) {
		touch_y = value;
		touch_y_called = true;
	}

	if (touch_x_called || touch_y_called) {
		touch_x_called = false;
		touch_y_called = false;
		queue_work_on(0, ot2w_input_wq, &ot2w_input_work);
	}
}

static int input_dev_filter(struct input_dev *dev) {
	if (strstr(dev->name, "touch")||
			strstr(dev->name, "mtk-tpd")) {
		return 0;
	} else {
		return 1;
	}
}

static int ot2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "ot2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void ot2w_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id ot2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler ot2w_input_handler = {
	.event		= ot2w_input_event,
	.connect	= ot2w_input_connect,
	.disconnect	= ot2w_input_disconnect,
	.name		= "ot2w_inputreq",
	.id_table	= ot2w_ids,
};

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
	case LCD_EVENT_ON_END:
		ot2w_scr_suspended = false;
		break;
	case LCD_EVENT_OFF_END:
		ot2w_scr_suspended = true;
		break;
	default:
		break;
	}

	return 0;
}
#else
static void ot2w_early_suspend(struct early_suspend *h) {
	ot2w_scr_suspended = true;
}

static void ot2w_late_resume(struct early_suspend *h) {
	ot2w_scr_suspended = false;
}

static struct early_suspend ot2w_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = ot2w_early_suspend,
	.resume = ot2w_late_resume,
};
#endif
#endif

/*
 * SYSFS stuff below here
 */
static ssize_t ot2w_onetap2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", ot2w_switch);

	return count;
}

static ssize_t ot2w_onetap2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int new_ot2w_switch;

	if (!sscanf(buf, "%du", &new_ot2w_switch))
		return -EINVAL;

	if (new_ot2w_switch == ot2w_switch)
		return count;

	switch (new_ot2w_switch) {
		case OT2W_OFF :
		case OT2W_ON :
			ot2w_switch = new_ot2w_switch;
			/* through 'adb shell' or by other means, if the toggle
			 * is done several times, 0-to-1, 1-to-0, we need to
			 * inform the toggle correctly
			 */
			pr_info("[dump_ot2w]: OneTap2Wake toggled. | "
					"ot2w='%d' \n", ot2w_switch);
			return count;
		default:
			return -EINVAL;
	}

	/* We should never get here */
	return -EINVAL;
}

static DEVICE_ATTR(onetap2wake, 0666,
	ot2w_onetap2wake_show, ot2w_onetap2wake_dump);

static ssize_t ot2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%s\n", DRIVER_VERSION);

	return count;
}

static ssize_t ot2w_version_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(onetap2wake_version, (S_IWUSR|S_IRUGO),
	ot2w_version_show, ot2w_version_dump);

/*
 * INIT / EXIT stuff below here
 */
#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *android_touch_kobj;
#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
#endif

static int __init onetap2wake_init(void)
{
	int rc = 0;

	onetap2wake_pwrdev = input_allocate_device();
	if (!onetap2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(onetap2wake_pwrdev, EV_KEY, KEY_POWER);
	onetap2wake_pwrdev->name = "ot2w_pwrkey";
	onetap2wake_pwrdev->phys = "ot2w_pwrkey/input0";

	rc = input_register_device(onetap2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}
	ot2w_input_wq = create_workqueue("ot2wiwq");
	if (!ot2w_input_wq) {
		pr_err("%s: Failed to create ot2wiwq workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&ot2w_input_work, ot2w_input_callback);
	rc = input_register_handler(&ot2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register ot2w_input_handler\n", __func__);

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
	ot2w_lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&ot2w_lcd_notif) != 0) {
		pr_err("%s: Failed to register lcd callback\n", __func__);
	}
#else
	register_early_suspend(&ot2w_early_suspend_handler);
#endif
#endif

#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("android_touch", NULL) ;
	if (android_touch_kobj == NULL) {
		pr_warn("%s: android_touch_kobj create_and_add failed\n", __func__);
	}
#endif
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_onetap2wake.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for onetap2wake\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_onetap2wake_version.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for onetap2wake_version\n", __func__);
	}

err_input_dev:
	input_free_device(onetap2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);
	return 0;
}

static void __exit onetap2wake_exit(void)
{
#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(android_touch_kobj);
#endif
#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
	lcd_unregister_client(&ot2w_lcd_notif);
#endif
#endif
	input_unregister_handler(&ot2w_input_handler);
	destroy_workqueue(ot2w_input_wq);
	input_unregister_device(onetap2wake_pwrdev);
	input_free_device(onetap2wake_pwrdev);
	return;
}

module_init(onetap2wake_init);
module_exit(onetap2wake_exit);
