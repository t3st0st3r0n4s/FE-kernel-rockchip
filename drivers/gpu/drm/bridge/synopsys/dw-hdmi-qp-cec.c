// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author:
 *      Algea Cao <algea.cao@rock-chips.com>
 */
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include <drm/drm_edid.h>
#include <drm/bridge/dw_hdmi.h>

#include <media/cec.h>
#include <media/cec-notifier.h>

#include "dw-hdmi-qp-cec.h"

enum {
	CEC_TX_CONTROL		= 0x1000,
	CEC_CTRL_CLEAR		= BIT(0),
	CEC_CTRL_START		= BIT(0),

	CEC_STAT_DONE		= BIT(0),
	CEC_STAT_NACK		= BIT(1),
	CEC_STAT_ARBLOST	= BIT(2),
	CEC_STAT_LINE_ERR	= BIT(3),
	CEC_STAT_RETRANS_FAIL	= BIT(4),
	CEC_STAT_DISCARD	= BIT(5),
	CEC_STAT_TX_BUSY	= BIT(8),
	CEC_STAT_RX_BUSY	= BIT(9),
	CEC_STAT_DRIVE_ERR	= BIT(10),
	CEC_STAT_EOM		= BIT(11),
	CEC_STAT_NOTIFY_ERR	= BIT(12),

	CEC_CONFIG		= 0x1008,
	CEC_ADDR		= 0x100c,
	CEC_TX_CNT		= 0x1020,
	CEC_RX_CNT		= 0x1040,
	CEC_TX_DATA3_0		= 0x1024,
	CEC_RX_DATA3_0		= 0x1044,
	CEC_LOCK_CONTROL	= 0x1054,

	CEC_INT_STATUS		= 0x4000,
	CEC_INT_MASK_N		= 0x4004,
	CEC_INT_CLEAR		= 0x4008,
};

#define CEC_EN			BIT(0)
#define CEC_WAKE		BIT(1)

#ifndef CEC_PHYS_ADDR_INVALID
#define CEC_PHYS_ADDR_INVALID	0xffff
#endif

static const char *dw_hdmi_qp_cec_opcode_name(u8 opcode)
{
	switch (opcode) {
	case 0x04:
		return "Image View On";
	case 0x0d:
		return "Text View On";
	case 0x36:
		return "Standby";
	case 0x80:
		return "Routing Change";
	case 0x81:
		return "Routing Information";
	case 0x82:
		return "Active Source";
	case 0x85:
		return "Request Active Source";
	case 0x86:
		return "Set Stream Path";
	default:
		return "other";
	}
}

static bool dw_hdmi_qp_cec_extract_route_path(const struct cec_msg *msg,
						 u16 *path)
{
	if (!msg || msg->len < 2)
		return false;

	switch (msg->msg[1]) {
	case 0x80: /* <Routing Change>: old path, new path */
		if (msg->len < 6)
			return false;
		*path = (msg->msg[4] << 8) | msg->msg[5];
		return true;
	case 0x81: /* <Routing Information> */
	case 0x82: /* <Active Source> */
	case 0x86: /* <Set Stream Path> */
		if (msg->len < 4)
			return false;
		*path = (msg->msg[2] << 8) | msg->msg[3];
		return true;
	default:
		return false;
	}
}

struct dw_hdmi_qp_cec {
	struct device *dev;
	struct dw_hdmi_qp *hdmi;
	struct miscdevice misc_dev;
	const struct dw_hdmi_qp_cec_ops *ops;
	u32 addresses;
	struct cec_adapter *adap;
	struct cec_msg rx_msg;
	unsigned int tx_status;
	bool tx_done;
	bool rx_done;
	struct cec_notifier *notify;
	struct input_dev *devinput;
	int irq;
	bool wake_en;
	bool standby_en;
	/*
	 * Rockchip HAL toggles the private CEC_EN bit before the Linux CEC
	 * adapter finishes unconfiguring logical addresses.  When CEC_EN is
	 * already false the QP CEC/HDMI register window can be unavailable;
	 * avoid touching CEC MMIO from .adap_log_addr() in that window.
	 */
	bool system_cec_enabled;
	struct mutex wake_lock;
};

static void dw_hdmi_qp_write(struct dw_hdmi_qp_cec *cec, u32 val, int offset)
{
	cec->ops->write(cec->hdmi, val, offset);
}

static u32 dw_hdmi_qp_read(struct dw_hdmi_qp_cec *cec, int offset)
{
	return cec->ops->read(cec->hdmi, offset);
}


static bool dw_hdmi_qp_cec_is_local_path_wake_msg(struct dw_hdmi_qp_cec *cec,
						  const struct cec_msg *msg, u16 *path)
{
	u16 local_path;

	if (!cec || !msg || !cec->adap)
		return false;

	if (!dw_hdmi_qp_cec_extract_route_path(msg, path))
		return false;

	/* Only routing-like messages carrying a physical path are considered. */
	switch (msg->msg[1]) {
	case 0x80: /* <Routing Change> */
	case 0x81: /* <Routing Information> */
	case 0x86: /* <Set Stream Path> */
		break;
	default:
		return false;
	}

	local_path = cec->adap->phys_addr;
	if (local_path == CEC_PHYS_ADDR_INVALID)
		return false;

	return *path == local_path;
}

static void dw_hdmi_qp_cec_inject_power_key_wake(struct dw_hdmi_qp_cec *cec,
						 const struct cec_msg *msg, u16 path)
{
	u8 opcode = msg->len >= 2 ? msg->msg[1] : 0xff;

	mutex_lock(&cec->wake_lock);

	if (!cec->standby_en || !cec->wake_en) {
		mutex_unlock(&cec->wake_lock);
		return;
	}

	if (!cec->devinput) {
		dev_warn(cec->dev,
			 "OPI5U CEC: wake skipped, no input device opcode=0x%02x(%s) path=0x%04x\n",
			 opcode, dw_hdmi_qp_cec_opcode_name(opcode), path);
		mutex_unlock(&cec->wake_lock);
		return;
	}

	/* One-shot wake for this standby interval, matching the old non-QP driver. */
	cec->standby_en = false;

	dev_info(cec->dev,
		 "OPI5U CEC: local-path standby wake opcode=0x%02x(%s) path=0x%04x\n",
		 opcode, dw_hdmi_qp_cec_opcode_name(opcode), path);

	input_event(cec->devinput, EV_KEY, KEY_POWER, 1);
	input_sync(cec->devinput);
	input_event(cec->devinput, EV_KEY, KEY_POWER, 0);
	input_sync(cec->devinput);

	mutex_unlock(&cec->wake_lock);
}

static int dw_hdmi_qp_cec_log_addr(struct cec_adapter *adap, u8 logical_addr)
{
	struct dw_hdmi_qp_cec *cec = cec_get_drvdata(adap);

	if (logical_addr == CEC_LOG_ADDR_INVALID)
		cec->addresses = 0;
	else
		cec->addresses |= BIT(logical_addr) | BIT(15);

	/*
	 * Main CEC disable path can arrive here after the Rockchip HAL has
	 * already cleared CEC_EN through /dev/rk_cec.  On RK3588 HDMI-QP this
	 * window can make the QP CEC register access below raise an asynchronous
	 * SError.  Keep the software address mask in sync, but skip MMIO while
	 * main CEC is disabled.  The next .adap_enable(true) re-enables MMIO and
	 * the CEC core will program logical addresses again.
	 */
	if (!cec->system_cec_enabled) {
		dev_info(cec->dev,
			 "OPI5U CEC: skip log_addr MMIO while main CEC disabled, la=0x%x mask=0x%x\n",
			 logical_addr, cec->addresses);
		return 0;
	}

	dw_hdmi_qp_write(cec, cec->addresses, CEC_ADDR);

	return 0;
}

static int dw_hdmi_qp_cec_transmit(struct cec_adapter *adap, u8 attempts,
				   u32 signal_free_time, struct cec_msg *msg)
{
	struct dw_hdmi_qp_cec *cec = cec_get_drvdata(adap);
	unsigned int i;
	u32 val;

	for (i = 0; i < msg->len; i++) {
		if (!(i % 4))
			val = msg->msg[i];
		if ((i % 4) == 1)
			val |= msg->msg[i] << 8;
		if ((i % 4) == 2)
			val |= msg->msg[i] << 16;
		if ((i % 4) == 3)
			val |= msg->msg[i] << 24;

		if (i == (msg->len - 1) || (i % 4) == 3)
			dw_hdmi_qp_write(cec, val, CEC_TX_DATA3_0 + (i / 4) * 4);
	}

	dw_hdmi_qp_write(cec, msg->len - 1, CEC_TX_CNT);
	dw_hdmi_qp_write(cec, CEC_CTRL_START, CEC_TX_CONTROL);


	return 0;
}

static irqreturn_t dw_hdmi_qp_cec_hardirq(int irq, void *data)
{
	struct cec_adapter *adap = data;
	struct dw_hdmi_qp_cec *cec = cec_get_drvdata(adap);
	u32 stat = dw_hdmi_qp_read(cec, CEC_INT_STATUS);
	irqreturn_t ret = IRQ_HANDLED;

	if (stat == 0)
		return IRQ_NONE;

	dw_hdmi_qp_write(cec, stat, CEC_INT_CLEAR);

	if (stat & CEC_STAT_LINE_ERR) {
		cec->tx_status = CEC_TX_STATUS_ERROR;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	} else if (stat & CEC_STAT_DONE) {
		cec->tx_status = CEC_TX_STATUS_OK;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	} else if (stat & CEC_STAT_NACK) {
		cec->tx_status = CEC_TX_STATUS_NACK;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	}

	if (stat & CEC_STAT_EOM) {
		unsigned int len, i, val;

		val = dw_hdmi_qp_read(cec, CEC_RX_CNT);
		len = (val & 0xf) + 1;

		if (len > sizeof(cec->rx_msg.msg))
			len = sizeof(cec->rx_msg.msg);

		for (i = 0; i < 4; i++) {
			val = dw_hdmi_qp_read(cec, CEC_RX_DATA3_0 + i * 4);
			cec->rx_msg.msg[i * 4] = val & 0xff;
			cec->rx_msg.msg[i * 4 + 1] = (val >> 8) & 0xff;
			cec->rx_msg.msg[i * 4 + 2] = (val >> 16) & 0xff;
			cec->rx_msg.msg[i * 4 + 3] = (val >> 24) & 0xff;
		}

		dw_hdmi_qp_write(cec, 1, CEC_LOCK_CONTROL);

		cec->rx_msg.len = len;
		cec->rx_done = true;

		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t dw_hdmi_qp_cec_thread(int irq, void *data)
{
	struct cec_adapter *adap = data;
	struct dw_hdmi_qp_cec *cec = cec_get_drvdata(adap);

	if (cec->tx_done) {
		cec->tx_done = false;
		cec_transmit_attempt_done(adap, cec->tx_status);
	}
	if (cec->rx_done) {
		u16 path = 0;
		bool wake = dw_hdmi_qp_cec_is_local_path_wake_msg(cec, &cec->rx_msg, &path);

		cec->rx_done = false;
		if (wake)
			dw_hdmi_qp_cec_inject_power_key_wake(cec, &cec->rx_msg, path);
		cec_received_msg(adap, &cec->rx_msg);
	}
	return IRQ_HANDLED;
}

static int dw_hdmi_qp_cec_enable(struct cec_adapter *adap, bool enable)
{
	struct dw_hdmi_qp_cec *cec = cec_get_drvdata(adap);
	unsigned int irqs = CEC_STAT_LINE_ERR | CEC_STAT_NACK | CEC_STAT_EOM |
			    CEC_STAT_DONE;

	if (!enable) {
		if (!cec->system_cec_enabled) {
			/*
			 * Main CEC toggle-off arrives here after the Rockchip HAL has
			 * already cleared CEC_EN through /dev/rk_cec.  On RK3588 HDMI-QP
			 * even ops->disable() can touch a clock-gated/dead register window
			 * and raise an async SError.  The Linux CEC framework requires
			 * adap_enable(false) to return 0, so treat the hardware as already
			 * disabled and avoid all QP CEC MMIO in this path.
			 */
			cec->wake_en = false;
			cec->standby_en = false;
			dev_info(cec->dev,
				 "OPI5U CEC: main CEC disabled, skip adap_enable(false) MMIO/ops->disable\n");
			return 0;
		}

		if (cec->wake_en && cec->standby_en) {
			/* Keep QP CEC RX IRQs armed while Android is in HDMI standby. */
			dw_hdmi_qp_write(cec, ~0, CEC_INT_CLEAR);
			dw_hdmi_qp_write(cec, 1, CEC_LOCK_CONTROL);
			dw_hdmi_qp_write(cec, irqs, CEC_INT_MASK_N);
		} else {
			dw_hdmi_qp_write(cec, 0, CEC_INT_MASK_N);
			dw_hdmi_qp_write(cec, ~0, CEC_INT_CLEAR);
			cec->ops->disable(cec->hdmi);
		}
	} else {
		cec->system_cec_enabled = true;
		cec->ops->enable(cec->hdmi);

		dw_hdmi_qp_write(cec, ~0, CEC_INT_CLEAR);
		dw_hdmi_qp_write(cec, 1, CEC_LOCK_CONTROL);

		dw_hdmi_qp_cec_log_addr(cec->adap, CEC_LOG_ADDR_INVALID);

		dw_hdmi_qp_write(cec, ~0, CEC_INT_CLEAR);
		dw_hdmi_qp_write(cec, irqs, CEC_INT_MASK_N);
	}
	return 0;
}

static int rockchip_hdmi_qp_cec_input_init(struct dw_hdmi_qp_cec *cec)
{
	int err;

	cec->devinput = devm_input_allocate_device(cec->dev);
	if (!cec->devinput)
		return -ENOMEM;

	cec->devinput->name = "hdmi_cec_key";
	cec->devinput->phys = "hdmi_cec_key/input0";
	cec->devinput->id.bustype = BUS_HOST;
	cec->devinput->id.vendor = 0x0001;
	cec->devinput->id.product = 0x0001;
	cec->devinput->id.version = 0x0100;
	input_set_capability(cec->devinput, EV_KEY, KEY_POWER);

	err = input_register_device(cec->devinput);
	if (err < 0)
		return err;

	return 0;
}

static long dw_hdmi_qp_cec_standby(struct cec_adapter *adap, __u8 __user *parg)
{
	struct dw_hdmi_qp_cec *cec = cec_get_drvdata(adap);
	__u8 en;
	int ret;

	if (copy_from_user(&en, parg, sizeof(en)))
		return -EFAULT;

	mutex_lock(&cec->wake_lock);
	cec->standby_en = !en;
	dev_dbg(cec->dev,
		 "OPI5U CEC: CEC_STANDBY userspace_en=%u -> standby_en=%d wake_en=%d\n",
		 en, cec->standby_en, cec->wake_en);
	ret = adap->ops->adap_enable(adap, en);
	mutex_unlock(&cec->wake_lock);

	return ret;
}

static long dw_hdmi_qp_cec_func_en(struct dw_hdmi_qp_cec *cec, int __user *parg)
{
	int en_mask;

	if (copy_from_user(&en_mask, parg, sizeof(en_mask)))
		return -EFAULT;

	mutex_lock(&cec->wake_lock);
	cec->system_cec_enabled = !!(en_mask & CEC_EN);
	cec->wake_en = cec->system_cec_enabled && (en_mask & CEC_WAKE);
	if (!cec->system_cec_enabled)
		cec->standby_en = false;
	dev_info(cec->dev,
		 "OPI5U CEC: CEC_FUNC_EN mask=0x%x -> system_cec_enabled=%d wake_en=%d standby_en=%d\n",
		 en_mask, cec->system_cec_enabled, cec->wake_en, cec->standby_en);
	mutex_unlock(&cec->wake_lock);

	return 0;
}

static long dw_hdmi_qp_cec_ioctl(struct file *f, unsigned int cmd,
					 unsigned long arg)
{
	struct dw_hdmi_qp_cec *cec;
	struct miscdevice *misc_dev;
	void __user *data;

	if (!f)
		return -EFAULT;

	misc_dev = f->private_data;
	cec = container_of(misc_dev, struct dw_hdmi_qp_cec, misc_dev);
	data = (void __user *)arg;

	switch (cmd) {
	case CEC_STANDBY:
		return dw_hdmi_qp_cec_standby(cec->adap, data);
	case CEC_FUNC_EN:
		return dw_hdmi_qp_cec_func_en(cec, data);
	default:
		return -EINVAL;
	}
}

static int dw_hdmi_qp_cec_open(struct inode *inode, struct file *f)
{
	return 0;
}

static int dw_hdmi_qp_cec_release(struct inode *inode, struct file *f)
{
	return 0;
}

static const struct file_operations dw_hdmi_qp_cec_file_operations = {
	.compat_ioctl = dw_hdmi_qp_cec_ioctl,
	.unlocked_ioctl = dw_hdmi_qp_cec_ioctl,
	.open = dw_hdmi_qp_cec_open,
	.release = dw_hdmi_qp_cec_release,
	.owner = THIS_MODULE,
};

static const struct cec_adap_ops dw_hdmi_qp_cec_ops = {
	.adap_enable = dw_hdmi_qp_cec_enable,
	.adap_log_addr = dw_hdmi_qp_cec_log_addr,
	.adap_transmit = dw_hdmi_qp_cec_transmit,
};

static void dw_hdmi_qp_cec_del(void *data)
{
	struct dw_hdmi_qp_cec *cec = data;

	cec_delete_adapter(cec->adap);
}

static int dw_hdmi_qp_cec_probe(struct platform_device *pdev)
{
	struct dw_hdmi_qp_cec_data *data = dev_get_platdata(&pdev->dev);
	struct dw_hdmi_qp_cec *cec;
	int ret;

	if (!data) {
		dev_err(&pdev->dev, "can't get data\n");
		return -ENXIO;
	}

	/*
	 * Our device is just a convenience - we want to link to the real
	 * hardware device here, so that userspace can see the association
	 * between the HDMI hardware and its associated CEC chardev.
	 */
	cec = devm_kzalloc(&pdev->dev, sizeof(*cec), GFP_KERNEL);
	if (!cec)
		return -ENOMEM;

	cec->dev = &pdev->dev;
	cec->ops = data->ops;
	cec->hdmi = data->hdmi;
	cec->irq = data->irq;
	cec->system_cec_enabled = true;
	mutex_init(&cec->wake_lock);

	platform_set_drvdata(pdev, cec);

	dw_hdmi_qp_write(cec, 0, CEC_TX_CNT);
	dw_hdmi_qp_write(cec, ~0, CEC_INT_CLEAR);
	dw_hdmi_qp_write(cec, 0, CEC_INT_MASK_N);

	cec->adap = cec_allocate_adapter(&dw_hdmi_qp_cec_ops, cec, "dw_hdmi_qp",
					 CEC_CAP_LOG_ADDRS | CEC_CAP_TRANSMIT |
					 CEC_CAP_RC | CEC_CAP_PASSTHROUGH,
					 CEC_MAX_LOG_ADDRS);
	if (IS_ERR(cec->adap)) {
		dev_err(&pdev->dev, "cec allocate adapter failed\n");
		return PTR_ERR(cec->adap);
	}

	dw_hdmi_qp_set_cec_adap(cec->hdmi, cec->adap);

	/* override the module pointer */
	cec->adap->owner = THIS_MODULE;

	ret = devm_add_action(&pdev->dev, dw_hdmi_qp_cec_del, cec);
	if (ret) {
		dev_err(&pdev->dev, "cec add action failed\n");
		cec_delete_adapter(cec->adap);
		return ret;
	}

	if (cec->irq < 0) {
		ret = cec->irq;
		dev_err(&pdev->dev, "cec get irq failed\n");
		return ret;
	}

	ret = devm_request_threaded_irq(&pdev->dev, cec->irq,
					dw_hdmi_qp_cec_hardirq,
					dw_hdmi_qp_cec_thread, IRQF_SHARED,
					"dw-hdmi-qp-cec", cec->adap);
	if (ret < 0) {
		dev_err(&pdev->dev, "cec request irq thread failed\n");
		return ret;
	}

	cec->notify = cec_notifier_cec_adap_register(pdev->dev.parent,
						     NULL, cec->adap);
	if (!cec->notify) {
		dev_err(&pdev->dev, "cec notifier adap register failed\n");
		return -ENOMEM;
	}

	ret = cec_register_adapter(cec->adap, pdev->dev.parent);
	if (ret < 0) {
		dev_err(&pdev->dev, "cec adap register failed\n");
		cec_notifier_cec_adap_unregister(cec->notify, cec->adap);
		return ret;
	}

	/*
	 * CEC documentation says we must not call cec_delete_adapter
	 * after a successful call to cec_register_adapter().
	 */
	devm_remove_action(&pdev->dev, dw_hdmi_qp_cec_del, cec);

	ret = rockchip_hdmi_qp_cec_input_init(cec);
	if (ret)
		dev_warn(&pdev->dev, "OPI5U CEC: input init failed (%d)\n", ret);

	cec->misc_dev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "rk_cec");
	if (!cec->misc_dev.name)
		return -ENOMEM;
	cec->misc_dev.minor = MISC_DYNAMIC_MINOR;
	cec->misc_dev.fops = &dw_hdmi_qp_cec_file_operations;
	cec->misc_dev.mode = 0666;

	ret = misc_register(&cec->misc_dev);
	if (ret)
		dev_err(&pdev->dev, "OPI5U CEC: misc_register rk_cec failed (%d)\n", ret);
	else
		dev_info(&pdev->dev, "OPI5U CEC: registered /dev/rk_cec for QP CEC\n");

	return ret;
}

static int dw_hdmi_qp_cec_remove(struct platform_device *pdev)
{
	struct dw_hdmi_qp_cec *cec = platform_get_drvdata(pdev);

	cec_notifier_cec_adap_unregister(cec->notify, cec->adap);
	cec_unregister_adapter(cec->adap);
	misc_deregister(&cec->misc_dev);

	return 0;
}

static struct platform_driver dw_hdmi_qp_cec_driver = {
	.probe	= dw_hdmi_qp_cec_probe,
	.remove	= dw_hdmi_qp_cec_remove,
	.driver = {
		.name = "dw-hdmi-qp-cec",
	},
};
module_platform_driver(dw_hdmi_qp_cec_driver);

MODULE_AUTHOR("Algea Cao <algea.cao@rock-chips.com>");
MODULE_DESCRIPTION("Synopsys Designware HDMI QP CEC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS(PLATFORM_MODULE_PREFIX "dw-hdmi-qp-cec");
