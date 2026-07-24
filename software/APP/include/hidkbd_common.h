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

typedef enum { KB_USB = 1, KB_BLE = 2, KB_BOTH = 3 } kb_mode_t;

void kb_set_mode(kb_mode_t m);
kb_mode_t kb_get_mode(void);
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
int      kb_ble_disconnect_slot(uint8_t slot);    /* drop one host link */
void     kb_ble_send_report_slot(uint8_t slot, uint8_t mods, uint8_t *keys, int count);

#ifdef __cplusplus
}
#endif

#endif
