/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_init.h
 * Author             : at-node
 * Description        : BLE peripheral initialization header
 ********************************************************************************/

#ifndef BLE_INIT_H
#define BLE_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Initialize BLE peripheral: GAP role, GATT services, start advertising.
 *
 *   Must be called after hws_init() and ble_stack_init().
 *   Must be called before main_loop().
 */
void ble_peripheral_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_INIT_H */
