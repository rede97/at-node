/*
 * AT-Node Zephyr — main: 7-stage init mirroring the CH582 layer stack.
 *
 *  1. led_init            WS2812 status LED (yellow = booting)
 *  2. cfg_init            settings/NVS config registry
 *  3. kbd_init            keyboard routing + tap/text sequence thread
 *  4. wifi_sta_init       STA + 15 s reconnect watchdog
 *  5. kbd_ble_start       BLE HID peripheral (if ble.auto)
 *  6. httpd_start         /at-node control plane (if http.auto)
 *  7. mqttc_start         MQTT/TLS remote plane (if mqtt.auto)
 *  8. at_serial_init      UART0 AT console
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "at_serial.h"
#include "cfg.h"
#include "httpd.h"
#include "hws.h"
#include "kbd.h"
#include "led.h"
#include "mqttc.h"
#include "wifi_sta.h"

#define DEFAULT_NAME_PREFIX "AT-Node-S3"

void node_cfg_changed(const char *key)
{
	if (strncmp(key, "wifi.", 5) == 0) {
		wifi_sta_reconnect();
	} else if (strncmp(key, "mqtt.", 5) == 0) {
		bool want = cfg_get_bool("mqtt.enable", false);

		if (want && !mqttc_running()) {
			mqttc_start();
		} else if (!want && mqttc_running()) {
			mqttc_stop();
		}
	} else if (strcmp(key, "http.enable") == 0) {
		bool want = cfg_get_bool("http.enable", true);

		if (want && !httpd_running()) {
			httpd_start();
		} else if (!want && httpd_running()) {
			httpd_stop();
		}
	} else if (strcmp(key, "ble.enable") == 0) {
		bool want = cfg_get_bool("ble.enable", true);

		if (want) {
			kbd_ble_start();
		} else {
			kbd_ble_stop();
		}
	}
	/* device.name: applied on next boot for BLE/MQTT identity */
}

int main(void)
{
	printk("AT-Node Zephyr (ESP32-S3) booting\n");

	led_init();
	led_status(LED_BOOT);

	if (cfg_init() != 0) {
		printk("CFG: settings init failed\n");
		led_status(LED_ERROR);
	}

	kbd_init();
	hws_init();
	wifi_sta_init();

	/* USB HID keyboard: always on (independent of BLE target) */
	kbd_usb_start();

	if (cfg_get_bool("ble.auto", true) && !IS_ENABLED(CONFIG_ATNODE_TRIAGE_NO_BT)) {
		kbd_ble_start();
	}
	if (cfg_get_bool("http.auto", true)) {
		httpd_start();
	}
	if (cfg_get_bool("mqtt.auto", false) && cfg_get_bool("mqtt.enable", true)) {
		mqttc_start();
	}

	at_serial_init();

	printk("AT-Node ready (type AT+HELP)\n");
	return 0;
}
