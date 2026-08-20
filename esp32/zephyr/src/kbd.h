/*
 * AT-Node Zephyr — keyboard routing layer (CH582 kb_* equivalent).
 *
 * Target bitmask routing like wchble/mr2 hidkbd_common.h: AT+DEV selects
 * USB and/or BLE; kbd_send_report() fans out to enabled backends.
 * Input injection rules (FIELD-NOTES F18): always use kbd_tap() /
 * kbd_type_text() (atomic press+release via the sequence thread);
 * raw kbd_send_report() press MUST be paired with a kbd_send_report(0,{0})
 * release — the AT+KEY handler documents this.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KB_TGT_USB 0x01
#define KB_TGT_BLE 0x02
#define KB_TGT_ALL (KB_TGT_USB | KB_TGT_BLE)

void    kbd_init(void); /* starts the tap/text sequence thread */
void    kbd_set_targets(uint8_t mask);
uint8_t kbd_get_targets(void);

/* Routed HID boot-keyboard report: mods bitmap + up to 6 keycodes.
 * Returns 0 if at least one enabled backend accepted it, else -ENOTCONN.
 */
int  kbd_send_report(uint8_t mods, const uint8_t keys[6]);
int  kbd_tap(uint16_t ms, uint8_t mods, uint8_t key);
int  kbd_type_text(const char *s, uint16_t ms, uint16_t gap);

/* Backend interface (kbd_ble.c / kbd_usb.c) */
int  kbd_ble_start(void);  /* advertising per pairing policy */
void kbd_ble_stop(void);
int  kbd_usb_start(void);
void kbd_usb_stop(void);
int  kbd_ble_send(uint8_t mods, const uint8_t keys[6]); /* -ENOTCONN no host */
int  kbd_usb_send(uint8_t mods, const uint8_t keys[6]); /* -ENOTCONN no host */
bool kbd_ble_connected(void);
bool kbd_usb_ready(void);

/* Pairing policy (Arduino semantics): default no open advertising; AT+PAIR=1
 * opens a 60 s public pairing window; afterwards only bonded hosts may
 * reconnect (filter accept list).
 */
void kbd_ble_pair_open(void);              /* start 60 s window */
bool kbd_ble_pair_window_active(void);
bool kbd_ble_has_bond(void);
void kbd_ble_clear_bonds(void);

/* Web UI (ble/status, bonds endpoints) support */
bool kbd_ble_advertising(void);
void kbd_ble_addr_str(char *buf, size_t len);            /* own BLE MAC */
int  kbd_ble_peers_json(char *buf, size_t len);          /* [{addr,bonded,encrypted}] */
int  kbd_ble_bonds_json(char *buf, size_t len);          /* [{idx,addr}] */
int  kbd_ble_unbond_idx(int idx);                        /* -ENOENT bad idx */
bool kbd_typing(void);                                   /* sequence queue busy */
