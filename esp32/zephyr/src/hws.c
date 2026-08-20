/*
 * AT-Node Zephyr — hardware services (GPIO / ADC / I2C).
 *
 * GPIO: gpio0 (pins 0..31) / gpio1 (pins 32..48) via DEVICE_DT_GET.
 * Rejects S3 strapping / USB / flash / PSRAM / console pins.
 *
 * ADC: adc0 (unit 1), channels 0..9 = GPIO1..GPIO10 (app.overlay).
 * Reference ADC_REF_INTERNAL (driver reports 1100 mV), gain ADC_GAIN_1_4
 * (driver maps it to ADC_ATTEN_DB_12, same full-scale range as Arduino
 * analogReadMilliVolts default attenuation).
 *
 * I2C: i2c0 on GPIO8/SDA + GPIO9/SCL (app.overlay). Scan uses zero-length
 * write probes (START, ADDR+W, STOP): the esp32 i2c driver checks the ACK
 * of the address byte and returns -EIO on NACK, and its master_write()
 * handles len 0 without touching the buffer (verified in
 * drivers/i2c/i2c_esp32.c: i2c_esp32_write_msg/write_addr/master_write).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "hws.h"

#define HWS_ADC_RESOLUTION 12
#define HWS_ADC_MAX_CH     9
#define HWS_I2C_SCAN_FIRST 0x08
#define HWS_I2C_SCAN_LAST  0x77

static const struct device *const gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static K_MUTEX_DEFINE(hws_lock);
static uint32_t adc_ch_configured; /* bitmask of adc_channel_setup() done */

/* Pins that must never be touched by user AT commands:
 *  0, 3, 45, 46  strapping
 *  19, 20        native USB D-/D+
 *  26..32        SPI flash / PSRAM
 *  33..37        octal PSRAM
 *  43, 44        UART0 console (ESPLink)
 * Also rejects nonexistent pins (> 48).
 */
static bool hws_pin_forbidden(uint8_t pin)
{
	if (pin > 48) {
		return true;
	}
	if (pin >= 26 && pin <= 37) {
		return true;
	}
	switch (pin) {
	case 0:
	case 3:
	case 19:
	case 20:
	case 43:
	case 44:
	case 45:
	case 46:
		return true;
	default:
		return false;
	}
}

static const struct device *hws_gpio_dev(uint8_t pin, gpio_pin_t *port_pin)
{
	if (pin < 32) {
		*port_pin = pin;
		return gpio0_dev;
	}
	*port_pin = pin - 32;
	return gpio1_dev;
}

int hws_init(void)
{
	if (!device_is_ready(gpio0_dev) || !device_is_ready(gpio1_dev)) {
		printk("HWS: gpio not ready\n");
		return -ENODEV;
	}
	if (!device_is_ready(adc_dev)) {
		printk("HWS: adc0 not ready\n");
		return -ENODEV;
	}
	if (!device_is_ready(i2c_dev)) {
		printk("HWS: i2c0 not ready\n");
		return -ENODEV;
	}
	printk("HWS: gpio/adc/i2c ready\n");
	return 0;
}

int hws_gpio_write(uint8_t pin, int level)
{
	gpio_pin_t ppin;
	const struct device *dev;
	int rc;

	if (hws_pin_forbidden(pin)) {
		return -EINVAL;
	}
	dev = hws_gpio_dev(pin, &ppin);

	k_mutex_lock(&hws_lock, K_FOREVER);
	rc = gpio_pin_configure(dev, ppin,
				level ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);
	k_mutex_unlock(&hws_lock);
	return rc;
}

int hws_gpio_read(uint8_t pin, int *level)
{
	gpio_pin_t ppin;
	const struct device *dev;
	int rc;

	if (level == NULL) {
		return -EINVAL;
	}
	if (hws_pin_forbidden(pin)) {
		return -EINVAL;
	}
	dev = hws_gpio_dev(pin, &ppin);

	k_mutex_lock(&hws_lock, K_FOREVER);
	/* Matches the Arduino variant: read as input with pull-up. */
	rc = gpio_pin_configure(dev, ppin, GPIO_INPUT | GPIO_PULL_UP);
	if (rc == 0) {
		*level = gpio_pin_get(dev, ppin);
		if (*level < 0) {
			rc = *level;
		}
	}
	k_mutex_unlock(&hws_lock);
	return rc;
}

int hws_adc_read_mv(uint8_t ch, int *mv)
{
	uint16_t raw = 0;
	int32_t val_mv;
	int rc;

	if (mv == NULL || ch > HWS_ADC_MAX_CH) {
		return -EINVAL;
	}

	struct adc_sequence seq = {
		.channels = BIT(ch),
		.buffer = &raw,
		.buffer_size = sizeof(raw),
		.resolution = HWS_ADC_RESOLUTION,
	};

	k_mutex_lock(&hws_lock, K_FOREVER);

	if ((adc_ch_configured & BIT(ch)) == 0) {
		struct adc_channel_cfg cfg = {
			.gain = ADC_GAIN_1_4,
			.reference = ADC_REF_INTERNAL,
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,
			.channel_id = ch,
		};

		rc = adc_channel_setup(adc_dev, &cfg);
		if (rc != 0) {
			k_mutex_unlock(&hws_lock);
			printk("HWS: adc ch%u setup failed (%d)\n", ch, rc);
			return rc;
		}
		adc_ch_configured |= BIT(ch);
	}

	rc = adc_read(adc_dev, &seq);
	if (rc == 0) {
		val_mv = (int32_t)raw;
		rc = adc_raw_to_millivolts(adc_ref_internal(adc_dev),
					   ADC_GAIN_1_4, HWS_ADC_RESOLUTION,
					   &val_mv);
		if (rc == 0) {
			*mv = (int)val_mv;
		}
	}
	k_mutex_unlock(&hws_lock);
	return rc;
}

int hws_i2c_scan(char *buf, size_t len)
{
	bool found = false;
	size_t off;

	if (buf == NULL || len == 0) {
		return -EINVAL;
	}

	off = snprintk(buf, len, "+I2C:");

	k_mutex_lock(&hws_lock, K_FOREVER);
	for (uint8_t addr = HWS_I2C_SCAN_FIRST; addr <= HWS_I2C_SCAN_LAST;
	     addr++) {
		/* Zero-length write probe: the esp32 driver ACK-checks the
		 * address byte (ack_en) and returns -EIO on NACK; len 0
		 * never touches the buffer. */
		int rc = i2c_write(i2c_dev, NULL, 0, addr);

		if (rc == 0 && off < len) {
			off += snprintk(buf + off, len - off, " 0x%02X", addr);
			found = true;
		}
	}
	k_mutex_unlock(&hws_lock);

	if (!found && off < len) {
		snprintk(buf + off, len - off, " none");
	}
	return 0;
}

int hws_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
	int rc;

	if (data == NULL || len == 0 || addr > 0x7F) {
		return -EINVAL;
	}
	k_mutex_lock(&hws_lock, K_FOREVER);
	rc = i2c_burst_read(i2c_dev, addr, reg, data, len);
	k_mutex_unlock(&hws_lock);
	return rc;
}

int hws_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
	int rc;

	if ((data == NULL && len > 0) || addr > 0x7F) {
		return -EINVAL;
	}
	k_mutex_lock(&hws_lock, K_FOREVER);
	rc = i2c_burst_write(i2c_dev, addr, reg, data, len);
	k_mutex_unlock(&hws_lock);
	return rc;
}
