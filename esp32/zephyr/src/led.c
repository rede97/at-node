/*
 * AT-Node Zephyr — WS2812 status LED (free-color use).
 *
 * Tracked presets (led_status) vs free custom color (led_set_rgb):
 * custom color holds until the next led_status() call. WIFI_CONNECTING
 * blinks slowly via a 500 ms timer; other presets are static.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "led.h"

#define STRIP_NODE   DT_ALIAS(led_strip)
#define NUM_PIXELS   DT_PROP(STRIP_NODE, chain_length)
#define BRIGHTNESS   0x20

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[NUM_PIXELS];

static enum led_status cur = LED_BOOT;
static bool custom;
static bool blink_on;

static void led_apply(uint8_t r, uint8_t g, uint8_t b)
{
	memset(pixels, 0, sizeof(pixels));
	pixels[0].r = r;
	pixels[0].g = g;
	pixels[0].b = b;
	if (led_strip_update_rgb(strip, pixels, NUM_PIXELS) != 0) {
		printk("LED: strip update failed\n");
	}
}

static void led_apply_status(void)
{
	switch (cur) {
	case LED_BOOT:            led_apply(BRIGHTNESS, BRIGHTNESS, 0); break;
	case LED_WIFI_CONNECTING: led_apply(0, 0, blink_on ? BRIGHTNESS : 0); break;
	case LED_ONLINE:          led_apply(0, BRIGHTNESS, 0); break;
	case LED_ERROR:           led_apply(BRIGHTNESS, 0, 0); break;
	}
}

static void blink_work_fn(struct k_work *w);

K_WORK_DEFINE(blink_work, blink_work_fn);

static void blink_tick(struct k_timer *t)
{
	ARG_UNUSED(t);
	/* k_timer expiry runs in ISR context: led_strip_update_rgb() does an
	 * SPI transfer and must never run there (corrupts scheduler interrupt
	 * accounting -> later "blocking pend from ISR context" panic in an
	 * innocent thread). Defer to the system workqueue.
	 */
	k_work_submit(&blink_work);
}

static void blink_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!custom && cur == LED_WIFI_CONNECTING) {
		blink_on = !blink_on;
		led_apply_status();
	}
}

K_TIMER_DEFINE(blink_timer, blink_tick, NULL);

void led_init(void)
{
	if (!device_is_ready(strip)) {
		printk("LED: strip not ready\n");
		return;
	}
	k_timer_start(&blink_timer, K_MSEC(500), K_MSEC(500));
}

void led_status(enum led_status s)
{
	cur = s;
	custom = false;
	led_apply_status();
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	custom = true;
	led_apply(r, g, b);
}

void led_off(void)
{
	custom = true;
	led_apply(0, 0, 0);
}

bool led_is_custom(void)
{
	return custom;
}
