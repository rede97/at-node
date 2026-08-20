/*
 * AT-Node Zephyr — HTTP control plane (route map in httpd.h).
 *
 * Zephyr HTTP server (CONFIG_HTTP_SERVER): one static gzip SPA resource +
 * dynamic JSON resources mirroring the Arduino variant's handlers.
 * Bodies arrive as JSON (CONFIG_JSON_LIBRARY) or k=v form/query params,
 * like the Arduino WebServer arg() semantics.
 *
 * Concurrency: the server runs single-threaded and serializes every
 * dynamic resource through its `holder`, so per-resource static body
 * accumulators plus one shared response buffer are safe.
 *
 * Query params for POST handlers are read from client->url_buffer starting
 * at current_detail->path_len (the in-tree websocket code does the same in
 * http_server_ws.c); GET params arrive as request_ctx->data.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/status.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "at_core.h"
#include "cfg.h"
#include "hws.h"
#include "httpd.h"
#include "kbd.h"
#include "mqttc.h"
#include "web_page_zephyr.h"
#include "wifi_sta.h"

#define HTTPD_PORT     80
#define BODY_MAX       512  /* max POST body (JSON / form / raw AT line) */
#define AT_RESP_MAX    1200 /* collected AT response before JSON escaping */
#define I2C_DATA_MAX   32

/* ------------------------------------------------------------------ */
/* Shared response buffer (single-threaded server, one-shot responses) */
/* ------------------------------------------------------------------ */

static char rsp_buf[3072];

static const struct http_header cors_hdrs[] = {
	{ .name = "Access-Control-Allow-Origin", .value = "*" },
};

static void rsp_take(struct http_response_ctx *rsp, const void *body, size_t len,
		     enum http_status code)
{
	rsp->status = code;
	rsp->body = body;
	rsp->body_len = len;
	rsp->headers = cors_hdrs;
	rsp->header_count = ARRAY_SIZE(cors_hdrs);
	rsp->final_chunk = true;
}

/* Bounded append into rsp_buf; returns new offset (never overflows). */
static size_t appf(size_t off, const char *fmt, ...)
{
	va_list ap;
	size_t left = sizeof(rsp_buf) - off;
	int n;

	if (left == 0) {
		return off;
	}
	va_start(ap, fmt);
	n = vsnprintk(rsp_buf + off, left, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return off;
	}
	if ((size_t)n >= left) {
		return sizeof(rsp_buf) - 1;
	}
	return off + (size_t)n;
}

/* Append s JSON-escaped (no surrounding quotes). */
static size_t app_esc(size_t off, const char *s)
{
	for (; *s != '\0' && off < sizeof(rsp_buf) - 8; s++) {
		uint8_t c = (uint8_t)*s;

		switch (c) {
		case '"':
			rsp_buf[off++] = '\\';
			rsp_buf[off++] = '"';
			break;
		case '\\':
			rsp_buf[off++] = '\\';
			rsp_buf[off++] = '\\';
			break;
		case '\n':
			rsp_buf[off++] = '\\';
			rsp_buf[off++] = 'n';
			break;
		case '\r':
			rsp_buf[off++] = '\\';
			rsp_buf[off++] = 'r';
			break;
		case '\t':
			rsp_buf[off++] = '\\';
			rsp_buf[off++] = 't';
			break;
		default:
			if (c < 0x20) {
				off = appf(off, "\\u%04x", c);
			} else {
				rsp_buf[off++] = (char)c;
			}
			break;
		}
	}
	rsp_buf[off] = '\0';
	return off;
}

static void rsp_ok(struct http_response_ctx *rsp, size_t len)
{
	rsp_take(rsp, rsp_buf, len, HTTP_200_OK);
}

static void rsp_err(struct http_response_ctx *rsp, enum http_status code, const char *msg)
{
	size_t off = appf(0, "{\"ok\":false,\"error\":\"%s\"}", msg);

	rsp_take(rsp, rsp_buf, off, code);
}

/* ------------------------------------------------------------------ */
/* Body accumulation + param extraction                                */
/* ------------------------------------------------------------------ */

struct body_acc {
	size_t len;
	bool overflow;
	uint8_t buf[BODY_MAX + 1];
};

/* Feed one request fragment. Returns 1 when the final body is ready
 * (NUL-terminated in acc->buf), 0 when more data is expected, -1 on a
 * terminal notification (acc already reset).
 */
static int acc_feed(struct body_acc *acc, enum http_transaction_status status,
		    const struct http_request_ctx *req)
{
	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		acc->len = 0;
		acc->overflow = false;
		return -1;
	}
	if (req->data != NULL && req->data_len > 0 && !acc->overflow) {
		if (acc->len + req->data_len > BODY_MAX) {
			acc->overflow = true;
		} else {
			memcpy(acc->buf + acc->len, req->data, req->data_len);
			acc->len += req->data_len;
		}
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	acc->buf[acc->len] = '\0';
	return 1;
}

static void acc_reset(struct body_acc *acc)
{
	acc->len = 0;
	acc->overflow = false;
}

static bool acc_is_json(const struct body_acc *acc)
{
	for (size_t i = 0; i < acc->len; i++) {
		if (!isspace(acc->buf[i])) {
			return acc->buf[i] == '{';
		}
	}
	return false;
}

/* Request view passed to handlers. */
struct httpd_req {
	struct http_client_ctx *client;
	const char *query;      /* "?k=v&..." or "" */
	struct body_acc *acc;   /* NULL for handlers without a body */
	bool json;              /* body is a JSON object */
};

/* Query string of the current request (points at '?' or '\0'). Works for
 * HTTP/1 (client->current_detail) and HTTP/2 (per-stream detail).
 */
static const char *req_query(struct http_client_ctx *client)
{
	struct http_resource_detail *d = client->current_detail;

	if (d == NULL && client->current_stream != NULL) {
		d = client->current_stream->current_detail;
	}
	if (d == NULL || d->path_len <= 0 ||
	    d->path_len >= (int)sizeof(client->url_buffer)) {
		return "";
	}
	return (const char *)client->url_buffer + d->path_len;
}

static void url_decode(const char *src, size_t n, char *dst, size_t dst_len)
{
	size_t o = 0;

	for (size_t i = 0; i < n && o + 1 < dst_len; i++) {
		char c = src[i];

		if (c == '%' && i + 2 < n && isxdigit((uint8_t)src[i + 1]) &&
		    isxdigit((uint8_t)src[i + 2])) {
			char hex[3] = { src[i + 1], src[i + 2], '\0' };

			dst[o++] = (char)strtoul(hex, NULL, 16);
			i += 2;
		} else if (c == '+') {
			dst[o++] = ' ';
		} else {
			dst[o++] = c;
		}
	}
	dst[o] = '\0';
}

/* Find key in a "k=v&k2=v2" string (leading '?' skipped); value URL-decoded. */
static bool kv_find(const char *s, size_t len, const char *key, char *out, size_t out_len)
{
	size_t klen = strlen(key);
	size_t i = 0;

	if (len > 0 && s[0] == '?') {
		i = 1;
	}
	while (i < len) {
		size_t seg_end = i;

		while (seg_end < len && s[seg_end] != '&') {
			seg_end++;
		}
		const char *eq = memchr(s + i, '=', seg_end - i);
		size_t name_len = (eq != NULL) ? (size_t)(eq - (s + i)) : seg_end - i;

		if (name_len == klen && memcmp(s + i, key, klen) == 0) {
			if (eq == NULL) {
				out[0] = '\0';
			} else {
				url_decode(eq + 1, seg_end - (size_t)(eq + 1 - s), out,
					   out_len);
			}
			return true;
		}
		i = seg_end + 1;
	}
	return false;
}

static bool param_str(const struct httpd_req *r, const char *key, char *out, size_t len)
{
	if (r->query != NULL && kv_find(r->query, strlen(r->query), key, out, len)) {
		return true;
	}
	if (r->acc != NULL && !r->json && r->acc->len > 0 &&
	    kv_find((const char *)r->acc->buf, r->acc->len, key, out, len)) {
		return true;
	}
	return false;
}

/* Integer param: decimal, with 0x.. hex accepted (Arduino toInt/strtoul). */
static bool param_int(const struct httpd_req *r, const char *key, long *v)
{
	char tmp[24];
	char *end;
	long x;

	if (!param_str(r, key, tmp, sizeof(tmp))) {
		return false;
	}
	if (tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) {
		x = strtol(tmp, &end, 0);
	} else {
		x = strtol(tmp, &end, 10);
	}
	if (end == tmp) {
		return false;
	}
	*v = x;
	return true;
}

static bool hid_available(void)
{
	return kbd_ble_connected() || kbd_usb_ready();
}

/* Common POST handler prologue: accumulate body; false = response sent or
 * more data pending (handler must return immediately).
 */
static bool post_body_ready(struct body_acc *acc, enum http_transaction_status status,
			    const struct http_request_ctx *req,
			    struct http_response_ctx *rsp)
{
	int rc = acc_feed(acc, status, req);

	if (rc <= 0) {
		return false;
	}
	if (acc->overflow) {
		acc_reset(acc);
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "body too large");
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* JSON body descriptors                                               */
/* ------------------------------------------------------------------ */

struct j_tap {
	int32_t mods;
	int32_t k;
	int32_t ms;
};
static const struct json_obj_descr j_tap_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_tap, mods, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_tap, k, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_tap, ms, JSON_TOK_NUMBER),
};

struct j_text {
	char *s;
	int32_t ms;
	int32_t gap;
};
static const struct json_obj_descr j_text_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_text, s, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct j_text, ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_text, gap, JSON_TOK_NUMBER),
};

struct j_key {
	int32_t mods;
	int32_t keys[6];
	size_t keys_len;
};
static const struct json_obj_descr j_key_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_key, mods, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_ARRAY(struct j_key, keys, 6, keys_len, JSON_TOK_NUMBER),
};

struct j_pin_level {
	int32_t pin;
	int32_t level;
};
static const struct json_obj_descr j_pin_level_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_pin_level, pin, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_pin_level, level, JSON_TOK_NUMBER),
};

struct j_pin {
	int32_t pin;
};
static const struct json_obj_descr j_pin_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_pin, pin, JSON_TOK_NUMBER),
};

struct j_ch {
	int32_t ch;
};
static const struct json_obj_descr j_ch_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_ch, ch, JSON_TOK_NUMBER),
};

struct j_i2c_read {
	int32_t addr;
	int32_t reg;
	int32_t len;
};
static const struct json_obj_descr j_i2c_read_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_i2c_read, addr, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_i2c_read, reg, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_i2c_read, len, JSON_TOK_NUMBER),
};

struct j_i2c_write {
	int32_t addr;
	int32_t reg;
	int32_t data[I2C_DATA_MAX];
	size_t data_len;
};
static const struct json_obj_descr j_i2c_write_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_i2c_write, addr, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct j_i2c_write, reg, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_ARRAY(struct j_i2c_write, data, I2C_DATA_MAX, data_len,
			     JSON_TOK_NUMBER),
};

struct j_cfg_set {
	char *key;
	char *val;
	char *value;
};
static const struct json_obj_descr j_cfg_set_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_cfg_set, key, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct j_cfg_set, val, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct j_cfg_set, value, JSON_TOK_STRING),
};

struct j_enable {
	int32_t enable;
};
static const struct json_obj_descr j_enable_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct j_enable, enable, JSON_TOK_NUMBER),
};

/* ------------------------------------------------------------------ */
/* GET endpoints                                                       */
/* ------------------------------------------------------------------ */

/* Feature flags object, same shape as Arduino build_ability_json();
 * embedded in /cmd/status and served by /cmd/ability. Booleans, not 1/0:
 * the SPA's applyAbility() hides tabs only on a strict === false.
 */
static const char ABILITY_OBJ[] =
	"{\"ble\":true,\"usb\":true,\"mqtt\":true,\"http\":true,"
	"\"i2c\":true,\"adc\":true,\"ir\":false,\"rathole\":false,"
	"\"breath_led\":false}";

static int h_status(struct http_client_ctx *client, enum http_transaction_status status,
		    const struct http_request_ctx *req, struct http_response_ctx *rsp,
		    void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	char name[32], ip[16], mac[18], ble_addr[20];
	size_t off;
	int rssi, pct;

	cfg_get_str("device.name", name, sizeof(name), "AT-Node-S3");
	wifi_sta_ip_str(ip, sizeof(ip));
	wifi_sta_mac_str(mac, sizeof(mac));
	kbd_ble_addr_str(ble_addr, sizeof(ble_addr));

	/* Arduino rssi_to_pct(): clamp(2*(rssi+100), 0, 100) */
	rssi = wifi_sta_rssi();
	pct = 2 * (rssi + 100);
	if (pct < 0) {
		pct = 0;
	} else if (pct > 100) {
		pct = 100;
	}

	/* Arduino handle_cmd_status() field set. heap/temp_c omitted (no
	 * CONFIG_SYS_HEAP_RUNTIME_STATS / temp sensor here; the SPA guards
	 * undefined), likewise ble_rssi/ble_pct (no per-peer RSSI API).
	 */
	off = appf(0, "{\"ok\":true,\"device\":\"");
	off = app_esc(off, name);
	off = appf(off, "\",\"hostname\":\"");
	off = app_esc(off, name);
	off = appf(off, "\",\"ip\":\"%s\",\"mac\":\"%s\",\"wifi_rssi\":%d,"
		       "\"wifi_pct\":%d,\"ble_addr\":\"",
		   ip, mac, rssi, pct);
	off = app_esc(off, ble_addr);
	off = appf(off, "\",\"connected\":%s,\"typing\":%s,\"mqtt\":%s,"
		       "\"ap\":false,\"http_enabled\":%s,\"usb\":%s,"
		       "\"targets\":%u,\"ability\":%s}",
		   kbd_ble_connected() ? "true" : "false",
		   kbd_typing() ? "true" : "false",
		   mqttc_connected() ? "true" : "false",
		   httpd_running() ? "true" : "false",
		   kbd_usb_ready() ? "true" : "false",
		   (unsigned int)kbd_get_targets(), ABILITY_OBJ);
	rsp_ok(rsp, off);
	return 0;
}

static int h_ability(struct http_client_ctx *client, enum http_transaction_status status,
		     const struct http_request_ctx *req, struct http_response_ctx *rsp,
		     void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"ability\":%s}", ABILITY_OBJ));
	return 0;
}

/* Machine-readable API catalog (Arduino build_services_json() shape),
 * trimmed to the endpoints this firmware implements.
 */
static const char HELP_JSON[] =
	"{\"ok\":true,\"services\":{"
	"\"keyboard/tap\":{\"d\":\"press+release one key\",\"p\":{"
	"\"mods\":\"modifier mask (0x01=Ctrl 0x02=Shift 0x04=Alt 0x08=GUI)\","
	"\"k\":\"HID keycode (4=a 5=b ... 0x39=CapsLock)\","
	"\"ms\":\"hold duration ms, default 100\"}},"
	"\"keyboard/text\":{\"d\":\"type ASCII string\",\"p\":{"
	"\"s\":\"ASCII text to type\",\"ms\":\"per-key hold ms, default 40\","
	"\"gap\":\"inter-key gap ms, default 30\"}},"
	"\"keyboard/key\":{\"d\":\"raw 6KRO report (hold until released)\",\"p\":{"
	"\"mods\":\"modifier mask\",\"keys\":\"array of up to 6 HID keycodes (0=none)\"}},"
	"\"gpio/write\":{\"d\":\"set GPIO output level\",\"p\":{"
	"\"pin\":\"GPIO number\",\"level\":\"0=LOW 1=HIGH\"}},"
	"\"gpio/read\":{\"d\":\"read GPIO input\",\"p\":{\"pin\":\"GPIO number\"}},"
	"\"adc/read\":{\"d\":\"read ADC millivolts\",\"p\":{\"ch\":\"ADC channel\"}},"
	"\"i2c/scan\":{\"d\":\"scan I2C bus for devices\",\"p\":{}},"
	"\"i2c/read\":{\"d\":\"read I2C register\",\"p\":{"
	"\"addr\":\"I2C device address (hex ok)\",\"reg\":\"register address\","
	"\"len\":\"bytes to read, 1-32\"}},"
	"\"i2c/write\":{\"d\":\"write I2C register\",\"p\":{"
	"\"addr\":\"I2C device address (hex ok)\",\"reg\":\"register address\","
	"\"data\":\"byte array or hex string (e.g. FF01)\"}},"
	"\"ble/pair\":{\"d\":\"open 60s public pairing window\",\"p\":{"
	"\"enable\":\"1=start 60s public advertising\"}},"
	"\"ble/status\":{\"d\":\"BLE state: addr, advertising, peers, bonds\",\"p\":{}},"
	"\"ble/bonds/delete\":{\"d\":\"remove one bonded host\",\"p\":{"
	"\"idx\":\"bond index from ble/status\"}},"
	"\"ble/bonds/clear\":{\"d\":\"remove ALL bonded hosts\",\"p\":{}},"
	"\"mqtt/status\":{\"d\":\"MQTT state: connected, broker, port, client_id\",\"p\":{}},"
	"\"mqtt/config\":{\"d\":\"set mqtt.* config keys (alias)\",\"p\":{"
	"\"broker\":\"host/IP\",\"port\":\"1-65535 (default 8883)\","
	"\"user\":\"auth user\",\"pass\":\"auth password (write-only)\","
	"\"auto\":\"1=auto-connect at boot\"}},"
	"\"mqtt/connect\":{\"d\":\"start the MQTT client now\",\"p\":{}},"
	"\"mqtt/clear\":{\"d\":\"stop MQTT and reset all mqtt.* keys\",\"p\":{}},"
	"\"mqtt/ca\":{\"d\":\"no-op: embedded CA cert is used\",\"p\":{"
	"\"fp\":\"ignored CA fingerprint\"}},"
	"\"wifi/config\":{\"d\":\"set wifi.ssid/wifi.pass and re-associate\",\"p\":{"
	"\"ssid\":\"network name\",\"pass\":\"password (empty=open)\"}},"
	"\"ability\":{\"d\":\"feature flags\",\"p\":{}},"
	"\"config/set\":{\"d\":\"unified config: set key=val (NVS)\",\"p\":{"
	"\"key\":\"config key (see config/list)\",\"val\":\"value\"}},"
	"\"config/get\":{\"d\":\"unified config: read key\",\"p\":{\"key\":\"config key\"}},"
	"\"config/list\":{\"d\":\"unified config: list all keys\",\"p\":{}},"
	"\"nvs/clear\":{\"d\":\"factory reset NVS and reboot\",\"p\":{}}"
	"}}";

static int h_help(struct http_client_ctx *client, enum http_transaction_status status,
		  const struct http_request_ctx *req, struct http_response_ctx *rsp,
		  void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp_take(rsp, HELP_JSON, sizeof(HELP_JSON) - 1, HTTP_200_OK);
	return 0;
}

/* --- unified config endpoints ------------------------------------- */

static void config_get(struct httpd_req *r, struct http_response_ctx *rsp)
{
	char key[48], val[CFG_VAL_MAX];
	int rc;

	if (!param_str(r, "key", key, sizeof(key)) || key[0] == '\0' ||
	    !cfg_key_exists(key)) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "unknown key");
		return;
	}
	rc = cfg_get(key, val, sizeof(val));
	if (rc == -EACCES) {
		val[0] = '\0'; /* write-only secret: Arduino returns "" */
	} else if (rc != 0) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "unknown key");
		return;
	}

	size_t off = appf(0, "{\"ok\":true,\"key\":\"");

	off = app_esc(off, key);
	off = appf(off, "\",\"value\":\"");
	off = app_esc(off, val);
	off = appf(off, "\",\"val\":\"");
	off = app_esc(off, val);
	off = appf(off, "\"}");
	rsp_ok(rsp, off);
}

static void config_set(struct httpd_req *r, struct http_response_ctx *rsp)
{
	char key[48], val[CFG_VAL_MAX];
	bool have_key, have_val;
	struct j_cfg_set js = { 0 };
	int rc;

	have_key = param_str(r, "key", key, sizeof(key));
	have_val = param_str(r, "val", val, sizeof(val)) ||
		   param_str(r, "value", val, sizeof(val));

	if (r->json) {
		rc = json_obj_parse((char *)r->acc->buf, r->acc->len, j_cfg_set_descr,
				    ARRAY_SIZE(j_cfg_set_descr), &js);
		if (rc >= 0) {
			if (!have_key && (rc & BIT(0)) && js.key != NULL) {
				strncpy(key, js.key, sizeof(key) - 1);
				key[sizeof(key) - 1] = '\0';
				have_key = true;
			}
			if (!have_val && (rc & BIT(1)) && js.val != NULL) {
				strncpy(val, js.val, sizeof(val) - 1);
				val[sizeof(val) - 1] = '\0';
				have_val = true;
			}
			if (!have_val && (rc & BIT(2)) && js.value != NULL) {
				strncpy(val, js.value, sizeof(val) - 1);
				val[sizeof(val) - 1] = '\0';
				have_val = true;
			}
		}
	}

	if (!have_key || key[0] == '\0') {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "missing key");
		return;
	}
	if (!have_val) {
		val[0] = '\0';
	}
	rc = cfg_set(key, val);
	if (rc != 0) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "unknown key or invalid value");
		return;
	}
	node_cfg_changed(key);

	size_t off = appf(0, "{\"ok\":true,\"cmd\":\"config\",\"key\":\"");

	off = app_esc(off, key);
	off = appf(off, "\"}");
	rsp_ok(rsp, off);
}

static int h_config(struct http_client_ctx *client, enum http_transaction_status status,
		    const struct http_request_ctx *req, struct http_response_ctx *rsp,
		    void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;

	ARG_UNUSED(user_data);

	if (client->method == HTTP_GET) {
		if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
			return 0;
		}
		r.client = client;
		r.query = req_query(client);
		r.acc = NULL;
		r.json = false;
		config_get(&r, rsp);
		return 0;
	}

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);
	config_set(&r, rsp);
	acc_reset(&acc);
	return 0;
}

struct list_ctx {
	size_t off;
	bool first_key;
	bool first_cfg;
};

static void config_list_cb(const char *key, const char *val, bool write_only, void *ctx)
{
	struct list_ctx *c = ctx;

	c->off = appf(c->off, "%s{\"key\":\"", c->first_key ? "" : ",");
	c->first_key = false;
	c->off = app_esc(c->off, key);
	if (write_only) {
		c->off = appf(c->off, "\",\"secret\":true}");
		return; /* omitted from the "config" object */
	}
	c->off = appf(c->off, "\",\"value\":\"");
	c->off = app_esc(c->off, val != NULL ? val : "");
	c->off = appf(c->off, "\"}");
}

static void config_obj_cb(const char *key, const char *val, bool write_only, void *ctx)
{
	struct list_ctx *c = ctx;

	if (write_only) {
		return;
	}
	c->off = appf(c->off, "%s\"", c->first_cfg ? "" : ",");
	c->first_cfg = false;
	c->off = app_esc(c->off, key);
	c->off = appf(c->off, "\":\"");
	c->off = app_esc(c->off, val != NULL ? val : "");
	c->off = appf(c->off, "\"");
}

static int h_config_list(struct http_client_ctx *client,
			 enum http_transaction_status status,
			 const struct http_request_ctx *req,
			 struct http_response_ctx *rsp, void *user_data)
{
	struct list_ctx c = { .first_key = true, .first_cfg = true };

	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	/* Arduino shape: "keys" array; Zephyr addition: "config" object. */
	c.off = appf(0, "{\"ok\":true,\"keys\":[");
	cfg_list(config_list_cb, &c);
	c.off = appf(c.off, "],\"config\":{");
	cfg_list(config_obj_cb, &c);
	c.off = appf(c.off, "}}");
	rsp_ok(rsp, c.off);
	return 0;
}

/* ------------------------------------------------------------------ */
/* POST /at-node/at — raw AT line                                      */
/* ------------------------------------------------------------------ */

static int h_at(struct http_client_ctx *client, enum http_transaction_status status,
		const struct http_request_ctx *req, struct http_response_ctx *rsp,
		void *user_data)
{
	static struct body_acc acc;
	static char at_buf[AT_RESP_MAX];
	static char pbuf[BODY_MAX];
	struct httpd_req r;
	char *cmd;
	int n;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}

	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = false;

	/* Form/query plain|cmd param (Arduino) wins; else raw text body. */
	if (param_str(&r, "plain", pbuf, sizeof(pbuf)) ||
	    param_str(&r, "cmd", pbuf, sizeof(pbuf))) {
		cmd = pbuf;
	} else if (acc.len > 0) {
		cmd = (char *)acc.buf;
	} else {
		acc_reset(&acc);
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "empty command");
		return 0;
	}

	while (isspace((uint8_t)*cmd)) {
		cmd++;
	}
	size_t clen = strlen(cmd);

	while (clen > 0 && isspace((uint8_t)cmd[clen - 1])) {
		cmd[--clen] = '\0';
	}
	if (clen == 0) {
		acc_reset(&acc);
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "empty command");
		return 0;
	}

	n = at_handle_collect(cmd, at_buf, sizeof(at_buf));
	acc_reset(&acc);
	if (n <= 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "at core error");
		return 0;
	}
	at_buf[sizeof(at_buf) - 1] = '\0';

	/* Strip trailing newlines; ok = final line is exactly "OK". */
	size_t alen = strlen(at_buf);

	while (alen > 0 && (at_buf[alen - 1] == '\n' || at_buf[alen - 1] == '\r')) {
		at_buf[--alen] = '\0';
	}
	char *last = strrchr(at_buf, '\n');

	last = (last != NULL) ? last + 1 : at_buf;
	bool ok = strcmp(last, "OK") == 0;

	size_t off = appf(0, "{\"ok\":%s,\"response\":\"", ok ? "true" : "false");

	off = app_esc(off, at_buf);
	off = appf(off, "\"}");
	rsp_ok(rsp, off);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Keyboard endpoints                                                  */
/* ------------------------------------------------------------------ */

static int h_kbd_tap(struct http_client_ctx *client, enum http_transaction_status status,
		     const struct http_request_ctx *req, struct http_response_ctx *rsp,
		     void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long mods = 0, key = 0, ms = 0;
	bool have_ms;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	(void)param_int(&r, "mods", &mods);
	(void)param_int(&r, "k", &key);
	have_ms = param_int(&r, "ms", &ms);

	if (r.json) {
		struct j_tap js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_tap_descr,
				    ARRAY_SIZE(j_tap_descr), &js);
		if (rc >= 0) {
			if (rc & BIT(0)) {
				mods = js.mods;
			}
			if (rc & BIT(1)) {
				key = js.k;
			}
			if (rc & BIT(2)) {
				ms = js.ms;
				have_ms = true;
			}
		}
	}
	acc_reset(&acc);

	if (!have_ms || ms <= 0) {
		ms = 100;
	}
	if (ms > 10000) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid ms");
		return 0;
	}
	if (!hid_available()) {
		rsp_err(rsp, HTTP_409_CONFLICT, "no HID target connected");
		return 0;
	}
	if (key > 0) {
		rc = kbd_tap((uint16_t)ms, (uint8_t)mods, (uint8_t)key);
		if (rc == -EINVAL) {
			rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid ms");
			return 0;
		}
		if (rc != 0) {
			rsp_err(rsp, HTTP_423_LOCKED, "typing in progress");
			return 0;
		}
	}
	/* key == 0: no-op press, still OK (Arduino parse_uint8 semantics) */
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"cmd\":\"keyboard/tap\",\"ms\":%ld}", ms));
	return 0;
}

static int h_kbd_text(struct http_client_ctx *client, enum http_transaction_status status,
		      const struct http_request_ctx *req, struct http_response_ctx *rsp,
		      void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long ms = 0, gap = 0;
	char *text = NULL;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	(void)param_int(&r, "ms", &ms);
	(void)param_int(&r, "gap", &gap);

	if (r.json) {
		struct j_text js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_text_descr,
				    ARRAY_SIZE(j_text_descr), &js);
		if (rc >= 0) {
			if ((rc & BIT(0)) && js.s != NULL) {
				text = js.s;
			}
			if (rc & BIT(1)) {
				ms = js.ms;
			}
			if (rc & BIT(2)) {
				gap = js.gap;
			}
		}
	} else {
		static char sbuf[300];

		if (param_str(&r, "s", sbuf, sizeof(sbuf))) {
			text = sbuf;
		}
	}
	/* note: JSON text points into acc.buf, still valid here */
	if (text == NULL || text[0] == '\0') {
		acc_reset(&acc);
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "missing s");
		return 0;
	}
	if (ms <= 0) {
		ms = 40;
	}
	if (gap <= 0) {
		gap = 30;
	}
	if (!hid_available()) {
		acc_reset(&acc);
		rsp_err(rsp, HTTP_409_CONFLICT, "no HID target connected");
		return 0;
	}
	rc = kbd_type_text(text, (uint16_t)ms, (uint16_t)gap);
	acc_reset(&acc);
	if (rc == -EINVAL) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "text too long");
		return 0;
	}
	if (rc != 0) {
		rsp_err(rsp, HTTP_423_LOCKED, "typing in progress");
		return 0;
	}
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"keyboard/text\",\"queued\":true}",
		 sizeof("{\"ok\":true,\"cmd\":\"keyboard/text\",\"queued\":true}") - 1,
		 HTTP_200_OK);
	return 0;
}

static int h_kbd_key(struct http_client_ctx *client, enum http_transaction_status status,
		     const struct http_request_ctx *req, struct http_response_ctx *rsp,
		     void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long mods = 0;
	uint8_t keys[6] = { 0 };
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	(void)param_int(&r, "mods", &mods);

	if (r.json) {
		struct j_key js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_key_descr,
				    ARRAY_SIZE(j_key_descr), &js);
		if (rc >= 0) {
			if (rc & BIT(0)) {
				mods = js.mods;
			}
			if (rc & BIT(1)) {
				size_t nk = js.keys_len;

				if (nk > 6) {
					nk = 6;
				}
				for (size_t i = 0; i < nk; i++) {
					keys[i] = (uint8_t)js.keys[i];
				}
			}
		}
	} else {
		/* Arduino form: k0..k5 */
		for (int i = 0; i < 6; i++) {
			char pname[4] = { 'k', (char)('0' + i), '\0', '\0' };
			long kv = 0;

			if (param_int(&r, pname, &kv)) {
				keys[i] = (uint8_t)kv;
			}
		}
	}
	acc_reset(&acc);

	rc = kbd_send_report((uint8_t)mods, keys);
	if (rc != 0) {
		rsp_err(rsp, HTTP_409_CONFLICT, "no HID target connected");
		return 0;
	}
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"keyboard/key\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"keyboard/key\"}") - 1, HTTP_200_OK);
	return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO / ADC                                                          */
/* ------------------------------------------------------------------ */

static int h_gpio_write(struct http_client_ctx *client,
			enum http_transaction_status status,
			const struct http_request_ctx *req,
			struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long pin = -1, level = 0;
	bool have_pin, have_level;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	have_pin = param_int(&r, "pin", &pin);
	have_level = param_int(&r, "level", &level);

	if (r.json) {
		struct j_pin_level js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_pin_level_descr,
				    ARRAY_SIZE(j_pin_level_descr), &js);
		if (rc >= 0) {
			if (!have_pin && (rc & BIT(0))) {
				pin = js.pin;
				have_pin = true;
			}
			if (!have_level && (rc & BIT(1))) {
				level = js.level;
				have_level = true;
			}
		}
	}
	acc_reset(&acc);

	if (!have_pin || pin < 0 || pin > 48) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid pin");
		return 0;
	}
	rc = hws_gpio_write((uint8_t)pin, level ? 1 : 0);
	if (rc != 0) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid pin");
		return 0;
	}
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"cmd\":\"gpio/write\",\"pin\":%ld,\"level\":%ld}",
			 pin, level));
	return 0;
}

static int h_gpio_read(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *req,
		       struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long pin = -1;
	bool have_pin;
	int level = 0;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	have_pin = param_int(&r, "pin", &pin);

	if (r.json) {
		struct j_pin js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_pin_descr,
				    ARRAY_SIZE(j_pin_descr), &js);
		if (rc >= 0 && !have_pin && (rc & BIT(0))) {
			pin = js.pin;
			have_pin = true;
		}
	}
	acc_reset(&acc);

	if (!have_pin || pin < 0 || pin > 48) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid pin");
		return 0;
	}
	rc = hws_gpio_read((uint8_t)pin, &level);
	if (rc != 0) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid pin");
		return 0;
	}
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"cmd\":\"gpio/read\",\"pin\":%ld,\"level\":%d}",
			 pin, level));
	return 0;
}

static int h_adc_read(struct http_client_ctx *client, enum http_transaction_status status,
		      const struct http_request_ctx *req, struct http_response_ctx *rsp,
		      void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long ch = -1;
	bool have_ch;
	int mv = 0;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	have_ch = param_int(&r, "ch", &ch);

	if (r.json) {
		struct j_ch js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_ch_descr,
				    ARRAY_SIZE(j_ch_descr), &js);
		if (rc >= 0 && !have_ch && (rc & BIT(0))) {
			ch = js.ch;
			have_ch = true;
		}
	}
	acc_reset(&acc);

	if (!have_ch || ch < 0 || ch > 255) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid adc ch");
		return 0;
	}
	rc = hws_adc_read_mv((uint8_t)ch, &mv);
	if (rc != 0) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid adc ch");
		return 0;
	}
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"cmd\":\"adc/read\",\"ch\":%ld,\"mv\":%d}",
			 ch, mv));
	return 0;
}

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

static int h_i2c_scan(struct http_client_ctx *client, enum http_transaction_status status,
		      const struct http_request_ctx *req, struct http_response_ctx *rsp,
		      void *user_data)
{
	char scan[96];
	int rc;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	ARG_UNUSED(req);

	rc = hws_i2c_scan(scan, sizeof(scan));
	if (rc < 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "i2c error");
		return 0;
	}

	/* hws: "+I2C: 0x3C 0x50" / "+I2C: none" -> ["0x3c","0x50"] */
	size_t off = appf(0, "{\"ok\":true,\"cmd\":\"i2c/scan\",\"devices\":[");
	char *p = strchr(scan, ':');
	bool first = true;

	if (p != NULL) {
		p++;
		while (*p != '\0') {
			while (*p == ' ') {
				p++;
			}
			if (*p == '\0') {
				break;
			}
			char *e = p;

			while (*e != '\0' && *e != ' ') {
				e++;
			}
			if ((size_t)(e - p) == 4 && memcmp(p, "none", 4) == 0) {
				break;
			}
			char tok[12];
			size_t tl = (size_t)(e - p);

			if (tl >= sizeof(tok)) {
				tl = sizeof(tok) - 1;
			}
			for (size_t i = 0; i < tl; i++) {
				tok[i] = (char)tolower((uint8_t)p[i]);
			}
			tok[tl] = '\0';
			off = appf(off, "%s\"%s\"", first ? "" : ",", tok);
			first = false;
			p = e;
		}
	}
	off = appf(off, "]}");
	rsp_ok(rsp, off);
	return 0;
}

static int h_i2c_read(struct http_client_ctx *client, enum http_transaction_status status,
		      const struct http_request_ctx *req, struct http_response_ctx *rsp,
		      void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long addr = 0, reg = 0, len = 0;
	uint8_t data[I2C_DATA_MAX];
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	(void)param_int(&r, "addr", &addr);
	(void)param_int(&r, "reg", &reg);
	(void)param_int(&r, "len", &len);

	if (r.json) {
		struct j_i2c_read js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_i2c_read_descr,
				    ARRAY_SIZE(j_i2c_read_descr), &js);
		if (rc >= 0) {
			if (rc & BIT(0)) {
				addr = js.addr;
			}
			if (rc & BIT(1)) {
				reg = js.reg;
			}
			if (rc & BIT(2)) {
				len = js.len;
			}
		}
	}
	acc_reset(&acc);

	if (len <= 0 || len > I2C_DATA_MAX) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "len must be 1-32");
		return 0;
	}
	rc = hws_i2c_read((uint8_t)addr, (uint8_t)reg, data, (size_t)len);
	if (rc != 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "i2c no ack");
		return 0;
	}

	size_t off = appf(0, "{\"ok\":true,\"cmd\":\"i2c/read\",\"addr\":\"0x%lx\","
			     "\"reg\":\"0x%lx\",\"data\":[", addr, reg);

	for (long i = 0; i < len; i++) {
		off = appf(off, "%s%u", i == 0 ? "" : ",", data[i]);
	}
	off = appf(off, "]}");
	rsp_ok(rsp, off);
	return 0;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int max)
{
	char clean[2 * I2C_DATA_MAX + 1];
	size_t n = 0;

	for (const char *p = hex; *p != '\0'; p++) {
		if (isspace((uint8_t)*p)) {
			continue;
		}
		if (!isxdigit((uint8_t)*p) || n >= sizeof(clean) - 1) {
			return -1;
		}
		clean[n++] = *p;
	}
	clean[n] = '\0';
	if (n == 0 || (n % 2) != 0 || n / 2 > (size_t)max) {
		return -1;
	}
	for (size_t i = 0; i < n / 2; i++) {
		char pair[3] = { clean[2 * i], clean[2 * i + 1], '\0' };

		out[i] = (uint8_t)strtoul(pair, NULL, 16);
	}
	return (int)(n / 2);
}

static int h_i2c_write(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *req,
		       struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long addr = 0, reg = 0;
	uint8_t data[I2C_DATA_MAX];
	int ndata = 0;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	(void)param_int(&r, "addr", &addr);
	(void)param_int(&r, "reg", &reg);

	if (r.json) {
		struct j_i2c_write js = { 0 };

		rc = json_obj_parse((char *)acc.buf, acc.len, j_i2c_write_descr,
				    ARRAY_SIZE(j_i2c_write_descr), &js);
		if (rc >= 0) {
			if (rc & BIT(0)) {
				addr = js.addr;
			}
			if (rc & BIT(1)) {
				reg = js.reg;
			}
			if (rc & BIT(2)) {
				ndata = (int)js.data_len;
				for (int i = 0; i < ndata; i++) {
					if (js.data[i] < 0 || js.data[i] > 255) {
						ndata = -1;
						break;
					}
					data[i] = (uint8_t)js.data[i];
				}
			}
			if ((rc & BIT(2)) && js.data_len == 0) {
				ndata = -1;
			}
		}
	} else {
		char hex[2 * I2C_DATA_MAX + 8];

		if (param_str(&r, "data", hex, sizeof(hex))) {
			ndata = hex_to_bytes(hex, data, I2C_DATA_MAX);
		} else {
			ndata = -1;
		}
	}
	acc_reset(&acc);

	if (ndata < 0) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "data must be hex pairs");
		return 0;
	}
	rc = hws_i2c_write((uint8_t)addr, (uint8_t)reg, data, (size_t)ndata);
	if (rc != 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "i2c no ack");
		return 0;
	}
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"cmd\":\"i2c/write\",\"addr\":\"0x%lx\","
			    "\"reg\":\"0x%lx\"}", addr, reg));
	return 0;
}

/* ------------------------------------------------------------------ */
/* BLE pairing / factory reset                                         */
/* ------------------------------------------------------------------ */

static int h_ble_pair(struct http_client_ctx *client, enum http_transaction_status status,
		      const struct http_request_ctx *req, struct http_response_ctx *rsp,
		      void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	char val[12];

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	val[0] = '\0';
	if (!param_str(&r, "enable", val, sizeof(val))) {
		(void)param_str(&r, "start", val, sizeof(val));
	}
	if (val[0] == '\0' && r.json) {
		struct j_enable js = { 0 };
		int rc = json_obj_parse((char *)acc.buf, acc.len, j_enable_descr,
					ARRAY_SIZE(j_enable_descr), &js);

		if (rc >= 0 && (rc & BIT(0))) {
			snprintk(val, sizeof(val), "%ld", (long)js.enable);
		}
	}
	acc_reset(&acc);

	if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0) {
		kbd_ble_pair_open();
	}
/* enable=0: no close API; the 60 s window expires by itself */
	rsp_ok(rsp, appf(0, "{\"ok\":true,\"cmd\":\"ble/pair\",\"advertising\":%s,"
			    "\"pairing_mode\":%s}",
			 kbd_ble_pair_window_active() ? "true" : "false",
			 kbd_ble_pair_window_active() ? "true" : "false"));
	return 0;
}

/* ------------------------------------------------------------------ */
/* BLE status + bond management (web UI BLE pane)                      */
/* ------------------------------------------------------------------ */

/* kbd_ble_*_json() output staged here; safe: single-threaded server. */
static char ble_peers_buf[512];
static char ble_bonds_buf[512];

static int h_ble_status(struct http_client_ctx *client,
			enum http_transaction_status status,
			const struct http_request_ctx *req,
			struct http_response_ctx *rsp, void *user_data)
{
	char name[32], addr[20];
	size_t off;

	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	cfg_get_str("device.name", name, sizeof(name), "AT-Node-S3");
	kbd_ble_addr_str(addr, sizeof(addr));

	/* Peers/bonds arrive as ready JSON arrays; embed raw (the SPA
	 * expects real arrays, not strings).
	 */
	if (kbd_ble_peers_json(ble_peers_buf, sizeof(ble_peers_buf)) < 0) {
		snprintk(ble_peers_buf, sizeof(ble_peers_buf), "[]");
	}
	ble_peers_buf[sizeof(ble_peers_buf) - 1] = '\0';
	if (kbd_ble_bonds_json(ble_bonds_buf, sizeof(ble_bonds_buf)) < 0) {
		snprintk(ble_bonds_buf, sizeof(ble_bonds_buf), "[]");
	}
	ble_bonds_buf[sizeof(ble_bonds_buf) - 1] = '\0';

	off = appf(0, "{\"ok\":true,\"name\":\"");
	off = app_esc(off, name);
	off = appf(off, "\",\"addr\":\"");
	off = app_esc(off, addr);
	off = appf(off, "\",\"connected\":%s,\"advertising\":%s,"
		       "\"pairing_mode\":%s,\"peers\":%s,\"bonds\":%s}",
		   kbd_ble_connected() ? "true" : "false",
		   kbd_ble_advertising() ? "true" : "false",
		   kbd_ble_pair_window_active() ? "true" : "false",
		   ble_peers_buf, ble_bonds_buf);
	rsp_ok(rsp, off);
	return 0;
}

static int h_ble_bonds_delete(struct http_client_ctx *client,
			      enum http_transaction_status status,
			      const struct http_request_ctx *req,
			      struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	long idx;
	bool have_idx;
	int rc;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = false;

	have_idx = param_int(&r, "idx", &idx);
	acc_reset(&acc);

	if (!have_idx) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "missing idx");
		return 0;
	}
	rc = kbd_ble_unbond_idx((int)idx);
	if (rc == -ENOENT) {
		rsp_err(rsp, HTTP_404_NOT_FOUND, "invalid idx");
		return 0;
	}
	if (rc != 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "unpair failed");
		return 0;
	}
	printk("HTTPD: ble bond idx=%ld removed\n", idx);
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"ble/bonds/delete\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"ble/bonds/delete\"}") - 1,
		 HTTP_200_OK);
	return 0;
}

static int h_ble_bonds_clear(struct http_client_ctx *client,
			     enum http_transaction_status status,
			     const struct http_request_ctx *req,
			     struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	ARG_UNUSED(req);

	kbd_ble_clear_bonds();
	printk("HTTPD: ble bonds cleared\n");
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"ble/bonds/clear\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"ble/bonds/clear\"}") - 1,
		 HTTP_200_OK);
	return 0;
}

/* ------------------------------------------------------------------ */
/* MQTT status / config / lifecycle (web UI MQTT pane)                 */
/* ------------------------------------------------------------------ */

static int h_mqtt_status(struct http_client_ctx *client,
			 enum http_transaction_status status,
			 const struct http_request_ctx *req,
			 struct http_response_ctx *rsp, void *user_data)
{
	char name[32], broker[CFG_VAL_MAX];
	size_t off;

	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	cfg_get_str("device.name", name, sizeof(name), "AT-Node-S3");
	cfg_get_str("mqtt.broker", broker, sizeof(broker), "");

	/* ca_fp is "" here: the Zephyr build pins an embedded CA cert
	 * (ca_cert.h) instead of a fingerprint override.
	 */
	off = appf(0, "{\"ok\":true,\"connected\":%s,\"client_id\":\"",
		   mqttc_connected() ? "true" : "false");
	off = app_esc(off, name);
	off = appf(off, "\",\"broker\":\"");
	off = app_esc(off, broker);
	off = appf(off, "\",\"port\":%d,\"ca_fp\":\"\",\"auto\":%s,"
		       "\"enable\":%s}",
		   cfg_get_int("mqtt.port", 8883),
		   cfg_get_bool("mqtt.auto", false) ? "true" : "false",
		   cfg_get_bool("mqtt.enable", false) ? "true" : "false");
	rsp_ok(rsp, off);
	return 0;
}

/* Legacy alias over the unified config layer (Arduino handle_mqtt_config). */
static const struct {
	const char *param;
	const char *key;
} mqtt_cfg_map[] = {
	{ "broker", "mqtt.broker" },
	{ "port",   "mqtt.port" },
	{ "user",   "mqtt.user" },
	{ "pass",   "mqtt.pass" },
	{ "auto",   "mqtt.auto" },
};

static int h_mqtt_config(struct http_client_ctx *client,
			 enum http_transaction_status status,
			 const struct http_request_ctx *req,
			 struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	bool changed = false;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	for (size_t i = 0; i < ARRAY_SIZE(mqtt_cfg_map); i++) {
		char v[CFG_VAL_MAX];

		if (!param_str(&r, mqtt_cfg_map[i].param, v, sizeof(v))) {
			continue;
		}
		/* Empty port would fail INT validation; treat as "keep". */
		if (strcmp(mqtt_cfg_map[i].param, "port") == 0 && v[0] == '\0') {
			continue;
		}
		if (cfg_set(mqtt_cfg_map[i].key, v) != 0) {
			acc_reset(&acc);
			rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid value");
			return 0;
		}
		changed = true;
	}
	acc_reset(&acc);

	if (changed) {
		/* One notification for the batch; main.c fan-out restarts
		 * the client when mqtt.enable is set.
		 */
		node_cfg_changed("mqtt.broker");
	}
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"mqtt/config\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"mqtt/config\"}") - 1, HTTP_200_OK);
	return 0;
}

static int h_mqtt_connect(struct http_client_ctx *client,
			  enum http_transaction_status status,
			  const struct http_request_ctx *req,
			  struct http_response_ctx *rsp, void *user_data)
{
	int rc;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	ARG_UNUSED(req);

	rc = mqttc_running() ? 0 : mqttc_start();
	if (rc == -EINVAL) {
		rsp_err(rsp, HTTP_400_BAD_REQUEST, "broker not configured");
		return 0;
	}
	if (rc != 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "start failed");
		return 0;
	}
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"mqtt/connect\",\"queued\":true}",
		 sizeof("{\"ok\":true,\"cmd\":\"mqtt/connect\",\"queued\":true}") - 1,
		 HTTP_200_OK);
	return 0;
}

static int h_mqtt_clear(struct http_client_ctx *client,
			enum http_transaction_status status,
			const struct http_request_ctx *req,
			struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	ARG_UNUSED(req);

	mqttc_stop();
	(void)cfg_set("mqtt.broker", "");
	(void)cfg_set("mqtt.user", "");
	(void)cfg_set("mqtt.pass", "");
	/* INT/BOOL keys reject "": write the registry defaults instead. */
	(void)cfg_set("mqtt.port", "8883");
	(void)cfg_set("mqtt.auto", "0");
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"mqtt/clear\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"mqtt/clear\"}") - 1, HTTP_200_OK);
	return 0;
}

static int h_mqtt_ca(struct http_client_ctx *client,
		     enum http_transaction_status status,
		     const struct http_request_ctx *req,
		     struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	/* Drain the posted fp form for symmetry with the other handlers. */
	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	acc_reset(&acc);

	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"mqtt/ca\",\"note\":\"embedded CA cert; "
		      "fingerprint ignored\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"mqtt/ca\",\"note\":\"embedded CA cert; "
			"fingerprint ignored\"}") - 1,
		 HTTP_200_OK);
	return 0;
}

/* ------------------------------------------------------------------ */
/* WiFi config (legacy alias over the unified config layer)            */
/* ------------------------------------------------------------------ */

static int h_wifi_config(struct http_client_ctx *client,
			 enum http_transaction_status status,
			 const struct http_request_ctx *req,
			 struct http_response_ctx *rsp, void *user_data)
{
	static struct body_acc acc;
	struct httpd_req r;
	char v[CFG_VAL_MAX];
	bool changed = false;

	ARG_UNUSED(user_data);

	if (!post_body_ready(&acc, status, req, rsp)) {
		return 0;
	}
	r.client = client;
	r.query = req_query(client);
	r.acc = &acc;
	r.json = acc_is_json(&acc);

	if (param_str(&r, "ssid", v, sizeof(v)) && v[0] != '\0') {
		if (cfg_set("wifi.ssid", v) != 0) {
			acc_reset(&acc);
			rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid ssid");
			return 0;
		}
		changed = true;
	}
	/* Empty pass is valid: open network / clear stored password. */
	if (param_str(&r, "pass", v, sizeof(v))) {
		if (cfg_set("wifi.pass", v) != 0) {
			acc_reset(&acc);
			rsp_err(rsp, HTTP_400_BAD_REQUEST, "invalid pass");
			return 0;
		}
		changed = true;
	}
	acc_reset(&acc);

	if (changed) {
		node_cfg_changed("wifi.ssid"); /* kicks wifi_sta_reconnect() */
	}
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"wifi/config\"}",
		 sizeof("{\"ok\":true,\"cmd\":\"wifi/config\"}") - 1, HTTP_200_OK);
	return 0;
}

static void reboot_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("HTTPD: nvs/clear done, rebooting\n");
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_fn);

static int h_nvs_clear(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *req,
		       struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	int rc = cfg_erase_all();

	if (rc != 0) {
		rsp_err(rsp, HTTP_500_INTERNAL_SERVER_ERROR, "erase failed");
		return 0;
	}
	k_work_schedule(&reboot_work, K_SECONDS(1));
	rsp_take(rsp, "{\"ok\":true,\"cmd\":\"nvs/clear\",\"restarting\":true}",
		 sizeof("{\"ok\":true,\"cmd\":\"nvs/clear\",\"restarting\":true}") - 1,
		 HTTP_200_OK);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 404 fallback                                                        */
/* ------------------------------------------------------------------ */

static int h_not_found(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *req,
		       struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp_err(rsp, HTTP_404_NOT_FOUND, "not found");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Service + resource registration                                     */
/* ------------------------------------------------------------------ */

#define DYN_DETAIL(_name, _methods, _cb)                                                          \
	static struct http_resource_detail_dynamic _name = {                                   \
		.common = {                                                                    \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                    \
			.bitmask_of_supported_http_methods = (_methods),                       \
			.content_type = "application/json",                                    \
		},                                                                             \
		.cb = (_cb),                                                                   \
	}

DYN_DETAIL(status_detail, BIT(HTTP_GET), h_status);
DYN_DETAIL(help_detail, BIT(HTTP_GET), h_help);
DYN_DETAIL(ability_detail, BIT(HTTP_GET), h_ability);
DYN_DETAIL(at_detail, BIT(HTTP_POST), h_at);
DYN_DETAIL(kbd_tap_detail, BIT(HTTP_POST), h_kbd_tap);
DYN_DETAIL(kbd_text_detail, BIT(HTTP_POST), h_kbd_text);
DYN_DETAIL(kbd_key_detail, BIT(HTTP_POST), h_kbd_key);
DYN_DETAIL(gpio_write_detail, BIT(HTTP_POST), h_gpio_write);
DYN_DETAIL(gpio_read_detail, BIT(HTTP_POST), h_gpio_read);
DYN_DETAIL(adc_read_detail, BIT(HTTP_POST), h_adc_read);
DYN_DETAIL(i2c_scan_detail, BIT(HTTP_POST), h_i2c_scan);
DYN_DETAIL(i2c_read_detail, BIT(HTTP_POST), h_i2c_read);
DYN_DETAIL(i2c_write_detail, BIT(HTTP_POST), h_i2c_write);
DYN_DETAIL(config_detail, BIT(HTTP_GET) | BIT(HTTP_POST), h_config);
DYN_DETAIL(config_list_detail, BIT(HTTP_GET), h_config_list);
DYN_DETAIL(ble_pair_detail, BIT(HTTP_POST), h_ble_pair);
DYN_DETAIL(ble_status_detail, BIT(HTTP_GET), h_ble_status);
DYN_DETAIL(ble_bonds_delete_detail, BIT(HTTP_POST), h_ble_bonds_delete);
DYN_DETAIL(ble_bonds_clear_detail, BIT(HTTP_POST), h_ble_bonds_clear);
DYN_DETAIL(mqtt_status_detail, BIT(HTTP_GET), h_mqtt_status);
DYN_DETAIL(mqtt_config_detail, BIT(HTTP_POST), h_mqtt_config);
DYN_DETAIL(mqtt_connect_detail, BIT(HTTP_POST), h_mqtt_connect);
DYN_DETAIL(mqtt_clear_detail, BIT(HTTP_POST), h_mqtt_clear);
DYN_DETAIL(mqtt_ca_detail, BIT(HTTP_POST), h_mqtt_ca);
DYN_DETAIL(wifi_config_detail, BIT(HTTP_POST), h_wifi_config);
DYN_DETAIL(nvs_clear_detail, BIT(HTTP_POST), h_nvs_clear);
DYN_DETAIL(not_found_detail,
	   BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_PUT) | BIT(HTTP_DELETE) |
	   BIT(HTTP_PATCH) | BIT(HTTP_OPTIONS),
	   h_not_found);

/* SPA: single gzipped page straight from flash. */
static struct http_resource_detail_static spa_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = WEB_PAGE_GZ,
	.static_data_len = sizeof(WEB_PAGE_GZ),
};

static uint16_t httpd_port = HTTPD_PORT;

HTTP_SERVICE_DEFINE(atnode_http_service, NULL, &httpd_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, NULL,
		    &not_found_detail.common, NULL);

HTTP_RESOURCE_DEFINE(spa_resource, atnode_http_service, "/", &spa_detail);
HTTP_RESOURCE_DEFINE(status_resource, atnode_http_service, "/at-node/cmd/status",
		     &status_detail);
HTTP_RESOURCE_DEFINE(help_resource, atnode_http_service, "/at-node/help.json",
		     &help_detail);
HTTP_RESOURCE_DEFINE(ability_resource, atnode_http_service, "/at-node/cmd/ability",
		     &ability_detail);
HTTP_RESOURCE_DEFINE(at_resource, atnode_http_service, "/at-node/at", &at_detail);
HTTP_RESOURCE_DEFINE(kbd_tap_resource, atnode_http_service, "/at-node/cmd/keyboard/tap",
		     &kbd_tap_detail);
HTTP_RESOURCE_DEFINE(kbd_text_resource, atnode_http_service, "/at-node/cmd/keyboard/text",
		     &kbd_text_detail);
HTTP_RESOURCE_DEFINE(kbd_key_resource, atnode_http_service, "/at-node/cmd/keyboard/key",
		     &kbd_key_detail);
HTTP_RESOURCE_DEFINE(gpio_write_resource, atnode_http_service, "/at-node/cmd/gpio/write",
		     &gpio_write_detail);
HTTP_RESOURCE_DEFINE(gpio_read_resource, atnode_http_service, "/at-node/cmd/gpio/read",
		     &gpio_read_detail);
HTTP_RESOURCE_DEFINE(adc_read_resource, atnode_http_service, "/at-node/cmd/adc/read",
		     &adc_read_detail);
HTTP_RESOURCE_DEFINE(i2c_scan_resource, atnode_http_service, "/at-node/cmd/i2c/scan",
		     &i2c_scan_detail);
HTTP_RESOURCE_DEFINE(i2c_read_resource, atnode_http_service, "/at-node/cmd/i2c/read",
		     &i2c_read_detail);
HTTP_RESOURCE_DEFINE(i2c_write_resource, atnode_http_service, "/at-node/cmd/i2c/write",
		     &i2c_write_detail);
HTTP_RESOURCE_DEFINE(config_resource, atnode_http_service, "/at-node/cmd/config",
		     &config_detail);
HTTP_RESOURCE_DEFINE(config_list_resource, atnode_http_service, "/at-node/cmd/config/list",
		     &config_list_detail);
HTTP_RESOURCE_DEFINE(ble_pair_resource, atnode_http_service, "/at-node/cmd/ble/pair",
		     &ble_pair_detail);
HTTP_RESOURCE_DEFINE(ble_status_resource, atnode_http_service,
		     "/at-node/cmd/ble/status", &ble_status_detail);
HTTP_RESOURCE_DEFINE(ble_bonds_delete_resource, atnode_http_service,
		     "/at-node/cmd/ble/bonds/delete", &ble_bonds_delete_detail);
HTTP_RESOURCE_DEFINE(ble_bonds_clear_resource, atnode_http_service,
		     "/at-node/cmd/ble/bonds/clear", &ble_bonds_clear_detail);
HTTP_RESOURCE_DEFINE(mqtt_status_resource, atnode_http_service,
		     "/at-node/cmd/mqtt/status", &mqtt_status_detail);
HTTP_RESOURCE_DEFINE(mqtt_config_resource, atnode_http_service,
		     "/at-node/cmd/mqtt/config", &mqtt_config_detail);
HTTP_RESOURCE_DEFINE(mqtt_connect_resource, atnode_http_service,
		     "/at-node/cmd/mqtt/connect", &mqtt_connect_detail);
HTTP_RESOURCE_DEFINE(mqtt_clear_resource, atnode_http_service,
		     "/at-node/cmd/mqtt/clear", &mqtt_clear_detail);
HTTP_RESOURCE_DEFINE(mqtt_ca_resource, atnode_http_service, "/at-node/cmd/mqtt/ca",
		     &mqtt_ca_detail);
HTTP_RESOURCE_DEFINE(wifi_config_resource, atnode_http_service,
		     "/at-node/cmd/wifi/config", &wifi_config_detail);
HTTP_RESOURCE_DEFINE(nvs_clear_resource, atnode_http_service, "/at-node/cmd/nvs/clear",
		     &nvs_clear_detail);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static bool httpd_up;

int httpd_start(void)
{
	int rc = http_server_start();

	if (rc == 0 || rc == -EALREADY) {
		httpd_up = true;
		printk("HTTPD: server started (port %u)\n", (unsigned int)httpd_port);
		return 0;
	}
	printk("HTTPD: start failed (%d)\n", rc);
	return rc;
}

void httpd_stop(void)
{
	if (!httpd_up) {
		return;
	}
	(void)http_server_stop();
	httpd_up = false;
	printk("HTTPD: stopped\n");
}

bool httpd_running(void)
{
	return httpd_up;
}
