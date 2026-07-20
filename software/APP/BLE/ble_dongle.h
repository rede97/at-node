/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_dongle.h
 * Author             : at-node
 * Description        : BLE HID Host (dongle/receiver) — Central role
 *
 *   Compile-time role alternative to the BLE keyboard (Peripheral) mode,
 *   enabled via BLE_DONGLE=TRUE in config.h. AT-Node scans for a BLE
 *   keyboard, connects as Central, subscribes to its HID Boot Keyboard
 *   Input Report (0x2A22) and forwards the standard 8-byte reports to
 *   USB HID. Boot mode avoids HID Report Map parsing — the report layout
 *   is identical to USB HID keyboard reports.
 *
 *   AT commands (see at_cmds.c):
 *     AT+BT_SCAN=<sec>   scan, lists devices advertising HID service
 *     AT+BT_CONN=<idx>   connect to list entry
 *     AT+BT_DISC         disconnect
 ********************************************************************************/

#ifndef BLE_DONGLE_H
#define BLE_DONGLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

/*********************************************************************
 * ble_dongle_init — initialize Central role (GAP central + bond mgr +
 *   GATT client) and register the dongle TMOS task. Called from
 *   ble_peripheral_init() when BLE_DONGLE == TRUE (replaces all
 *   Peripheral services in that build).
 */
void    ble_dongle_init(void);

/*********************************************************************
 * ble_dongle_scan — start BLE device discovery for `seconds`.
 *
 *   Results are reported asynchronously over the AT response channel:
 *   one "+BT_SCAN:<idx>,<addr>,<rssi>,<name>" line per HID-advertising
 *   device, then "+BT_SCAN: done <n>".
 *   Returns 0 if scan started, -1 if busy (already scanning/connected).
 */
int     ble_dongle_scan(uint8_t seconds);

/*********************************************************************
 * ble_dongle_connect — connect to scan result <idx> (0-based, from the
 *   last ble_dongle_scan list). Returns 0 if connecting, -1 on bad
 *   index or busy. Outcome reported asynchronously:
 *   "+BT_CONN: connected" / "+BT_CONN: armed (boot mode)" / error.
 */
int     ble_dongle_connect(uint8_t index);

/*********************************************************************
 * ble_dongle_disconnect — terminate the current link. Returns 0 if a
 *   link was active, -1 if idle.
 */
int     ble_dongle_disconnect(void);

/*********************************************************************
 * ble_dongle_connected — 1 when a keyboard link is up, else 0.
 */
uint8_t ble_dongle_connected(void);

/*********************************************************************
 * ble_dongle_passkey — answer an SMP passkey request (MITM pairing).
 *
 *   When the keyboard displays a 6-digit code, enter it via
 *   AT+BT_PASSKEY=<code>. Returns 0 if a request was pending, -1 else.
 */
int     ble_dongle_passkey(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* BLE_DONGLE_H */
