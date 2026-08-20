/*
 * AT-Node Zephyr — transport-agnostic AT command core.
 *
 * Semantics aligned with (priority order):
 *   1. CH582 variant      wchble/mr2/USER-MANUAL.md
 *   2. Arduino variant    esp32/arduino/arduino.ino (serial_exec handler)
 *   3. esp32/arduino/README.md / API.md / PLAN.md
 *
 * One dispatcher shared by serial UART, HTTP /at-node/at and the MQTT cmd
 * topic. Responses go out line-by-line through emit() without CR/LF; each
 * command ends with exactly one "OK" or "ERROR <reason>" line.
 *
 * Thread safety: a single mutex serializes whole commands, so the static
 * scratch buffers below are safe and emit() callbacks never interleave.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "at_core.h"
#include "cfg.h"
#include "hws.h"
#include "httpd.h"
#include "kbd.h"
#include "led.h"
#include "mqttc.h"
#include "wifi_sta.h"

#define AT_LINE_MAX  512 /* dispatcher input copy limit */
#define KEY_STR_MAX  256 /* kbd_job.text payload limit is 257 incl NUL */
#define I2C_IO_MAX   32  /* AT+I2C_R/AT+I2C_W data byte cap */
#define KEY_SEQ_MAX  128 /* Arduino caps AT+KEY_SEQ at 128 values */

static K_MUTEX_DEFINE(at_lock);

struct out {
	at_emit_fn emit;
	void *ctx;
};

/* Scratch (guarded by at_lock) */
static char     s_args[AT_LINE_MAX];
static char    *s_fields[KEY_SEQ_MAX + 3];
static uint32_t s_vals[KEY_SEQ_MAX];
static char     s_status[] = "status"; /* mutable default arg (split-safe) */

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */

static void out_line(struct out *o, const char *line)
{
	o->emit(line, o->ctx);
}

static void outf(struct out *o, const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintk(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = '\0';
	out_line(o, buf);
}

/* ------------------------------------------------------------------ */
/* Parsing helpers                                                     */
/* ------------------------------------------------------------------ */

/* Split s on ',' in place into at most max fields; the last field keeps
 * any remaining commas. Empty fields stay empty (parse as 0), mirroring
 * the Arduino String/indexOf parsing. Returns the field count.
 */
static int split(char *s, char *fields[], int max)
{
	int n = 0;

	fields[n++] = s;
	while (n < max) {
		char *c = strchr(fields[n - 1], ',');

		if (c == NULL) {
			break;
		}
		*c = '\0';
		fields[n++] = c + 1;
	}
	return n;
}

static uint32_t to_u32(const char *s)
{
	return (uint32_t)strtoul(s, NULL, 0); /* base 0: accepts 0x.. like Arduino */
}

/* CH582 KEY_STR escapes (USER-MANUAL §1.1): \n=Enter \t=Tab \\=backslash.
 * A raw command line cannot contain a real LF, so unescape here once for
 * every transport. Unknown escapes keep the char after the backslash.
 * Returns length written (<= cap-1), NUL-terminated.
 */
static size_t unescape_text(const char *in, char *outp, size_t cap)
{
	size_t n = 0;

	while (*in != '\0' && n < cap - 1) {
		if (*in == '\\' && in[1] != '\0') {
			in++;
			switch (*in) {
			case 'n':  outp[n++] = '\n'; break;
			case 't':  outp[n++] = '\t'; break;
			case '\\': outp[n++] = '\\'; break;
			default:   outp[n++] = *in;  break;
			}
			in++;
		} else {
			outp[n++] = *in++;
		}
	}
	outp[n] = '\0';
	return n;
}

/* ------------------------------------------------------------------ */
/* Command handlers (line is the mutable s_args buffer)                */
/* ------------------------------------------------------------------ */

static void cmd_help(struct out *o)
{
	static const char *const lines[] = {
		"AT-Node Zephyr commands:",
		"  AT / AT+STATUS / AT+VER / AT+HELP / AT+ABILITY",
		"  AT+TAP=<ms>,<mods>,<key>     press+release (ms 1..10000)",
		"  AT+KEY=<mods>,<k1>..<k6>    raw HID report (use AT+KEY=0,0 to release)",
		"  AT+KEY_STR=<text>           type text (escapes \\n \\t \\\\)",
		"  AT+KEY_SEQ=<ms>,<mods>,<k1..6>,... batch HID reports",
		"  AT+DEV[=USB|BLE|ALL]        kbd output target query/set",
		"  AT+SET=<key>=<val> / AT+GET=<key> / AT+KEYS  config",
		"  AT+GPIO_W=<pin>,<level> / AT+GPIO_R=<pin>",
		"  AT+ADC=<ch> / AT+I2C_SCAN / AT+I2C_R=<addr>,<reg>,<len>",
		"  AT+I2C_W=<addr>,<reg>,<d0>,<d1>,...",
		"  AT+WIFI=ssid|pass,<val> / AT+WIFI=status",
		"  AT+MQTT=connect|disconnect|status|enable,0|1|auto,0|1",
		"  AT+HTTP=status|enable,0|1|auto,0|1",
		"  AT+PAIR=1 open 60s window / AT+PAIR=0|status / AT+UNPAIR",
		"  AT+LED=<r>,<g>,<b>|off|auto",
		"  AT+NVS=clear / AT+RST",
	};

	for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
		out_line(o, lines[i]);
	}
	out_line(o, "OK");
}

static void cmd_status(struct out *o)
{
	char name[CFG_VAL_MAX];
	char ip[16];
	char mac[18];
	const char *ble;
	const char *mqtt;

	cfg_get_str("device.name", name, sizeof(name), "AT-Node-S3");
	wifi_sta_ip_str(ip, sizeof(ip));
	wifi_sta_mac_str(mac, sizeof(mac));

	if (kbd_ble_connected()) {
		ble = "connected";
	} else if (kbd_ble_pair_window_active()) {
		ble = "pairing";
	} else {
		ble = "disconnected";
	}
	if (!mqttc_running()) {
		mqtt = "off";
	} else if (mqttc_connected()) {
		mqtt = "connected";
	} else {
		mqtt = "connecting";
	}

	outf(o, "device=%s", name);
	outf(o, "ble=%s", ble);
	outf(o, "usb=%s", kbd_usb_ready() ? "ready" : "off");
	outf(o, "wifi=%s", wifi_sta_is_up() ? "up" : "down");
	outf(o, "ip=%s", ip);
	outf(o, "mac=%s", mac);
	outf(o, "rssi=%d", wifi_sta_rssi());
	outf(o, "mqtt=%s", mqtt);
	outf(o, "http=%s", httpd_running() ? "on" : "off");
	out_line(o, "OK");
}

/* AT+SET=<key>=<val> — unified config layer (arduino.ino:2718) */
static void cmd_set(struct out *o, char *args)
{
	char *eq = strchr(args, '=');
	int rc;

	if (eq == NULL || eq == args) {
		out_line(o, "ERROR bad args");
		return;
	}
	*eq = '\0';
	rc = cfg_set(args, eq + 1);
	if (rc == 0) {
		node_cfg_changed(args);
		out_line(o, "OK");
	} else if (rc == -ENOENT) {
		out_line(o, "ERROR unknown key");
	} else {
		out_line(o, "ERROR bad value");
	}
}

/* AT+GET=<key> (arduino.ino:2727) */
static void cmd_get(struct out *o, const char *key)
{
	char val[CFG_VAL_MAX];
	int rc = cfg_get(key, val, sizeof(val));

	if (rc == 0) {
		outf(o, "+GET:%s=%s", key, val);
		out_line(o, "OK");
	} else if (rc == -EACCES) {
		out_line(o, "ERROR write-only");
	} else if (rc == -ENOENT) {
		out_line(o, "ERROR unknown key");
	} else {
		out_line(o, "ERROR bad value");
	}
}

static void keys_cb(const char *key, const char *val, bool write_only, void *ctx)
{
	struct out *o = ctx;

	if (write_only || val == NULL) {
		outf(o, "%s=***", key);
	} else {
		outf(o, "%s=%s", key, val);
	}
}

/* AT+TAP=<ms>,<mods>,<key> (arduino.ino:2613, CH582 §3) */
static void cmd_tap(struct out *o, char *args)
{
	int rc;

	if (split(args, s_fields, 3) != 3) {
		out_line(o, "ERROR bad args");
		return;
	}
	s_vals[0] = to_u32(s_fields[0]); /* ms: reject before uint16_t wrap */
	if (s_vals[0] == 0 || s_vals[0] > 10000) {
		out_line(o, "ERROR bad args");
		return;
	}
	rc = kbd_tap((uint16_t)s_vals[0], (uint8_t)to_u32(s_fields[1]),
		     (uint8_t)to_u32(s_fields[2]));
	if (rc == 0) {
		out_line(o, "OK");
	} else if (rc == -ENOSPC) {
		out_line(o, "ERROR busy");
	} else {
		out_line(o, "ERROR bad args"); /* ms==0, ms>10000 or key==0 */
	}
}

/* AT+KEY=<mods>,<k1>..<k6> — raw press; release with AT+KEY=0,0
 * (arduino.ino:2628, CH582 §3). Missing key fields pad as 0.
 */
static void cmd_key(struct out *o, char *args)
{
	uint8_t keys[6] = { 0 };
	uint8_t mods;
	int n = split(args, s_fields, 7);
	int rc;

	mods = (uint8_t)to_u32(s_fields[0]);
	for (int i = 0; i < 6 && i + 1 < n; i++) {
		keys[i] = (uint8_t)to_u32(s_fields[i + 1]);
	}
	rc = kbd_send_report(mods, keys);
	if (rc == 0) {
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR no host"); /* no enabled backend connected */
	}
}

/* AT+KEY_STR=<text> (CH582 §3; ms=40 gap=30 per PLAN.md §keyboard/text) */
static void cmd_key_str(struct out *o, const char *args)
{
	static char text[KEY_STR_MAX + 1];
	int rc;

	if (unescape_text(args, text, sizeof(text)) == 0) {
		out_line(o, "ERROR bad args");
		return;
	}
	rc = kbd_type_text(text, 40, 30);
	if (rc == 0) {
		out_line(o, "OK");
	} else if (rc == -ENOSPC) {
		out_line(o, "ERROR busy");
	} else {
		out_line(o, "ERROR bad args");
	}
}

/* AT+KEY_SEQ=<delay_ms>,<mods>,<k1>..<k6>,... groups of 7
 * (arduino.ino:2647): delay clamped 1..200, raw reports, no auto-release.
 */
static void cmd_key_seq(struct out *o, char *args)
{
	int n = split(args, s_fields, KEY_SEQ_MAX);
	int d, reports;

	for (int i = 0; i < n; i++) {
		s_vals[i] = to_u32(s_fields[i]);
	}
	if (n < 8 || ((n - 1) % 7) != 0) {
		out_line(o, "ERROR usage AT+KEY_SEQ=<ms>,<mods>,<k1..6>,...");
		return;
	}
	d = (int)s_vals[0];
	if (d < 1) {
		d = 1;
	}
	if (d > 200) {
		d = 200;
	}
	reports = (n - 1) / 7;
	for (int r = 0; r < reports; r++) {
		uint8_t keys[6];

		for (int i = 0; i < 6; i++) {
			keys[i] = (uint8_t)s_vals[1 + r * 7 + 1 + i];
		}
		kbd_send_report((uint8_t)s_vals[1 + r * 7], keys);
		k_sleep(K_MSEC(d));
	}
	outf(o, "%d reports sent", reports);
	out_line(o, "OK");
}

/* AT+DEV[=USB|BLE|ALL] (CH582 §3 AT+DEV; ALL from kbd.h KB_TGT_ALL) */
static void cmd_dev(struct out *o, const char *args)
{
	if (args == NULL || args[0] == '\0' || strcmp(args, "?") == 0) {
		uint8_t t = kbd_get_targets();

		outf(o, "+DEV:%s,usb=%s,ble=%s",
		     (t == KB_TGT_USB) ? "USB" : ((t == KB_TGT_BLE) ? "BLE" : "ALL"),
		     kbd_usb_ready() ? "ready" : "off",
		     kbd_ble_connected() ? "connected" : "disconnected");
		out_line(o, "OK");
	} else if (strcmp(args, "USB") == 0) {
		kbd_set_targets(KB_TGT_USB);
		out_line(o, "OK");
	} else if (strcmp(args, "BLE") == 0) {
		kbd_set_targets(KB_TGT_BLE);
		out_line(o, "OK");
	} else if (strcmp(args, "ALL") == 0) {
		kbd_set_targets(KB_TGT_ALL);
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR bad args");
	}
}

static void cmd_gpio_w(struct out *o, char *args)
{
	if (split(args, s_fields, 2) != 2) {
		out_line(o, "ERROR bad args");
		return;
	}
	if (hws_gpio_write((uint8_t)to_u32(s_fields[0]),
			   (int)to_u32(s_fields[1])) == 0) {
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR unsafe pin");
	}
}

static void cmd_gpio_r(struct out *o, char *args)
{
	int level;

	if (hws_gpio_read((uint8_t)to_u32(args), &level) == 0) {
		outf(o, "+GPIO_R:%d", level); /* arduino.ino:2772 format */
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR bad pin");
	}
}

static void cmd_adc(struct out *o, char *args)
{
	int mv;

	if (hws_adc_read_mv((uint8_t)to_u32(args), &mv) == 0) {
		outf(o, "+ADC:%d", mv); /* arduino.ino:2779 format (mV) */
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR bad channel");
	}
}

static void cmd_i2c_scan(struct out *o)
{
	static char buf[600]; /* "+I2C:" + up to 112 x " 0xXX" */

	if (hws_i2c_scan(buf, sizeof(buf)) == 0) {
		out_line(o, buf);
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR i2c");
	}
}

/* AT+I2C_R=<addr>,<reg>,<len> (arduino.ino:2797) */
static void cmd_i2c_r(struct out *o, char *args)
{
	uint8_t data[I2C_IO_MAX];
	uint32_t len;
	char *p;
	int left;

	if (split(args, s_fields, 3) != 3) {
		out_line(o, "ERROR bad args");
		return;
	}
	len = to_u32(s_fields[2]);
	if (len == 0 || len > I2C_IO_MAX) {
		out_line(o, "ERROR bad args");
		return;
	}
	if (hws_i2c_read((uint8_t)to_u32(s_fields[0]), (uint8_t)to_u32(s_fields[1]),
			 data, len) != 0) {
		out_line(o, "ERROR i2c");
		return;
	}
	/* "+I2C_R:0A 1F" — hex bytes after colon, space separated */
	p = s_args;
	left = sizeof(s_args);
	p += snprintk(p, left, "+I2C_R:");
	for (uint32_t i = 0; i < len; i++) {
		p += snprintk(p, left - (p - s_args), "%s%02X", (i != 0) ? " " : "",
			      data[i]);
	}
	out_line(o, s_args);
	out_line(o, "OK");
}

/* AT+I2C_W=<addr>,<reg>,<d0>,<d1>,... (task format; arduino.ino:2823) */
static void cmd_i2c_w(struct out *o, char *args)
{
	uint8_t data[I2C_IO_MAX];
	int n = split(args, s_fields, I2C_IO_MAX + 2);

	if (n < 3) {
		out_line(o, "ERROR bad args");
		return;
	}
	for (int i = 2; i < n; i++) {
		data[i - 2] = (uint8_t)to_u32(s_fields[i]);
	}
	if (hws_i2c_write((uint8_t)to_u32(s_fields[0]), (uint8_t)to_u32(s_fields[1]),
			  data, n - 2) == 0) {
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR i2c");
	}
}

/* AT+WIFI=ssid|pass,<val> alias over cfg (arduino.ino:2928); status */
static void cmd_wifi(struct out *o, char *args)
{
	char ssid[CFG_VAL_MAX];
	int n = split(args, s_fields, 2);

	if (n == 2 && strcmp(s_fields[0], "ssid") == 0) {
		if (cfg_set("wifi.ssid", s_fields[1]) == 0) {
			node_cfg_changed("wifi.ssid");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad value");
		}
	} else if (n == 2 && strcmp(s_fields[0], "pass") == 0) {
		if (cfg_set("wifi.pass", s_fields[1]) == 0) {
			node_cfg_changed("wifi.pass");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad value");
		}
	} else if (strcmp(s_fields[0], "status") == 0) {
		cfg_get_str("wifi.ssid", ssid, sizeof(ssid), "");
		outf(o, "+WIFI:%s", ssid); /* arduino.ino:2938 format */
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR bad args");
	}
}

/* AT+MQTT=connect|disconnect|status|enable,0|1|auto,0|1 (arduino.ino:2881) */
static void cmd_mqtt(struct out *o, char *args)
{
	int n = split(args, s_fields, 2);
	const char *sub = s_fields[0];
	const char *val = (n == 2) ? s_fields[1] : "";
	int rc;

	if (strcmp(sub, "connect") == 0) {
		rc = mqttc_start();
		if (rc == 0) {
			out_line(o, "OK");
		} else if (rc == -EINVAL) {
			out_line(o, "ERROR broker unset");
		} else {
			out_line(o, "ERROR start failed");
		}
	} else if (strcmp(sub, "disconnect") == 0) {
		mqttc_stop();
		out_line(o, "OK");
	} else if (strcmp(sub, "status") == 0) {
		outf(o, "+MQTT:%s,auto=%d,enabled=%d",
		     mqttc_connected() ? "connected" : "disconnected",
		     cfg_get_bool("mqtt.auto", false) ? 1 : 0,
		     cfg_get_bool("mqtt.enable", false) ? 1 : 0);
		out_line(o, "OK");
	} else if (n == 2 && strcmp(sub, "enable") == 0) {
		if (cfg_set("mqtt.enable", val) == 0) {
			node_cfg_changed("mqtt.enable");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad value");
		}
	} else if (n == 2 && strcmp(sub, "auto") == 0) {
		if (cfg_set("mqtt.auto", val) == 0) {
			node_cfg_changed("mqtt.auto");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad value");
		}
	} else {
		out_line(o, "ERROR bad args");
	}
}

/* AT+HTTP=status|enable,0|1|auto,0|1 (arduino.ino:2949) */
static void cmd_http(struct out *o, char *args)
{
	int n = split(args, s_fields, 2);
	const char *sub = s_fields[0];
	const char *val = (n == 2) ? s_fields[1] : "";

	if (strcmp(sub, "status") == 0) {
		outf(o, "+HTTP:%s,auto=%d", httpd_running() ? "enabled" : "disabled",
		     cfg_get_bool("http.auto", true) ? 1 : 0);
		out_line(o, "OK");
	} else if (n == 2 && strcmp(sub, "enable") == 0) {
		if (cfg_set("http.enable", val) == 0) {
			node_cfg_changed("http.enable");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad value");
		}
	} else if (n == 2 && strcmp(sub, "auto") == 0) {
		if (cfg_set("http.auto", val) == 0) {
			node_cfg_changed("http.auto");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad value");
		}
	} else {
		out_line(o, "ERROR bad args");
	}
}

/* AT+PAIR=1 open 60s public pairing window (API.md §8); =0|status query */
static void cmd_pair(struct out *o, const char *args)
{
	if (strcmp(args, "1") == 0) {
		kbd_ble_pair_open();
		out_line(o, "OK");
	} else if (strcmp(args, "0") == 0 || strcmp(args, "status") == 0) {
		outf(o, "+PAIR:window=%d,bond=%d",
		     kbd_ble_pair_window_active() ? 1 : 0,
		     kbd_ble_has_bond() ? 1 : 0);
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR bad args");
	}
}

/* AT+LED=<r>,<g>,<b>|off|auto */
static void cmd_led(struct out *o, char *args)
{
	if (strcmp(args, "off") == 0) {
		led_off();
		out_line(o, "OK");
		return;
	}
	if (strcmp(args, "auto") == 0) {
		led_status(wifi_sta_is_up() ? LED_ONLINE : LED_WIFI_CONNECTING);
		out_line(o, "OK");
		return;
	}
	if (split(args, s_fields, 3) == 3 && to_u32(s_fields[0]) <= 255 &&
	    to_u32(s_fields[1]) <= 255 && to_u32(s_fields[2]) <= 255) {
		led_set_rgb((uint8_t)to_u32(s_fields[0]), (uint8_t)to_u32(s_fields[1]),
			    (uint8_t)to_u32(s_fields[2]));
		out_line(o, "OK");
	} else {
		out_line(o, "ERROR bad args");
	}
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static void dispatch(char *line, struct out *o)
{
	if (strcmp(line, "AT") == 0) {
		out_line(o, "OK");
	} else if (strcmp(line, "AT+VER") == 0) {
		out_line(o, "AT-Node v1.0 [zephyr-s3]");
		out_line(o, "OK");
	} else if (strcmp(line, "AT+HELP") == 0) {
		cmd_help(o);
	} else if (strcmp(line, "AT+STATUS") == 0) {
		cmd_status(o);
	} else if (strcmp(line, "AT+ABILITY") == 0) {
		out_line(o, "{\"ble\":1,\"usb\":1,\"mqtt\":1,\"http\":1,"
			    "\"i2c\":1,\"adc\":1,\"ir\":0,\"rathole\":0}");
		out_line(o, "OK");
	} else if (strncmp(line, "AT+SET=", 7) == 0) {
		cmd_set(o, line + 7);
	} else if (strncmp(line, "AT+GET=", 7) == 0) {
		cmd_get(o, line + 7);
	} else if (strcmp(line, "AT+KEYS") == 0) {
		cfg_list(keys_cb, o);
		out_line(o, "OK");
	} else if (strncmp(line, "AT+TAP=", 7) == 0) {
		cmd_tap(o, line + 7);
	} else if (strncmp(line, "AT+KEY=", 7) == 0) {
		cmd_key(o, line + 7);
	} else if (strncmp(line, "AT+KEY_STR=", 11) == 0) {
		cmd_key_str(o, line + 11);
	} else if (strncmp(line, "AT+KEY_SEQ=", 11) == 0) {
		cmd_key_seq(o, line + 11);
	} else if (strncmp(line, "AT+DEV=", 7) == 0) {
		cmd_dev(o, line + 7);
	} else if (strcmp(line, "AT+DEV") == 0) {
		cmd_dev(o, NULL);
	} else if (strncmp(line, "AT+GPIO_W=", 10) == 0) {
		cmd_gpio_w(o, line + 10);
	} else if (strncmp(line, "AT+GPIO_R=", 10) == 0) {
		cmd_gpio_r(o, line + 10);
	} else if (strncmp(line, "AT+ADC=", 7) == 0) {
		cmd_adc(o, line + 7);
	} else if (strcmp(line, "AT+I2C_SCAN") == 0) {
		cmd_i2c_scan(o);
	} else if (strncmp(line, "AT+I2C_R=", 9) == 0) {
		cmd_i2c_r(o, line + 9);
	} else if (strncmp(line, "AT+I2C_W=", 9) == 0) {
		cmd_i2c_w(o, line + 9);
	} else if (strncmp(line, "AT+WIFI=", 8) == 0) {
		cmd_wifi(o, line + 8);
	} else if (strncmp(line, "AT+MQTT=", 8) == 0) {
		cmd_mqtt(o, line + 8);
	} else if (strncmp(line, "AT+HTTP=", 8) == 0) {
		cmd_http(o, line + 8);
	} else if (strcmp(line, "AT+HTTP") == 0) {
		cmd_http(o, s_status);
	} else if (strncmp(line, "AT+PAIR=", 8) == 0) {
		cmd_pair(o, line + 8);
	} else if (strcmp(line, "AT+PAIR") == 0) {
		cmd_pair(o, s_status);
	} else if (strcmp(line, "AT+UNPAIR") == 0) {
		kbd_ble_clear_bonds();
		out_line(o, "OK");
	} else if (strncmp(line, "AT+LED=", 7) == 0) {
		cmd_led(o, line + 7);
	} else if (strncmp(line, "AT+NVS=", 7) == 0) {
		if (strcmp(line + 7, "clear") == 0) {
			cfg_erase_all();
			out_line(o, "NVS erased, reboot with AT+RST");
			out_line(o, "OK");
		} else {
			out_line(o, "ERROR bad args");
		}
	} else if (strcmp(line, "AT+RST") == 0) {
		out_line(o, "OK");
		k_sleep(K_MSEC(100)); /* let the transport flush */
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		out_line(o, "ERROR unknown cmd");
	}
}

void at_handle_line(const char *line, at_emit_fn emit, void *ctx)
{
	struct out o = { emit, ctx };
	size_t n;

	if (line == NULL) {
		line = "";
	}
	k_mutex_lock(&at_lock, K_FOREVER);

	/* Bounded copy, then strip trailing whitespace/CR/LF (Arduino
	 * line.trim() semantics in the serial handler).
	 */
	n = strlen(line);
	if (n >= sizeof(s_args)) {
		n = sizeof(s_args) - 1;
	}
	memcpy(s_args, line, n);
	s_args[n] = '\0';
	while (n > 0 && (s_args[n - 1] == ' ' || s_args[n - 1] == '\t' ||
			 s_args[n - 1] == '\r' || s_args[n - 1] == '\n')) {
		s_args[--n] = '\0';
	}

	dispatch(s_args, &o);

	k_mutex_unlock(&at_lock);
}

/* ------------------------------------------------------------------ */
/* at_handle_collect — synchronous collect for HTTP/MQTT transports    */
/* ------------------------------------------------------------------ */

struct collect_ctx {
	char *buf;
	int len;
	int pos;
};

static void collect_emit(const char *line, void *ctx)
{
	struct collect_ctx *c = ctx;
	int n;

	if (c->pos >= c->len - 1) {
		return;
	}
	n = snprintk(c->buf + c->pos, c->len - c->pos, "%s\n", line);
	if (n > c->len - 1 - c->pos) {
		n = c->len - 1 - c->pos; /* snprintk may report would-be length */
	}
	if (n > 0) {
		c->pos += n;
	}
}

int at_handle_collect(const char *line, char *buf, int len)
{
	struct collect_ctx c = { buf, len, 0 };

	if (buf == NULL || len <= 0) {
		return 0;
	}
	buf[0] = '\0';
	at_handle_line(line, collect_emit, &c);
	buf[c.pos] = '\0';
	return c.pos;
}
