/*
 * AT-Node Zephyr — BLE HID boot-keyboard backend (kbd.h backend).
 *
 * HoG GATT service ported from zephyr/samples/bluetooth/peripheral_hids
 * (hog.c) down to a pure boot keyboard: no mouse/report-protocol remnants.
 *
 *   HID Information     0x2A4A  READ
 *   Report Map          0x2A4B  READ        (usage page 0x07, 8-byte report)
 *   Protocol Mode       0x2A4E  READ|WRITE_WITHOUT_RESP (default: boot)
 *   Boot KB Input       0x2A22  READ|NOTIFY (encrypted, 8-byte report)
 *   Boot KB Output      0x2A32  READ|WRITE|WRITE_WITHOUT_RESP (encrypted, LEDs)
 *   HID Control Point   0x2A4C  WRITE_WITHOUT_RESP
 *   (BAS and DIS are registered by the stack: CONFIG_BT_BAS / CONFIG_BT_DIS)
 *
 * Pairing policy (Arduino semantics):
 *   - default: no public advertising;
 *   - kbd_ble_pair_open(): 60 s window, public connectable + bondable;
 *   - outside window with bonds: filter accept list (bonded hosts only);
 *   - outside window without bonds: silent.
 * SMP JustWorks (no input no output, no auth callbacks); bondability is
 * gated by the window via bt_set_bondable(). Bonds persist via
 * CONFIG_BT_SETTINGS (settings_load() on first bt_enable()).
 *
 * Locking: ble_lock serializes API callers and workqueue handlers that run
 * adv_update_locked() (synchronous HCI). bt_conn callbacks run on the BT RX
 * thread and NEVER take ble_lock (RX must stay free to deliver the HCI
 * command completions those synchronous calls wait for); the connection
 * table is written only from RX context and read lock-free elsewhere.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/settings/settings.h>

#if defined(CONFIG_BT)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include "cfg.h"
#include "kbd.h"

#define PAIR_WINDOW_SECONDS 60

/* ------------------------------------------------------------------ */
/* HID service (boot keyboard)                                        */
/* ------------------------------------------------------------------ */

enum { HIDS_NORMALLY_CONNECTABLE = BIT(1) };

struct hids_info {
	uint16_t version; /* HID specification version (1.11) */
	uint8_t  code;    /* country code, 0 = not localized */
	uint8_t  flags;
} __packed;

static const struct hids_info hid_info = {
	.version = sys_cpu_to_le16(0x0111),
	.code = 0x00,
	.flags = HIDS_NORMALLY_CONNECTABLE,
};

enum proto_mode { PROTO_BOOT = 0, PROTO_REPORT = 1 };

static uint8_t protocol_mode = PROTO_BOOT;
static uint8_t boot_in_report[8];  /* mods, reserved, 6 keycodes */
static uint8_t boot_out_report;    /* LED bitmask from host */
static uint8_t ctrl_point;

/* Standard HID boot keyboard report map (HID spec E.6):
 * 8-byte input report (modifiers / reserved / 6 keycodes, usage page 0x07)
 * plus 1-byte LED output report.
 */
static const uint8_t report_map[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0xE0, /*   Usage Minimum (Left Control) */
	0x29, 0xE7, /*   Usage Maximum (Right GUI) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data,Var,Abs) - modifier byte */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x08, /*   Report Size (8) */
	0x81, 0x01, /*   Input (Const,Arr,Abs) - reserved byte */
	0x95, 0x05, /*   Report Count (5) */
	0x75, 0x01, /*   Report Size (1) */
	0x05, 0x08, /*   Usage Page (LEDs) */
	0x19, 0x01, /*   Usage Minimum (Num Lock) */
	0x29, 0x05, /*   Usage Maximum (Kana) */
	0x91, 0x02, /*   Output (Data,Var,Abs) - LED report */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x03, /*   Report Size (3) */
	0x91, 0x01, /*   Output (Const,Arr,Abs) - LED padding */
	0x95, 0x06, /*   Report Count (6) */
	0x75, 0x08, /*   Report Size (8) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x65, /*   Logical Maximum (101) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0x00, /*   Usage Minimum (0) */
	0x29, 0x65, /*   Usage Maximum (101) */
	0x81, 0x00, /*   Input (Data,Arr,Abs) - 6 keycodes */
	0xC0,       /* End Collection */
};

static ssize_t read_hids_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(struct hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, report_map,
				 sizeof(report_map));
}

static ssize_t read_proto_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &protocol_mode,
				 sizeof(protocol_mode));
}

static ssize_t write_proto_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset,
				uint8_t flags)
{
	uint8_t val = *((const uint8_t *)buf);

	if (len != 1 || val > PROTO_REPORT) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	protocol_mode = val;
	printk("KBD_BLE: protocol mode %s\n", val ? "report" : "boot");
	return len;
}

static ssize_t read_boot_in(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, boot_in_report,
				 sizeof(boot_in_report));
}

static void boot_in_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("KBD_BLE: boot input notify %s\n",
	       (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

static ssize_t read_boot_out(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &boot_out_report,
				 sizeof(boot_out_report));
}

static ssize_t write_boot_out(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset,
			      uint8_t flags)
{
	if (len != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	boot_out_report = *((const uint8_t *)buf);
	printk("KBD_BLE: LED output report 0x%02x (num=%u caps=%u scroll=%u)\n",
	       boot_out_report, !!(boot_out_report & BIT(0)), !!(boot_out_report & BIT(1)),
	       !!(boot_out_report & BIT(2)));
	return len;
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset,
				uint8_t flags)
{
	ctrl_point = *((const uint8_t *)buf);
	printk("KBD_BLE: HID control point %u (%s)\n", ctrl_point,
	       ctrl_point ? "exit suspend" : "suspend");
	return len;
}

/* Encryption required for report access/CCC (hog.c SAMPLE_BT_PERM_*); the
 * informational characteristics stay readable on an unencrypted link.
 */
BT_GATT_SERVICE_DEFINE(hids_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_hids_info, NULL,
			       (void *)&hid_info),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_report_map, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_proto_mode, write_proto_mode, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_IN_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_boot_in, NULL, NULL),
	BT_GATT_CCC(boot_in_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_OUT_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
			       read_boot_out, write_boot_out, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_ctrl_point, NULL),
);

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static K_MUTEX_DEFINE(ble_lock);

static bool bt_inited;    /* bt_enable() + settings_load() done */
static bool ble_started;  /* kbd_ble_start() without matching stop */
static bool pair_window;  /* public 60 s pairing window open */
static bool adv_active;   /* connectable advertising actually running */

/* Written only from BT RX context, read lock-free from other threads. */
static struct bt_conn *conns[CONFIG_BT_MAX_CONN];

/* Disconnected connections pending bt_conn_unref(). The unref runs only in
 * adv_dwork_fn() under ble_lock, and all other-thread users of conns[]
 * (send/stop/clear_bonds) hold ble_lock too, so a conn pointer can never
 * be freed while in use. RX context only ever moves the pointer here.
 */
static struct bt_conn *graveyard[2 * CONFIG_BT_MAX_CONN];

static const struct bt_gatt_attr *boot_in_attr;

static char ble_name[CONFIG_BT_DEVICE_NAME_MAX];
static struct bt_data sd[1]; /* complete name, rebuilt at start */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      BT_BYTES_LIST_LE16(BT_APPEARANCE_HID_KEYBOARD)),
};

/* ------------------------------------------------------------------ */
/* Advertising policy                                                 */
/* ------------------------------------------------------------------ */

struct bond_count_ctx {
	size_t n;
};

static void bond_count_cb(const struct bt_bond_info *info, void *user_data)
{
	struct bond_count_ctx *ctx = user_data;

	ctx->n++;
}

static size_t bond_count(void)
{
	struct bond_count_ctx ctx = { 0 };

	bt_foreach_bond(BT_ID_DEFAULT, bond_count_cb, &ctx);
	return ctx.n;
}

static void bond_to_fal_cb(const struct bt_bond_info *info, void *user_data)
{
	int err = bt_le_filter_accept_list_add(&info->addr);

	if (err && err != -EALREADY) {
		printk("KBD_BLE: filter accept list add err %d\n", err);
	}
}

/* Caller holds ble_lock. Re-evaluates the advertising policy truth table:
 *
 *   started | window | bonds | action
 *   --------+--------+-------+----------------------------------------
 *     no    |   *    |   *   | nothing (BLE backend stopped)
 *     yes   |  yes   |   *   | public connectable + bondable adv
 *     yes   |   no   |  >0   | connectable adv, FAL = bonded hosts
 *     yes   |   no   |   0   | silent (no advertising)
 */
static void adv_update_locked(void)
{
	int err;

	if (!bt_inited || !ble_started) {
		return;
	}

	/* The filter accept list may only be modified while not advertising. */
	err = bt_le_adv_stop();
	if (err && err != -EALREADY) {
		printk("KBD_BLE: adv stop err %d\n", err);
	}
	adv_active = false;

	if (pair_window) {
		bt_set_bondable(true);
		err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				      sd, ARRAY_SIZE(sd));
		if (!err) {
			printk("KBD_BLE: public advertising (pairing window)\n");
		}
	} else if (bond_count() > 0) {
		struct bt_le_adv_param p = *BT_LE_ADV_CONN_FAST_2;

		p.options |= BT_LE_ADV_OPT_FILTER_CONN;
		bt_set_bondable(false);
		bt_le_filter_accept_list_clear();
		bt_foreach_bond(BT_ID_DEFAULT, bond_to_fal_cb, NULL);
		err = bt_le_adv_start(&p, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
		if (!err) {
			printk("KBD_BLE: filtered advertising (bonded hosts only)\n");
		}
	} else {
		bt_set_bondable(false);
		printk("KBD_BLE: advertising off (no window, no bonds)\n");
		return;
	}

	adv_active = (err == 0);

	if (err == -ENOMEM) {
		/* All connection objects in use; retried after a disconnect. */
		printk("KBD_BLE: adv start deferred (all connections busy)\n");
	} else if (err) {
		printk("KBD_BLE: adv start err %d\n", err);
	}
}

/* Deferred re-evaluation from RX-thread callbacks (disconnect / bond
 * deleted); the delay lets the connection object get recycled first so a
 * connectable adv restart cannot fail with -ENOMEM.
 */
static void adv_dwork_fn(struct k_work *work)
{
	k_mutex_lock(&ble_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(graveyard); i++) {
		if (graveyard[i]) {
			bt_conn_unref(graveyard[i]);
			graveyard[i] = NULL;
		}
	}
	adv_update_locked();
	k_mutex_unlock(&ble_lock);
}

static K_WORK_DELAYABLE_DEFINE(adv_dwork, adv_dwork_fn);

static void pair_win_expired(struct k_work *work)
{
	k_mutex_lock(&ble_lock, K_FOREVER);
	if (pair_window) {
		pair_window = false;
		printk("KBD_BLE: pairing window closed (timeout)\n");
		adv_update_locked();
	}
	k_mutex_unlock(&ble_lock);
}

static K_WORK_DELAYABLE_DEFINE(pair_win_work, pair_win_expired);

/* ------------------------------------------------------------------ */
/* Connection / pairing callbacks (BT RX thread: no ble_lock here)    */
/* ------------------------------------------------------------------ */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("KBD_BLE: connection to %s failed (err 0x%02x %s)\n",
		       addr, err, bt_hci_err_to_str(err));
		return;
	}

	printk("KBD_BLE: connected %s\n", addr);

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		if (!conns[i]) {
			conns[i] = bt_conn_ref(conn);
			break;
		}
	}

	/* Encryption is required for report access; triggers JustWorks SMP
	 * pairing (bonded only while the pairing window is open).
	 */
	int serr = bt_conn_set_security(conn, BT_SECURITY_L2);

	if (serr) {
		printk("KBD_BLE: security request err %d\n", serr);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("KBD_BLE: disconnected %s (reason 0x%02x %s)\n",
	       addr, reason, bt_hci_err_to_str(reason));

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		if (conns[i] == conn) {
			conns[i] = NULL;
			break;
		}
	}
	bool queued = false;

	for (size_t i = 0; i < ARRAY_SIZE(graveyard); i++) {
		if (!graveyard[i]) {
			graveyard[i] = conn; /* unref deferred to adv_dwork_fn */
			queued = true;
			break;
		}
	}
	if (!queued) {
		/* Should not happen (2 slots per conn); avoid leaking the ref. */
		printk("KBD_BLE: graveyard full, unref inline\n");
		bt_conn_unref(conn);
	}

	/* Policy on link loss: window open -> public adv, else bonds ->
	 * filtered adv, else silent. Handled in adv_update_locked().
	 */
	k_work_reschedule(&adv_dwork, K_MSEC(50));
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (!err) {
		printk("KBD_BLE: security changed, level %u\n", level);
	} else {
		printk("KBD_BLE: security failed, level %u err %s (%d)\n",
		       level, bt_security_err_to_str(err), err);
	}
}

BT_CONN_CB_DEFINE(kbd_ble_conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	printk("KBD_BLE: pairing complete (bonded=%d)\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	printk("KBD_BLE: pairing failed (%s)\n", bt_security_err_to_str(reason));
}

static void bond_deleted_cb(uint8_t id, const bt_addr_le_t *peer)
{
	printk("KBD_BLE: bond deleted\n");
	/* Fewer bonds may flip the policy to silent; re-evaluate. */
	k_work_reschedule(&adv_dwork, K_MSEC(50));
}

static struct bt_conn_auth_info_cb auth_info_cbs = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
	.bond_deleted = bond_deleted_cb,
};

/* ------------------------------------------------------------------ */
/* Public API (kbd.h)                                                 */
/* ------------------------------------------------------------------ */

int kbd_ble_start(void)
{
	int err = 0;

	k_mutex_lock(&ble_lock, K_FOREVER);
	if (ble_started) {
		goto out;
	}

	if (!bt_inited) {
		err = bt_enable(NULL);
		if (err) {
			printk("KBD_BLE: bt_enable err %d\n", err);
			goto out;
		}
		bt_inited = true;
		bt_conn_auth_info_cb_register(&auth_info_cbs);
		boot_in_attr = bt_gatt_find_by_uuid(hids_svc.attrs, hids_svc.attr_count,
						    BT_UUID_HIDS_BOOT_KB_IN_REPORT);
		if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
			settings_load(); /* restore persisted bonds */
		}
	}

	/* Device identity: cfg device.name drives both GAP name and adv. */
	cfg_get_str("device.name", ble_name, sizeof(ble_name), "AT-Node-S3");
	bt_set_name(ble_name);
	sd[0].type = BT_DATA_NAME_COMPLETE;
	sd[0].data = (const uint8_t *)ble_name;
	sd[0].data_len = MIN(strlen(ble_name), 29); /* SD PDU payload limit */

	ble_started = true;
	adv_update_locked();
	printk("KBD_BLE: started (name %s)\n", ble_name);

out:
	k_mutex_unlock(&ble_lock);
	return ble_started ? 0 : err;
}

void kbd_ble_stop(void)
{
	k_mutex_lock(&ble_lock, K_FOREVER);
	if (!ble_started) {
		k_mutex_unlock(&ble_lock);
		return;
	}
	ble_started = false;
	pair_window = false;
	k_work_cancel_delayable(&pair_win_work);
	k_work_cancel_delayable(&adv_dwork);
	bt_le_adv_stop();
	adv_active = false;
	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		if (conns[i]) {
			bt_conn_disconnect(conns[i], BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
	k_mutex_unlock(&ble_lock);
	printk("KBD_BLE: stopped\n");
}

int kbd_ble_send(uint8_t mods, const uint8_t keys[6])
{
	int ret = -ENOTCONN;

	if (!boot_in_attr) {
		return -ENOTCONN;
	}

	boot_in_report[0] = mods;
	boot_in_report[1] = 0;
	memcpy(&boot_in_report[2], keys, 6);

	/* First connected, encrypted host wins (JustWorks -> level 2). The
	 * lock also pins the conn objects: they are only freed by
	 * adv_dwork_fn(), which needs this same lock.
	 */
	k_mutex_lock(&ble_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		struct bt_conn *conn = conns[i];

		if (conn && bt_conn_get_security(conn) >= BT_SECURITY_L2 &&
		    bt_gatt_is_subscribed(conn, boot_in_attr, BT_GATT_CCC_NOTIFY)) {
			ret = bt_gatt_notify(conn, boot_in_attr, boot_in_report,
					     sizeof(boot_in_report));
			break;
		}
	}
	k_mutex_unlock(&ble_lock);
	return ret;
}

bool kbd_ble_connected(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		if (conns[i]) {
			return true;
		}
	}
	return false;
}

void kbd_ble_pair_open(void)
{
	if (!ble_started) {
		kbd_ble_start();
	}

	k_mutex_lock(&ble_lock, K_FOREVER);
	pair_window = true;
	k_work_reschedule(&pair_win_work, K_SECONDS(PAIR_WINDOW_SECONDS));
	adv_update_locked();
	k_mutex_unlock(&ble_lock);
	printk("KBD_BLE: pairing window open (%d s)\n", PAIR_WINDOW_SECONDS);
}

bool kbd_ble_pair_window_active(void)
{
	return pair_window;
}

bool kbd_ble_has_bond(void)
{
	return bt_inited && bond_count() > 0;
}

void kbd_ble_clear_bonds(void)
{
	if (!bt_inited) {
		return;
	}

	k_mutex_lock(&ble_lock, K_FOREVER);
	/* Drop live links first (same as the Arduino bonds/clear handler). */
	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		if (conns[i]) {
			bt_conn_disconnect(conns[i], BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
	k_mutex_unlock(&ble_lock);

	int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	printk("KBD_BLE: all bonds cleared (err %d)\n", err);

	k_mutex_lock(&ble_lock, K_FOREVER);
	adv_update_locked();
	k_mutex_unlock(&ble_lock);
}

/* ------------------------------------------------------------------ */
/* Web UI queries (kbd.h): addr, advertising state, peers, bonds      */
/* ------------------------------------------------------------------ */

bool kbd_ble_advertising(void)
{
	/* Written only under ble_lock; benign racy read, same pattern as
	 * kbd_ble_connected()/kbd_ble_pair_window_active().
	 */
	return adv_active;
}

void kbd_ble_addr_str(char *buf, size_t len)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	if (!len) {
		return;
	}
	buf[0] = '\0';

	/* Before bt_enable() there is no identity to report. bt_id_get()
	 * only copies stack state, so no ble_lock is needed here.
	 */
	if (!bt_inited) {
		return;
	}
	bt_id_get(addrs, &count);
	if (!count || !bt_addr_le_cmp(&addrs[0], BT_ADDR_LE_NONE)) {
		return;
	}
	/* Plain "AA:BB:CC:DD:EE:FF" (bt_addr_le_to_str appends a type
	 * suffix the SPA does not want). val[] is little-endian.
	 */
	snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
		 addrs[0].a.val[5], addrs[0].a.val[4], addrs[0].a.val[3],
		 addrs[0].a.val[2], addrs[0].a.val[1], addrs[0].a.val[0]);
}

/* Plain peer address, no type suffix (see kbd_ble_addr_str). */
static void peer_addr_fmt(char out[18], const bt_addr_le_t *a)
{
	snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
		 a->a.val[5], a->a.val[4], a->a.val[3],
		 a->a.val[2], a->a.val[1], a->a.val[0]);
}

/* Bounded JSON append; sets *trunc instead of ever overflowing. */
static void json_append(char *buf, size_t len, size_t *off, bool *trunc,
			const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*trunc) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(buf + *off, len - *off, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= len - *off) {
		*trunc = true;
		buf[*off] = '\0';
		return;
	}
	*off += (size_t)n;
}

struct bond_match_ctx {
	const bt_addr_le_t *addr;
	bool found;
};

static void bond_match_cb(const struct bt_bond_info *info, void *user_data)
{
	struct bond_match_ctx *ctx = user_data;

	if (!bt_addr_le_cmp(&info->addr, ctx->addr)) {
		ctx->found = true;
	}
}

/* Is this peer address in the persistent bond store? Note: a peer using
 * a resolvable private address will not string-match its bond identity
 * address and reports bonded=false; hosts that paired with us while
 * using an RPA are rare for this peripheral.
 */
static bool addr_is_bonded(const bt_addr_le_t *addr)
{
	struct bond_match_ctx ctx = { .addr = addr };

	bt_foreach_bond(BT_ID_DEFAULT, bond_match_cb, &ctx);
	return ctx.found;
}

int kbd_ble_peers_json(char *buf, size_t len)
{
	size_t off = 0;
	bool trunc = false;
	bool first = true;

	if (!buf || !len) {
		return -EINVAL;
	}

	/* ble_lock pins the conns[] objects: they are only freed by
	 * adv_dwork_fn(), which needs this same lock.
	 */
	k_mutex_lock(&ble_lock, K_FOREVER);
	json_append(buf, len, &off, &trunc, "[");
	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		struct bt_conn *conn = conns[i];
		char astr[18];

		if (!conn) {
			continue;
		}
		peer_addr_fmt(astr, bt_conn_get_dst(conn));
		json_append(buf, len, &off, &trunc,
			    "%s{\"addr\":\"%s\",\"bonded\":%s,\"encrypted\":%s}",
			    first ? "" : ",", astr,
			    addr_is_bonded(bt_conn_get_dst(conn)) ? "true" : "false",
			    bt_conn_get_security(conn) >= BT_SECURITY_L2 ? "true" : "false");
		first = false;
	}
	json_append(buf, len, &off, &trunc, "]");
	k_mutex_unlock(&ble_lock);

	if (trunc) {
		if (len < sizeof("[]")) {
			buf[0] = '\0';
			return 0;
		}
		strcpy(buf, "[]");
		return 2;
	}
	return (int)off;
}

struct bonds_json_ctx {
	char *buf;
	size_t len;
	size_t off;
	bool trunc;
	int idx;
};

static void bonds_json_cb(const struct bt_bond_info *info, void *user_data)
{
	struct bonds_json_ctx *ctx = user_data;
	char astr[18];

	peer_addr_fmt(astr, &info->addr);
	json_append(ctx->buf, ctx->len, &ctx->off, &ctx->trunc,
		    "%s{\"idx\":%d,\"addr\":\"%s\"}",
		    ctx->idx ? "," : "", ctx->idx, astr);
	ctx->idx++;
}

int kbd_ble_bonds_json(char *buf, size_t len)
{
	struct bonds_json_ctx ctx = { .buf = buf, .len = len };

	if (!buf || !len) {
		return -EINVAL;
	}

	/* idx is the bt_foreach_bond() enumeration order (internal bond-pool
	 * slot order, stable while no bond is added/removed). The SPA
	 * lists bonds and immediately deletes by idx, so this is consistent
	 * with kbd_ble_unbond_idx() below.
	 */
	k_mutex_lock(&ble_lock, K_FOREVER);
	json_append(buf, len, &ctx.off, &ctx.trunc, "[");
	bt_foreach_bond(BT_ID_DEFAULT, bonds_json_cb, &ctx);
	json_append(buf, len, &ctx.off, &ctx.trunc, "]");
	k_mutex_unlock(&ble_lock);

	if (ctx.trunc) {
		if (len < sizeof("[]")) {
			buf[0] = '\0';
			return 0;
		}
		strcpy(buf, "[]");
		return 2;
	}
	return (int)ctx.off;
}

struct unbond_ctx {
	int idx;   /* running enumeration index */
	int want;  /* index to remove */
	bool found;
	bt_addr_le_t addr;
};

static void unbond_cb(const struct bt_bond_info *info, void *user_data)
{
	struct unbond_ctx *ctx = user_data;

	if (ctx->idx == ctx->want && !ctx->found) {
		ctx->addr = info->addr;
		ctx->found = true;
	}
	ctx->idx++;
}

int kbd_ble_unbond_idx(int idx)
{
	struct unbond_ctx ctx = { .want = idx };
	int err;

	if (idx < 0 || !bt_inited) {
		return -ENOENT;
	}

	/* Holding ble_lock across bt_unpair() is safe: the RX thread (which
	 * fires bond_deleted_cb) never takes ble_lock, it only reschedules
	 * adv_dwork; the policy re-evaluation happens there.
	 */
	k_mutex_lock(&ble_lock, K_FOREVER);
	bt_foreach_bond(BT_ID_DEFAULT, unbond_cb, &ctx);
	if (!ctx.found) {
		k_mutex_unlock(&ble_lock);
		return -ENOENT;
	}
	err = bt_unpair(BT_ID_DEFAULT, &ctx.addr);
	k_mutex_unlock(&ble_lock);

	printk("KBD_BLE: unbond idx %d (err %d)\n", idx, err);
	return err;
}

#else /* CONFIG_BT=n: stubs */

int kbd_ble_start(void)
{
	printk("KBD_BLE: BT not compiled in\n");
	return -ENODEV;
}
void kbd_ble_stop(void) {}
int kbd_ble_send(uint8_t mods, const uint8_t keys[6])
{
	ARG_UNUSED(mods); ARG_UNUSED(keys);
	return -ENOTCONN;
}
bool kbd_ble_connected(void) { return false; }
void kbd_ble_pair_open(void) {}
bool kbd_ble_pair_window_active(void) { return false; }
bool kbd_ble_has_bond(void) { return false; }
void kbd_ble_clear_bonds(void) {}
bool kbd_ble_advertising(void) { return false; }
void kbd_ble_addr_str(char *buf, size_t len) { buf[0] = '\0'; }
int kbd_ble_peers_json(char *buf, size_t len) { return snprintk(buf, len, "[]"); }
int kbd_ble_bonds_json(char *buf, size_t len) { return snprintk(buf, len, "[]"); }
int kbd_ble_unbond_idx(int idx)
{
	ARG_UNUSED(idx);
	return -ENOENT;
}

#endif /* CONFIG_BT */
