/*
 * AT-Node Zephyr — WiFi STA with reconnect watchdog (15 s retry loop,
 * mirroring the Arduino variant's WiFi watchdog).
 *
 * Credentials come from the cfg registry (wifi.ssid/wifi.pass); the
 * watchdog thread (re)connects whenever the link is down and creds exist.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

int  wifi_sta_init(void);          /* spawns watchdog thread; reads cfg */
void wifi_sta_reconnect(void);     /* kick reconnect after cfg change */
bool wifi_sta_is_up(void);
int  wifi_sta_rssi(void);          /* dBm, 0 when unknown */
void wifi_sta_ip_str(char *buf, unsigned int len); /* "" when down */
void wifi_sta_mac_str(char *buf, unsigned int len);
