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
int  kb_ble_disconnect(void);
void kb_ble_forget_bonds(void);

#ifdef __cplusplus
}
#endif

#endif
