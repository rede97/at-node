/*
 * AT-Node Zephyr — MQTT (TLS) remote control plane.
 *
 * Broker/credentials from cfg registry (mqtt.*); CA cert embedded at build
 * time via src/ca_cert.h (tools/gen_certs.sh), same scheme as the
 * nano_esp32s3_demo reference.
 *
 * Topics (hostname = cfg device.name):
 *   sub  atnode/<hostname>/cmd     payload: raw AT line ("AT+TAP=100,0,4")
 *   pub  atnode/<hostname>/resp    AT response (lines joined by \n)
 *   pub  atnode/<hostname>/state   retained "online"/"offline" (LWT)
 *   pub  atnode/<hostname>/info    retained JSON manifest (name, ver, ability)
 *
 * Runs in its own thread with auto-reconnect; never blocks AT/HTTP.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

int  mqttc_start(void); /* reads cfg mqtt.*; -EINVAL when broker unset */
void mqttc_stop(void);
bool mqttc_running(void);
bool mqttc_connected(void);
