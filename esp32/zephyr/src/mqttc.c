/*
 * AT-Node Zephyr — MQTT (TLS) remote control plane.
 *
 * Port of the nano_esp32s3_demo MQTT/TLS pattern (see its docs/DEBUGGING.md):
 *  - prepare_fds() is called only AFTER mqtt_connect() created the socket;
 *  - publish payloads are read with mqtt_read_publish_payload(MIN(len, buf));
 *  - port 8883 -> TLS + CA verify, port 1883 -> plain TCP (anything else: TLS);
 *  - rx/tx buffers 2048, AT payload buffer 512 (longer lines truncated).
 *
 * Thread model: one k_thread runs the poll/mqtt_input/mqtt_live loop with a
 * 5 s reconnect backoff; mqttc_stop() sets a flag, aborts the session and
 * joins the thread. WiFi outages just make the loop wait for the link.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/poll.h>
#include <zephyr/sys/printk.h>

#include "at_core.h"
#include "cfg.h"
#include "mqttc.h"
#include "wifi_sta.h"

/* CA cert is embedded at build time by tools/gen_certs.sh; the build must
 * succeed without it (TLS then runs with peer verification disabled).
 */
#if __has_include("ca_cert.h")
#include "ca_cert.h"
#define HAVE_CA_CERT 1
#else
#define HAVE_CA_CERT 0
#endif

#define CA_CERT_TAG 12 /* same tag as the demo */

#define STACK_SIZE   4096
#define THREAD_PRIO  7 /* same as the wifi watchdog */

#define RECONNECT_BACKOFF_MS 5000
#define WIFI_WAIT_MS         1000

#define RX_BUF_SIZE      2048
#define TX_BUF_SIZE      2048
#define PAYLOAD_BUF_SIZE 512
#define RESP_BUF_SIZE    1024

#define TOPIC_BUF_SIZE (CFG_VAL_MAX + 16) /* "atnode/" + name + "/state" */

#define FW_VER "AT-Node v1.0 [zephyr-s3]"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct mqtt_client client;
static struct sockaddr_storage broker_addr;

static uint8_t rx_buffer[RX_BUF_SIZE];
static uint8_t tx_buffer[TX_BUF_SIZE];
static uint8_t payload_buf[PAYLOAD_BUF_SIZE];
static char resp_buf[RESP_BUF_SIZE];

/* config snapshot taken by mqttc_start() */
static char broker_host[CFG_VAL_MAX];
static int  broker_port;
static bool use_tls;
static char mqtt_user[CFG_VAL_MAX];
static char mqtt_pass[CFG_VAL_MAX];
static char device_name[CFG_VAL_MAX];

static char topic_cmd[TOPIC_BUF_SIZE];
static char topic_resp[TOPIC_BUF_SIZE];
static char topic_state[TOPIC_BUF_SIZE];
static char topic_info[TOPIC_BUF_SIZE];

static struct zsock_pollfd fds[1];
static int nfds;

static volatile bool started;
static volatile bool run_flag;
static volatile bool connected;

static struct k_thread thread_data;
static K_THREAD_STACK_DEFINE(thread_stack, STACK_SIZE);
static K_SEM_DEFINE(stop_sem, 0, 1); /* poked by mqttc_stop() to cut waits */

static uint16_t next_msg_id(void)
{
	static uint16_t id;

	id++;
	if (id == 0) {
		id = 1;
	}
	return id;
}

/* ------------------------------------------------------------------ */
/* Broker address (IP literal fast path, DNS via CONFIG_DNS_RESOLVER)  */
/* ------------------------------------------------------------------ */

static int resolve_broker(const char *host, uint16_t port)
{
	struct sockaddr_in *a = (struct sockaddr_in *)&broker_addr;

	memset(&broker_addr, 0, sizeof(broker_addr));

	if (zsock_inet_pton(AF_INET, host, &a->sin_addr) == 1) {
		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		return 0;
	}

	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = NET_SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	int rc = zsock_getaddrinfo(host, NULL, &hints, &res);

	if (rc != 0 || res == NULL) {
		return (rc != 0) ? rc : -EHOSTUNREACH;
	}
	memcpy(&broker_addr, res->ai_addr,
	       MIN((size_t)res->ai_addrlen, sizeof(broker_addr)));
	zsock_freeaddrinfo(res);

	a = (struct sockaddr_in *)&broker_addr;
	a->sin_port = htons(port);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Publish helpers                                                     */
/* ------------------------------------------------------------------ */

static int publish_str(const char *topic, const char *payload, bool retain)
{
	struct mqtt_publish_param param;

	memset(&param, 0, sizeof(param));
	param.message.topic.topic.utf8 = (const uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.payload.data = (void *)payload; /* lib does not write it */
	param.message.payload.len = strlen(payload);
	param.message_id = 0; /* QoS 0: id unused */
	param.dup_flag = 0;
	param.retain_flag = retain ? 1 : 0;

	return mqtt_publish(&client, &param);
}

/* retained JSON manifest: {"name":..,"ver":..,"ip":..,"ability":{...}} */
static void publish_info(void)
{
	char ip[NET_IPV4_ADDR_LEN];
	char info[320];

	wifi_sta_ip_str(ip, sizeof(ip));

	snprintk(info, sizeof(info),
		 "{\"name\":\"%s\",\"ver\":\"%s\",\"ip\":\"%s\","
		 "\"ability\":{\"ble\":%s,\"mqtt\":true,\"rathole\":false,"
		 "\"i2c\":%s,\"http\":%s,\"breath_led\":false}}",
		 device_name, FW_VER, ip,
		 IS_ENABLED(CONFIG_BT) ? "true" : "false",
		 IS_ENABLED(CONFIG_I2C) ? "true" : "false",
		 IS_ENABLED(CONFIG_HTTP_SERVER) ? "true" : "false");

	if (publish_str(topic_info, info, true) != 0) {
		printk("MQTT: info publish failed\n");
	}
}

static void subscribe_cmd(struct mqtt_client *c)
{
	struct mqtt_topic topic = {
		.topic = {
			.utf8 = (const uint8_t *)topic_cmd,
			.size = strlen(topic_cmd),
		},
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	};
	struct mqtt_subscription_list sub = {
		.list = &topic,
		.list_count = 1,
		.message_id = next_msg_id(),
	};

	if (mqtt_subscribe(c, &sub) == 0) {
		printk("MQTT: subscribed to %s\n", topic_cmd);
	} else {
		printk("MQTT: subscribe failed\n");
	}
}

/* ------------------------------------------------------------------ */
/* cmd topic -> AT core -> resp topic                                  */
/* ------------------------------------------------------------------ */

static void handle_cmd(char *line, int len)
{
	/* strip trailing CR/LF the publisher may have included */
	while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
		line[--len] = '\0';
	}
	if (len == 0) {
		return;
	}

	printk("MQTT: AT <- %s\n", line);

	len = at_handle_collect(line, resp_buf, sizeof(resp_buf));
	if (len <= 0) {
		return;
	}
	/* publish without the trailing newline of the final OK/ERROR line */
	while (len > 0 && resp_buf[len - 1] == '\n') {
		resp_buf[--len] = '\0';
	}

	if (publish_str(topic_resp, resp_buf, false) != 0) {
		printk("MQTT: resp publish failed\n");
	}
}

/* ------------------------------------------------------------------ */
/* Event handler (always runs on the mqtt thread via mqtt_input)       */
/* ------------------------------------------------------------------ */

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			printk("MQTT: connect refused, result %d\n", evt->result);
			break;
		}
		connected = true;
		printk("MQTT: connected to %s:%d (%s)\n", broker_host, broker_port,
		       use_tls ? "TLS" : "plain");

		subscribe_cmd(c);
		publish_str(topic_state, "online", true);
		publish_info();
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;
		uint32_t plen = pub->message.payload.len;
		uint32_t want = MIN(plen, (uint32_t)sizeof(payload_buf) - 1);
		uint32_t left;
		int rc;

		/* mqtt_read_publish_payload(buf, n): read n payload bytes;
		 * reading more than the payload yields 0/-EIO (DEBUGGING §6.3)
		 */
		rc = mqtt_read_publish_payload(c, payload_buf, want);
		if (rc < 0) {
			printk("MQTT: payload read error %d\n", rc);
			break;
		}
		payload_buf[rc] = '\0';

		/* drain the rest of an oversized payload to keep the
		 * stream aligned for the next frame
		 */
		left = plen - (uint32_t)rc;
		while (left > 0) {
			uint8_t sink[32];
			int d = mqtt_read_publish_payload(
				c, sink, MIN(left, (uint32_t)sizeof(sink)));

			if (d <= 0) {
				break;
			}
			left -= (uint32_t)d;
		}
		if (plen > want) {
			printk("MQTT: oversized AT line truncated to %u bytes\n",
			       want);
		}

		if (pub->message.topic.topic.size == strlen(topic_cmd) &&
		    strncmp(pub->message.topic.topic.utf8, topic_cmd,
			    strlen(topic_cmd)) == 0) {
			handle_cmd((char *)payload_buf, rc);
		}
		break;
	}

	case MQTT_EVT_DISCONNECT:
		connected = false;
		printk("MQTT: disconnected, reason %d\n", evt->result);
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Connect                                                             */
/* ------------------------------------------------------------------ */

/* socket is only created by mqtt_connect(); poll on it after (§6.2) */
static int prepare_fds(struct mqtt_client *c)
{
	if (c->transport.type == MQTT_TRANSPORT_SECURE) {
		fds[0].fd = c->transport.tls.sock;
	} else {
		fds[0].fd = c->transport.tcp.sock;
	}
	fds[0].events = ZSOCK_POLLIN;
	nfds = 1;
	return 0;
}

static void client_init(void)
{
	static struct mqtt_topic will_topic;
	static struct mqtt_utf8 will_message;
	static struct mqtt_utf8 user_name;
	static struct mqtt_utf8 password;
#if HAVE_CA_CERT
	static const sec_tag_t sec_tags[] = { CA_CERT_TAG };
#endif

	mqtt_client_init(&client); /* zeroes the whole struct */

	client.broker = &broker_addr;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (const uint8_t *)device_name;
	client.client_id.size = strlen(device_name);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

	/* LWT: retained "offline" on the state topic */
	will_topic.topic.utf8 = (const uint8_t *)topic_state;
	will_topic.topic.size = strlen(topic_state);
	will_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	will_message.utf8 = (const uint8_t *)"offline";
	will_message.size = strlen("offline");
	client.will_topic = &will_topic;
	client.will_message = &will_message;
	client.will_retain = 1;

	client.user_name = NULL;
	client.password = NULL;
	if (mqtt_user[0] != '\0') {
		user_name.utf8 = (const uint8_t *)mqtt_user;
		user_name.size = strlen(mqtt_user);
		client.user_name = &user_name;
	}
	if (mqtt_pass[0] != '\0') {
		password.utf8 = (const uint8_t *)mqtt_pass;
		password.size = strlen(mqtt_pass);
		client.password = &password;
	}

	if (use_tls) {
		struct mqtt_sec_config *tls_cfg = &client.transport.tls.config;

		client.transport.type = MQTT_TRANSPORT_SECURE;
		memset(tls_cfg, 0, sizeof(*tls_cfg));
		tls_cfg->cipher_list = NULL;
		tls_cfg->hostname = broker_host; /* SNI + cert verify */
#if HAVE_CA_CERT
		tls_cfg->peer_verify = TLS_PEER_VERIFY_REQUIRED;
		tls_cfg->sec_tag_list = sec_tags;
		tls_cfg->sec_tag_count = ARRAY_SIZE(sec_tags);
#else
		tls_cfg->peer_verify = TLS_PEER_VERIFY_NONE;
		tls_cfg->sec_tag_list = NULL;
		tls_cfg->sec_tag_count = 0;
#endif
	} else {
		client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	}
}

static int mqtt_try_connect(void)
{
	int rc;

	rc = resolve_broker(broker_host, (uint16_t)broker_port);
	if (rc != 0) {
		printk("MQTT: resolve %s failed %d\n", broker_host, rc);
		return rc;
	}

	client_init();

	rc = mqtt_connect(&client);
	if (rc == 0) {
		prepare_fds(&client);
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

static void wait_or_stop(int ms)
{
	k_sem_reset(&stop_sem);
	k_sem_take(&stop_sem, K_MSEC(ms));
}

static void mqtt_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	printk("MQTT: connecting to %s:%d (%s)\n", broker_host, broker_port,
	       use_tls ? "TLS" : "plain");

	while (run_flag) {
		if (!wifi_sta_is_up()) {
			wait_or_stop(WIFI_WAIT_MS);
			continue;
		}

		if (!connected) {
			int rc = mqtt_try_connect();

			if (rc != 0) {
				printk("MQTT: connect error %d, retry in %ds\n",
				       rc, RECONNECT_BACKOFF_MS / 1000);
				wait_or_stop(RECONNECT_BACKOFF_MS);
				continue;
			}
		}

		int timeout = MIN(mqtt_keepalive_time_left(&client), 1000);
		int rc = zsock_poll(fds, nfds, timeout);

		if (rc < 0) {
			printk("MQTT: poll error %d\n", errno);
		}
		if (rc > 0 && (fds[0].revents & ZSOCK_POLLIN)) {
			mqtt_input(&client);
		}
		if (rc > 0 &&
		    (fds[0].revents &
		     (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
			printk("MQTT: socket event 0x%x, dropping\n",
			       fds[0].revents);
			connected = false;
		}

		if (!connected) {
			mqtt_abort(&client);
			if (run_flag) {
				wait_or_stop(RECONNECT_BACKOFF_MS);
			}
			continue;
		}

		rc = mqtt_live(&client);
		if (rc != 0 && rc != -EAGAIN) {
			printk("MQTT: mqtt_live error %d\n", rc);
		}
	}

	/* graceful exit: retained "offline" + DISCONNECT, then drop socket */
	if (connected) {
		publish_str(topic_state, "offline", true);
		mqtt_disconnect(&client, NULL);
		connected = false;
	}
	mqtt_abort(&client);
	printk("MQTT: thread stopped\n");
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int mqttc_start(void)
{
	if (started) {
		return 0;
	}

	cfg_get_str("mqtt.broker", broker_host, sizeof(broker_host), "");
	if (broker_host[0] == '\0') {
		printk("MQTT: mqtt.broker not set "
		       "(AT+SET mqtt.broker=<host>), not starting\n");
		return -EINVAL;
	}

	broker_port = cfg_get_int("mqtt.port", 8883);
	if (broker_port <= 0 || broker_port > 65535) {
		printk("MQTT: invalid mqtt.port %d\n", broker_port);
		return -EINVAL;
	}
	use_tls = (broker_port != 1883); /* 8883/TLS, 1883/plain, else TLS */

	cfg_get_str("mqtt.user", mqtt_user, sizeof(mqtt_user), "");
	cfg_get_str("mqtt.pass", mqtt_pass, sizeof(mqtt_pass), "");
	cfg_get_str("device.name", device_name, sizeof(device_name),
		    "AT-Node-S3");

	snprintk(topic_cmd, sizeof(topic_cmd), "atnode/%s/cmd", device_name);
	snprintk(topic_resp, sizeof(topic_resp), "atnode/%s/resp", device_name);
	snprintk(topic_state, sizeof(topic_state), "atnode/%s/state",
		 device_name);
	snprintk(topic_info, sizeof(topic_info), "atnode/%s/info", device_name);

	if (use_tls) {
#if HAVE_CA_CERT
		int rc = tls_credential_add(CA_CERT_TAG,
					    TLS_CREDENTIAL_CA_CERTIFICATE,
					    ca_certificate,
					    sizeof(ca_certificate));

		if (rc != 0 && rc != -EEXIST) {
			printk("MQTT: failed to register CA cert %d\n", rc);
			return rc;
		}
#else
		printk("MQTT: WARNING: ca_cert.h missing, TLS peer "
		       "verification DISABLED\n");
		printk("MQTT: run tools/gen_certs.sh <broker_ip> to enable "
		       "CA verification\n");
#endif
	}

	connected = false;
	run_flag = true;
	k_sem_reset(&stop_sem);

	k_tid_t tid = k_thread_create(&thread_data, thread_stack,
				      K_THREAD_STACK_SIZEOF(thread_stack),
				      mqtt_thread_fn, NULL, NULL, NULL,
				      THREAD_PRIO, 0, K_NO_WAIT);

	if (tid == NULL) {
		run_flag = false;
		return -ENOMEM;
	}
	k_thread_name_set(tid, "mqttc");
	started = true;
	return 0;
}

void mqttc_stop(void)
{
	if (!started) {
		return;
	}

	run_flag = false;
	k_sem_give(&stop_sem);
	mqtt_abort(&client); /* unblock an in-progress connect/poll */

	if (k_current_get() == &thread_data) {
		/* never join self; the loop exits on its own */
		printk("MQTT: stop from mqtt thread, not joining\n");
		return;
	}

	k_thread_join(&thread_data, K_FOREVER);
	started = false;
	connected = false;
	printk("MQTT: stopped\n");
}

bool mqttc_running(void)
{
	return started;
}

bool mqttc_connected(void)
{
	return connected;
}
