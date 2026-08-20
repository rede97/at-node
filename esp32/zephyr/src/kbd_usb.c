/*
 * AT-Node Zephyr - USB HID keyboard backend (CH582 USB HID equivalent).
 *
 * Zephyr USB device_next stack (usbd) on the ESP32-S3 DWC2 OTG controller
 * with the internal full-speed PHY (D+ GPIO20 / D- GPIO19, fixed in
 * esp32s3_common.dtsi). Ported from zephyr/samples/subsys/usb/hid-keyboard
 * with the button/INPUT part removed; reports are pushed by kbd_send_report()
 * through the routing layer instead.
 *
 * Failure policy: the USB OTG port may be unwired/unpowered, so every error
 * path only logs a warning and returns an error code - never fatal.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/usb_buf.h>

#include "kbd.h"

#if DT_NODE_HAS_STATUS(DT_NODELABEL(zephyr_udc0), okay)

/* VID/PID identical to the upstream hid-keyboard sample (Zephyr project VID,
 * development use only - replace with a registered VID/PID for production).
 */
#define KBD_USB_VID       0x2FE3
#define KBD_USB_PID       0x0007
/* Bus-powered, no remote wakeup, 100 mA budget. */
#define KBD_USB_MAX_POWER 100

/* Boot keyboard input report: modifier byte, reserved byte, 6 keycodes. */
#define KBD_REPORT_LEN 8

static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

#define HID_DEV DEVICE_DT_GET_ONE(zephyr_hid_device)

/* ------------------------------------------------------------------ */
/* USB device context, string descriptors, FS configuration            */
/* ------------------------------------------------------------------ */

USBD_DEVICE_DEFINE(kbd_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   KBD_USB_VID, KBD_USB_PID);

USBD_DESC_LANG_DEFINE(kbd_lang);
USBD_DESC_MANUFACTURER_DEFINE(kbd_mfr, "AT-Node");
USBD_DESC_PRODUCT_DEFINE(kbd_product, "AT-Node Keyboard");

USBD_DESC_CONFIG_DEFINE(kbd_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(kbd_fs_config, 0, KBD_USB_MAX_POWER,
			  &kbd_fs_cfg_desc);

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static bool kb_ready;        /* set by iface_ready() callback            */
static bool kb_ctx_inited;   /* usbd_init() completed                    */
static bool kb_enabled;      /* usbd_enable() completed                  */
static uint32_t kb_idle_ms;  /* idle duration, stored but not acted upon */

static K_MUTEX_DEFINE(kb_report_lock);
UDC_STATIC_BUF_DEFINE(kb_report, KBD_REPORT_LEN);

/* ------------------------------------------------------------------ */
/* HID device callbacks (invoked from the USBD thread)                 */
/* ------------------------------------------------------------------ */

static void kb_iface_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	kb_ready = ready;
	printk("KBDUSB: HID interface %s\n", ready ? "ready" : "not ready");
}

static int kb_get_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);
	printk("KBDUSB: get_report type %u id %u not implemented\n", type, id);

	return 0;
}

static int kb_verify_set_report(const struct device *dev, const uint8_t type,
				const uint8_t id, const uint16_t len)
{
	ARG_UNUSED(dev);

	if (type != HID_REPORT_TYPE_OUTPUT) {
		printk("KBDUSB: unsupported report type %u\n", type);
		return -ENOTSUP;
	}

	if (id != 0) {
		printk("KBDUSB: unsupported report id %u\n", id);
		return -ENOTSUP;
	}

	if (len != 1) {
		printk("KBDUSB: unsupported report length %u\n", len);
		return -ENOTSUP;
	}

	return 0;
}

static int kb_set_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);

	if (type != HID_REPORT_TYPE_OUTPUT) {
		printk("KBDUSB: unsupported report type %u\n", type);
		return -ENOTSUP;
	}

	if (len < 1) {
		return -EINVAL;
	}

	/* Keyboard LED output report: bit0 NumLock, bit1 CapsLock,
	 * bit2 ScrollLock. No status LEDs wired to the host state, log only.
	 */
	printk("KBDUSB: LED report 0x%02x (NumLock %u CapsLock %u ScrollLock %u)\n",
	       buf[0], !!(buf[0] & BIT(0)), !!(buf[0] & BIT(1)),
	       !!(buf[0] & BIT(2)));

	return 0;
}

static void kb_set_idle(const struct device *dev,
			const uint8_t id, const uint32_t duration)
{
	ARG_UNUSED(dev);
	printk("KBDUSB: set idle id %u duration %u\n", id, duration);
	kb_idle_ms = duration;
}

static uint32_t kb_get_idle(const struct device *dev, const uint8_t id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);

	return kb_idle_ms;
}

static void kb_set_protocol(const struct device *dev, const uint8_t proto)
{
	ARG_UNUSED(dev);
	printk("KBDUSB: protocol changed to %s\n",
	       proto == 0U ? "Boot" : "Report");
}

static void kb_output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	kb_set_report(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}

static const struct hid_device_ops kb_ops = {
	.iface_ready = kb_iface_ready,
	.get_report = kb_get_report,
	.verify_set_report = kb_verify_set_report,
	.set_report = kb_set_report,
	.set_idle = kb_set_idle,
	.get_idle = kb_get_idle,
	.set_protocol = kb_set_protocol,
	.output_report = kb_output_report,
};

/* ------------------------------------------------------------------ */
/* USBD message callback: VBUS hot-plug handling when the controller   */
/* supports VBUS detection (ESP32-S3 DWC2 does not - enable at start). */
/* ------------------------------------------------------------------ */

static void kb_msg_cb(struct usbd_context *const usbd_ctx,
		      const struct usbd_msg *const msg)
{
	printk("KBDUSB: USBD message: %s\n", usbd_msg_type_string(msg->type));

	if (msg->type == USBD_MSG_CONFIGURATION) {
		printk("KBDUSB: configuration value %d\n", msg->status);
	}

	if (usbd_can_detect_vbus(usbd_ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(usbd_ctx) == 0) {
				kb_enabled = true;
			} else {
				printk("KBDUSB: failed to enable device\n");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(usbd_ctx) == 0) {
				kb_enabled = false;
			} else {
				printk("KBDUSB: failed to disable device\n");
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* One-time stack setup (descriptors, configuration, class instances)  */
/* ------------------------------------------------------------------ */

static int kb_usbd_setup(void)
{
	int err;

	err = usbd_add_descriptor(&kbd_usbd, &kbd_lang);
	if (err) {
		printk("KBDUSB: add language descriptor failed %d\n", err);
		return err;
	}

	err = usbd_add_descriptor(&kbd_usbd, &kbd_mfr);
	if (err) {
		printk("KBDUSB: add manufacturer descriptor failed %d\n", err);
		return err;
	}

	err = usbd_add_descriptor(&kbd_usbd, &kbd_product);
	if (err) {
		printk("KBDUSB: add product descriptor failed %d\n", err);
		return err;
	}

	/* ESP32-S3 DWC2 is full-speed only, no HS configuration needed. */
	err = usbd_add_configuration(&kbd_usbd, USBD_SPEED_FS, &kbd_fs_config);
	if (err) {
		printk("KBDUSB: add FS configuration failed %d\n", err);
		return err;
	}

	err = usbd_register_all_classes(&kbd_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		printk("KBDUSB: register classes failed %d\n", err);
		return err;
	}

	/* HID carries class information at the interface level, so the
	 * device class triple stays 0/0/0.
	 */
	err = usbd_device_set_code_triple(&kbd_usbd, USBD_SPEED_FS, 0, 0, 0);
	if (err) {
		printk("KBDUSB: set code triple failed %d\n", err);
		return err;
	}

	usbd_self_powered(&kbd_usbd, false);

	err = usbd_msg_register_cb(&kbd_usbd, kb_msg_cb);
	if (err) {
		printk("KBDUSB: register message callback failed %d\n", err);
		return err;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Backend interface (see kbd.h)                                       */
/* ------------------------------------------------------------------ */

int kbd_usb_start(void)
{
	int ret;

	if (!kb_ctx_inited) {
		if (!device_is_ready(HID_DEV)) {
			printk("KBDUSB: HID device not ready\n");
			return -ENODEV;
		}

		ret = hid_device_register(HID_DEV, hid_report_desc,
					  sizeof(hid_report_desc), &kb_ops);
		if (ret != 0) {
			printk("KBDUSB: hid_device_register failed %d\n", ret);
			return ret;
		}

		ret = kb_usbd_setup();
		if (ret != 0) {
			return ret;
		}

		ret = usbd_init(&kbd_usbd);
		if (ret != 0) {
			printk("KBDUSB: usbd_init failed %d\n", ret);
			return ret;
		}

		kb_ctx_inited = true;
	}

	if (kb_enabled) {
		return 0;
	}

	if (usbd_can_detect_vbus(&kbd_usbd)) {
		/* Enable happens in kb_msg_cb() once VBUS is detected. */
		printk("KBDUSB: waiting for VBUS\n");
		return 0;
	}

	ret = usbd_enable(&kbd_usbd);
	if (ret != 0) {
		printk("KBDUSB: usbd_enable failed %d "
		       "(OTG port unwired/unpowered?)\n", ret);
		return ret;
	}

	kb_enabled = true;
	printk("KBDUSB: USB device enabled\n");

	return 0;
}

void kbd_usb_stop(void)
{
	int ret;

	if (!kb_ctx_inited || !kb_enabled) {
		return;
	}

	ret = usbd_disable(&kbd_usbd);
	if (ret != 0) {
		printk("KBDUSB: usbd_disable failed %d\n", ret);
		return;
	}

	kb_enabled = false;
	kb_ready = false;
	printk("KBDUSB: USB device disabled\n");
}

bool kbd_usb_ready(void)
{
	return kb_ready;
}

int kbd_usb_send(uint8_t mods, const uint8_t keys[6])
{
	int ret;

	if (!kb_ready) {
		return -ENOTCONN;
	}

	/* Without an input_report_done callback, hid_device_submit_report()
	 * blocks until the IN transfer completes (same as the sample), so
	 * serialize access to the shared report buffer.
	 */
	k_mutex_lock(&kb_report_lock, K_FOREVER);

	kb_report[0] = mods;
	kb_report[1] = 0;
	memcpy(&kb_report[2], keys, 6);

	ret = hid_device_submit_report(HID_DEV, KBD_REPORT_LEN, kb_report);

	k_mutex_unlock(&kb_report_lock);

	if (ret != 0) {
		printk("KBDUSB: submit report failed %d\n", ret);
	}

	return ret;
}

#else /* zephyr_udc0 disabled (e.g. JTAG debug build): stubs */

int kbd_usb_start(void)
{
	printk("KBDUSB: usb_otg disabled in DT, backend inactive\n");
	return -ENODEV;
}

void kbd_usb_stop(void)
{
}

bool kbd_usb_ready(void)
{
	return false;
}

int kbd_usb_send(uint8_t mods, const uint8_t keys[6])
{
	ARG_UNUSED(mods);
	ARG_UNUSED(keys);
	return -ENOTCONN;
}

#endif /* DT_NODE_HAS_STATUS(zephyr_udc0) */
