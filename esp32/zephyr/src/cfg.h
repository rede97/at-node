/*
 * AT-Node Zephyr — persistent config registry (settings/NVS backed).
 *
 * Single entry point for all persistent config, mirroring the Arduino
 * variant's config_set/get/list registry. Key space (v1, no tunnel/IR):
 *
 *   device.name        BLE hostname + MQTT client prefix (default AT-Node-S3-XXXX)
 *   wifi.ssid          write-only via cfg_get (secret)
 *   wifi.pass          write-only (secret)
 *   mqtt.broker        host/IP string
 *   mqtt.port          int, default 8883
 *   mqtt.user          string (may be empty)
 *   mqtt.pass          write-only (secret)
 *   mqtt.auto          bool, auto-connect at boot (persisted)
 *   mqtt.enable        bool, runtime start/stop (persisted, unlike Arduino RAM-only)
 *   http.auto          bool
 *   http.enable        bool
 *   ble.auto           bool
 *   ble.enable         bool
 *
 * cfg_set() persists immediately. Services are notified through
 * node_cfg_changed() (main.c) — cfg.c itself has no service deps.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CFG_VAL_MAX 64 /* max value length (SSID/pass/broker host) */

/* key, value (NULL when write-only secret), write_only flag, ctx */
typedef void (*cfg_list_cb)(const char *key, const char *val, bool write_only,
			    void *ctx);

int  cfg_init(void); /* settings_subsys_init + load; once at boot */
int  cfg_set(const char *key, const char *val);
/* 0 ok, -ENOENT unknown key, -EINVAL bad value, -EACCES write-only (get) */
int  cfg_get(const char *key, char *buf, size_t len);
void cfg_list(cfg_list_cb cb, void *ctx);

/* Convenience typed readers (return dflt when unset/invalid) */
bool cfg_get_bool(const char *key, bool dflt);
void cfg_get_str(const char *key, char *buf, size_t len, const char *dflt);
int  cfg_get_int(const char *key, int dflt);

/* True when key exists in the registry (for AT+KEYS / unknown-key errors) */
bool cfg_key_exists(const char *key);

/* Erase all persisted settings (factory reset); caller reboots after */
int  cfg_erase_all(void);

/* Implemented in main.c: fan-out service notification after cfg_set().
 * Called by at_core on every successful cfg_set. Must be cheap/non-blocking.
 */
void node_cfg_changed(const char *key);
