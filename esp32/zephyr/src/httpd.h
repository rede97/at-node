/*
 * AT-Node Zephyr — HTTP control plane (/at-node routes) + shared gzip SPA.
 *
 * Routes mirror the Arduino variant (PLAN.md §3):
 *   GET  /                              -> SPA (gzip, single response)
 *   GET  /at-node/cmd/status            -> JSON status
 *   GET  /at-node/help.json             -> machine-readable API catalog
 *   GET  /at-node/cmd/ability           -> feature flags
 *   POST /at-node/at                    -> raw AT line (text/plain body)
 *   POST /at-node/cmd/keyboard/{tap,text,key}
 *   POST /at-node/cmd/gpio/{write,read}
 *   POST /at-node/cmd/adc/read
 *   POST /at-node/cmd/i2c/{scan,read,write}
 *   GET|POST /at-node/cmd/config[?key=&val=]  + GET .../config/list
 *   POST /at-node/cmd/ble/pair?enable=1
 *   POST /at-node/cmd/nvs/clear         -> factory reset + reboot
 * Unknown path -> {"ok":false,"error":"not found"}
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

int  httpd_start(void); /* bind :80; safe to call before WiFi is up */
void httpd_stop(void);
bool httpd_running(void);
