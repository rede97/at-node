/********************************** (C) COPYRIGHT *******************************
 * File Name          : hidkbd_common.h
 * Author             : at-node
 * Version            : V1.0
 * Description        : Keyboard routing layer — BLE/USB/BOTH dispatch
 ********************************************************************************/

#ifndef HIDKBD_COMMON_H
#define HIDKBD_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

int  kb_press(uint8_t keycode);
int  kb_release(void);
int  kb_set_mods(uint8_t mods);
uint8_t kb_ble_connected(void);
int  kb_ble_disconnect(void);          /* drop ALL host links */
void kb_ble_forget_bonds(void);

/* ---- multi-mode slot API (KBD_MULTI builds; KBD_MAX_CONN==1 elsewhere,
   slot is always 0 and the API folds back to single-host behavior) ---- */
int      kb_ble_conn_count(void);                 /* active host links */
uint16_t kb_ble_slot_handle(uint8_t slot);        /* GAP_CONNHANDLE_INIT if free */
const uint8_t *kb_ble_slot_addr(uint8_t slot);    /* host MAC (LSB-first) or NULL */
uint8_t  kb_ble_slot_secure(uint8_t slot);
uint8_t  kb_ble_slot_notify(uint8_t slot);
uint8_t  kb_ble_slot_params(uint8_t slot, uint16_t *intv, uint16_t *lat);        /* CCCD subscribed? */
const uint8_t *kb_ble_slot_mac(uint8_t slot);     /* per-slot own MAC */
int      kb_ble_slot_set_mac(uint8_t slot, const uint8_t *addr);
const char *kb_ble_slot_name(uint8_t slot);       /* AT+NAME label */
int      kb_ble_slot_set_name(uint8_t slot, const char *name);
uint16_t kb_ble_slot_pace(uint8_t slot);          /* KEY_STR pace ms */
int      kb_ble_slot_set_pace(uint8_t slot, uint16_t ms);
const uint8_t *kb_ble_slot_bound_addr(uint8_t slot); /* reserved host or NULL */
int      kb_ble_unbind_slot(uint8_t slot);        /* forget host: slot+bond */
void     kb_ble_factory_reset(void);              /* wipe all kbd config */
void     kb_ble_pair_open(int slot);              /* 60s pairing window */       /* encrypted/bonded? */
int      kb_ble_disconnect_slot(uint8_t slot);    /* drop one host link */
uint8_t  kb_ble_send_report_slot(uint8_t slot, uint8_t mods, uint8_t *keys, int count);

#ifdef __cplusplus
}
#endif

#endif
