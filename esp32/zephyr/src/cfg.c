/*
 * AT-Node Zephyr — persistent config registry (settings/NVS backed).
 *
 * All keys live under the "atnode" settings subtree, persisted on the
 * NVS "storage_partition". Values are cached in RAM at load/set time so
 * cfg_get*() never touches flash. Secret keys (wifi.pass, mqtt.pass) are
 * write-only through the AT-facing cfg_get()/cfg_list() surface; internal
 * typed readers (cfg_get_str() etc.) return the real value for the
 * WiFi/MQTT services.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include <esp_mac.h>

#include "cfg.h"

#define CFG_HANDLER_NAME "atnode"
#define CFG_NAME_MAX     (SETTINGS_FULL_NAME_LEN)

#define CFG_F_WO   BIT(0) /* write-only secret (cfg_get -> -EACCES) */
#define CFG_F_BOOL BIT(1) /* persisted as "1"/"0" */
#define CFG_F_INT  BIT(2) /* mqtt.port: 1..65535 */

struct cfg_entry {
	const char *name;
	uint8_t flags;
	bool is_set;
	char val[CFG_VAL_MAX];
};

static struct cfg_entry cfg_table[] = {
	{ "device.name",  0,         false, "" },
	{ "wifi.ssid",    0,         false, "" },
	{ "wifi.pass",    CFG_F_WO,  false, "" },
	{ "mqtt.broker",  0,         false, "" },
	{ "mqtt.port",    CFG_F_INT, false, "" },
	{ "mqtt.user",    0,         false, "" },
	{ "mqtt.pass",    CFG_F_WO,  false, "" },
	{ "mqtt.auto",    CFG_F_BOOL, false, "" },
	{ "mqtt.enable",  CFG_F_BOOL, false, "" },
	{ "http.auto",    CFG_F_BOOL, false, "" },
	{ "http.enable",  CFG_F_BOOL, false, "" },
	{ "ble.auto",     CFG_F_BOOL, false, "" },
	{ "ble.enable",   CFG_F_BOOL, false, "" },
};

static K_MUTEX_DEFINE(cfg_lock);
static bool cfg_ready;
static char cfg_def_name[CFG_VAL_MAX]; /* computed device.name default */

static struct cfg_entry *cfg_find(const char *key)
{
	for (size_t i = 0; i < ARRAY_SIZE(cfg_table); i++) {
		if (strcmp(cfg_table[i].name, key) == 0) {
			return &cfg_table[i];
		}
	}
	return NULL;
}

/* "AT-Node-S3-XXXX", XXXX = last 2 MAC bytes (efuse, WiFi not required) */
static void cfg_compute_def_name(void)
{
	uint8_t mac[6] = { 0 };

	if (esp_efuse_mac_get_default(mac) != 0) {
		printk("CFG: efuse MAC read failed, using zero suffix\n");
	}
	snprintk(cfg_def_name, sizeof(cfg_def_name), "AT-Node-S3-%02X%02X",
		 mac[4], mac[5]);
}

/* Registry default for a key ("" when none). Caller must hold cfg_lock. */
static void cfg_default(const struct cfg_entry *e, char *buf, size_t len)
{
	if (strcmp(e->name, "device.name") == 0) {
		snprintk(buf, len, "%s", cfg_def_name);
	} else if (strcmp(e->name, "mqtt.port") == 0) {
		snprintk(buf, len, "8883");
	} else {
		buf[0] = '\0';
	}
}

/* Validate + normalize a new value. Returns 0, -EINVAL on bad value. */
static int cfg_validate(const struct cfg_entry *e, const char *in, char *out,
			size_t out_len)
{
	if ((e->flags & CFG_F_BOOL) != 0) {
		if (strcmp(in, "1") == 0 || strcmp(in, "true") == 0) {
			snprintk(out, out_len, "1");
		} else if (strcmp(in, "0") == 0 || strcmp(in, "false") == 0) {
			snprintk(out, out_len, "0");
		} else {
			return -EINVAL;
		}
		return 0;
	}

	if ((e->flags & CFG_F_INT) != 0) {
		char *end = NULL;
		long v = strtol(in, &end, 10);

		if (end == in || *end != '\0' || v < 1 || v > 65535) {
			return -EINVAL;
		}
		snprintk(out, out_len, "%ld", v);
		return 0;
	}

	if (strlen(in) >= out_len) {
		return -EINVAL;
	}
	snprintk(out, out_len, "%s", in);
	return 0;
}

static int atnode_h_set(const char *key, size_t len, settings_read_cb read_cb,
			void *cb_arg)
{
	struct cfg_entry *e;
	ssize_t got;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	e = cfg_find(key);
	if (e == NULL) {
		/* Unknown key under our subtree: ignore, nothing to consume
		 * (read_cb may be left unread; backend does not care). */
		k_mutex_unlock(&cfg_lock);
		return 0;
	}

	if (len == 0) {
		/* Deleted entry. */
		e->is_set = false;
		e->val[0] = '\0';
		k_mutex_unlock(&cfg_lock);
		return 0;
	}

	if (len >= sizeof(e->val)) {
		printk("CFG: value for %s too long (%u), skipped\n", key, len);
		k_mutex_unlock(&cfg_lock);
		return 0;
	}

	got = read_cb(cb_arg, e->val, len);
	if (got < 0) {
		k_mutex_unlock(&cfg_lock);
		return (int)got;
	}
	e->val[got] = '\0';
	e->is_set = true;
	k_mutex_unlock(&cfg_lock);
	return 0;
}

static struct settings_handler atnode_settings = {
	.name = CFG_HANDLER_NAME,
	.h_set = atnode_h_set,
};

int cfg_init(void)
{
	int rc;

	if (cfg_ready) {
		return 0;
	}

	cfg_compute_def_name();

	rc = settings_subsys_init();
	if (rc != 0) {
		printk("CFG: settings_subsys_init failed (%d)\n", rc);
		return rc;
	}

	rc = settings_register(&atnode_settings);
	if (rc != 0) {
		printk("CFG: settings_register failed (%d)\n", rc);
		return rc;
	}

	rc = settings_load();
	if (rc != 0) {
		printk("CFG: settings_load failed (%d)\n", rc);
		return rc;
	}

	cfg_ready = true;
	printk("CFG: ready, device.name=%s\n", cfg_def_name);
	return 0;
}

int cfg_set(const char *key, const char *val)
{
	struct cfg_entry *e;
	char norm[CFG_VAL_MAX];
	char name[CFG_NAME_MAX];
	int rc;

	if (key == NULL || val == NULL) {
		return -EINVAL;
	}
	if (!cfg_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	e = cfg_find(key);
	if (e == NULL) {
		k_mutex_unlock(&cfg_lock);
		return -ENOENT;
	}

	rc = cfg_validate(e, val, norm, sizeof(norm));
	if (rc != 0) {
		k_mutex_unlock(&cfg_lock);
		return rc;
	}

	snprintk(name, sizeof(name), "%s/%s", CFG_HANDLER_NAME, key);

	if (norm[0] == '\0') {
		/* Empty value clears the key (settings_nvs stores len 0 as
		 * delete anyway); the registry default applies again. */
		rc = settings_delete(name);
		if (rc == 0) {
			e->is_set = false;
			e->val[0] = '\0';
		}
	} else {
		rc = settings_save_one(name, norm, strlen(norm));
		if (rc == 0) {
			snprintk(e->val, sizeof(e->val), "%s", norm);
			e->is_set = true;
		}
	}
	k_mutex_unlock(&cfg_lock);

	if (rc != 0) {
		printk("CFG: persist %s failed (%d)\n", key, rc);
	}
	return rc;
}

int cfg_get(const char *key, char *buf, size_t len)
{
	struct cfg_entry *e;
	const char *src;
	char def[CFG_VAL_MAX];
	int rc = 0;

	if (key == NULL || buf == NULL || len == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	e = cfg_find(key);
	if (e == NULL) {
		rc = -ENOENT;
	} else if ((e->flags & CFG_F_WO) != 0) {
		rc = -EACCES;
	} else {
		if (e->is_set) {
			src = e->val;
		} else {
			cfg_default(e, def, sizeof(def));
			src = def;
		}
		if (strlen(src) + 1 > len) {
			rc = -ERANGE;
		} else {
			snprintk(buf, len, "%s", src);
		}
	}
	k_mutex_unlock(&cfg_lock);
	return rc;
}

void cfg_list(cfg_list_cb cb, void *ctx)
{
	char def[CFG_VAL_MAX];
	const char *src;

	if (cb == NULL) {
		return;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(cfg_table); i++) {
		struct cfg_entry *e = &cfg_table[i];
		bool wo = (e->flags & CFG_F_WO) != 0;

		if (wo) {
			src = NULL;
		} else if (e->is_set) {
			src = e->val;
		} else {
			cfg_default(e, def, sizeof(def));
			src = def;
		}
		cb(e->name, src, wo, ctx);
	}
	k_mutex_unlock(&cfg_lock);
}

bool cfg_get_bool(const char *key, bool dflt)
{
	struct cfg_entry *e;
	bool v = dflt;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	e = cfg_find(key);
	if (e != NULL && e->is_set) {
		v = (strcmp(e->val, "1") == 0 || strcmp(e->val, "true") == 0);
	}
	k_mutex_unlock(&cfg_lock);
	return v;
}

void cfg_get_str(const char *key, char *buf, size_t len, const char *dflt)
{
	struct cfg_entry *e;
	const char *src = dflt != NULL ? dflt : "";
	char def[CFG_VAL_MAX];

	if (key == NULL || buf == NULL || len == 0) {
		return;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	e = cfg_find(key);
	if (e != NULL) {
		if (e->is_set) {
			src = e->val;
		} else {
			cfg_default(e, def, sizeof(def));
			if (def[0] != '\0') {
				src = def;
			}
		}
	}
	snprintk(buf, len, "%s", src);
	k_mutex_unlock(&cfg_lock);
}

int cfg_get_int(const char *key, int dflt)
{
	struct cfg_entry *e;
	int v = dflt;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	e = cfg_find(key);
	if (e != NULL && e->is_set) {
		v = atoi(e->val);
	}
	k_mutex_unlock(&cfg_lock);
	return v;
}

bool cfg_key_exists(const char *key)
{
	return key != NULL && cfg_find(key) != NULL;
}

int cfg_erase_all(void)
{
	char name[CFG_NAME_MAX];
	int rc = 0;

	if (!cfg_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(cfg_table); i++) {
		int rc2;

		snprintk(name, sizeof(name), "%s/%s", CFG_HANDLER_NAME,
			 cfg_table[i].name);
		rc2 = settings_delete(name);
		if (rc2 != 0 && rc == 0) {
			rc = rc2;
		}
		cfg_table[i].is_set = false;
		cfg_table[i].val[0] = '\0';
	}
	k_mutex_unlock(&cfg_lock);

	printk("CFG: erased all settings (%d), reboot required\n", rc);
	return rc;
}
