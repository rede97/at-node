/*
 * AT-Node Zephyr — WS2812 status LED (free-color use, user requirement).
 *
 * Status presets drive the board LED automatically; led_set_rgb() switches
 * to LED_CUSTOM until led_status() is called again (or led_auto() restores
 * the current tracked state).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum led_status {
	LED_BOOT,            /* yellow */
	LED_WIFI_CONNECTING, /* slow-blink blue */
	LED_ONLINE,          /* green */
	LED_ERROR,           /* red */
};

void led_init(void);
void led_status(enum led_status s);          /* switch to a tracked preset */
void led_set_rgb(uint8_t r, uint8_t g, uint8_t b); /* free custom color */
void led_off(void);
bool led_is_custom(void);
