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
void kb_press_and_release(uint8_t keycode);
void kb_key_down(uint8_t keycode);
void kb_key_up(uint8_t keycode);
void kb_set_mods(uint8_t mods);
void kb_release_all(void);
uint8_t kb_ble_connected(void);

#ifdef __cplusplus
}
#endif

#endif
