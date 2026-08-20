/*
 * AT-Node Zephyr — WiFi STA with reconnect watchdog.
 *
 * Watchdog thread: while creds exist and link is down, (re)issue
 * NET_REQUEST_WIFI_CONNECT every 15 s (Arduino loop() parity). When the
 * link drops, LED goes back to slow-blink blue; green when IPv4 is up.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>

#include "cfg.h"
#include "led.h"
#include "wifi_sta.h"

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static volatile bool link_up;
static volatile bool ipv4_up;
static volatile int last_rssi;

static K_SEM_DEFINE(reconnect_sem, 0, 1);

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event, struct net_if *iface)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		if (status->status == 0) {
			printk("WIFI: connected\n");
			link_up = true;
		} else {
			printk("WIFI: connect failed, status %d\n", status->status);
			link_up = false;
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		printk("WIFI: disconnected\n");
		link_up = false;
		ipv4_up = false;
		led_status(LED_WIFI_CONNECTING);
		break;
	default:
		break;
	}
}

static void ipv4_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		char buf[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET,
			      &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
			      buf, sizeof(buf));
		printk("NET: IPv4 address %s\n", buf);
		ipv4_up = true;
		led_status(LED_ONLINE);
	}
}

static int wifi_connect_once(void)
{
	char ssid[CFG_VAL_MAX];
	char pass[CFG_VAL_MAX];

	cfg_get_str("wifi.ssid", ssid, sizeof(ssid), "");
	cfg_get_str("wifi.pass", pass, sizeof(pass), "");
	if (ssid[0] == '\0') {
		return -ENOENT;
	}

	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = strlen(ssid),
		.psk = (const uint8_t *)pass,
		.psk_length = strlen(pass),
		.security = pass[0] ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.channel = WIFI_CHANNEL_ANY,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	printk("WIFI: connecting to %s ...\n", ssid);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params, sizeof(params));
}

static void poll_rssi(void)
{
	struct wifi_iface_status st = { 0 };

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface, &st, sizeof(st)) == 0 &&
	    st.state >= WIFI_STATE_ASSOCIATED) {
		last_rssi = st.rssi;
	}
}

static void watchdog_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	sta_iface = net_if_get_default();
	if (sta_iface == NULL) {
		printk("WIFI: no default interface\n");
		led_status(LED_ERROR);
		return;
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_mgmt_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	led_status(LED_WIFI_CONNECTING);

	while (1) {
		k_sem_take(&reconnect_sem, K_SECONDS(15));

		if (!ipv4_up) {
			int rc = wifi_connect_once();

			if (rc == -ENOENT) {
				continue; /* no creds yet: wait for AT+SET */
			}
			if (rc != 0) {
				printk("WIFI: connect request failed %d\n", rc);
			}
		} else {
			poll_rssi();
		}
	}
}

K_THREAD_DEFINE(wifi_wd, 3072, watchdog_thread, NULL, NULL, NULL, 7, 0, 0);

int wifi_sta_init(void)
{
	/* thread is static; nothing else to do */
	return 0;
}

void wifi_sta_reconnect(void)
{
	struct net_if *iface = sta_iface;

	if (iface != NULL && link_up) {
		net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	}
	link_up = false;
	ipv4_up = false;
	led_status(LED_WIFI_CONNECTING);
	k_sem_give(&reconnect_sem);
}

bool wifi_sta_is_up(void)
{
	return ipv4_up;
}

int wifi_sta_rssi(void)
{
	return last_rssi;
}

void wifi_sta_ip_str(char *buf, unsigned int len)
{
	if (!ipv4_up || sta_iface == NULL) {
		buf[0] = '\0';
		return;
	}
	net_addr_ntop(AF_INET,
		      &sta_iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
		      buf, len);
}

void wifi_sta_mac_str(char *buf, unsigned int len)
{
	struct net_linkaddr *la;

	if (sta_iface == NULL) {
		buf[0] = '\0';
		return;
	}
	la = &sta_iface->if_dev->link_addr;
	snprintk(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
		 la->addr[0], la->addr[1], la->addr[2],
		 la->addr[3], la->addr[4], la->addr[5]);
}
