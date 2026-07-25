/********************************** (C) COPYRIGHT *******************************
 * File Name          : hidkbd.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef HIDKBD_H
#define HIDKBD_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * INCLUDES
 */

/*********************************************************************
 * CONSTANTS
 */

// Task Events
#define START_DEVICE_EVT          0x0001
#define START_REPORT_EVT          0x0002
#define START_PARAM_UPDATE_EVT    0x0004
#define START_PHY_UPDATE_EVT      0x0008
/*********************************************************************
 * MACROS
 */

#ifdef DEBUG
#undef PRINT
#define PRINT(fmt, ...) printf(fmt "\r", ##__VA_ARGS__)
#endif


/*********************************************************************
 * FUNCTIONS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

/*
 * Task Initialization for the BLE Application
 */
extern void ble_hid_emu_init(void);
extern uint16_t ble_hid_emu_process_event(uint8_t task_id, uint16_t events);
extern uint8_t kb_ble_connected(void);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif
