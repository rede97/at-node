/********************************** (C) COPYRIGHT *******************************
 * File Name          : devinfoservice.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/11
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef BLE_DEV_INFO_H
#define BLE_DEV_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * INCLUDES
 */

/*********************************************************************
 * CONSTANTS
 */

// Device Information Service Parameters
#define BLE_DEV_INFO_SYSTEM_ID              0
#define BLE_DEV_INFO_MODEL_NUMBER           1
#define BLE_DEV_INFO_SERIAL_NUMBER          2
#define BLE_DEV_INFO_FIRMWARE_REV           3
#define BLE_DEV_INFO_HARDWARE_REV           4
#define BLE_DEV_INFO_SOFTWARE_REV           5
#define BLE_DEV_INFO_MANUFACTURER_NAME      6
#define BLE_DEV_INFO_11073_CERT_DATA        7
#define BLE_DEV_INFO_PNP_ID                 8

// IEEE 11073 authoritative body values
#define BLE_DEV_INFO_11073_BODY_EMPTY       0
#define BLE_DEV_INFO_11073_BODY_IEEE        1
#define BLE_DEV_INFO_11073_BODY_CONTINUA    2
#define BLE_DEV_INFO_11073_BODY_EXP         254

// System ID length
#define BLE_DEV_INFO_SYSTEM_ID_LEN          8

// PnP ID length
#define BLE_DEV_INFO_PNP_ID_LEN             7

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * Profile Callbacks
 */

/*********************************************************************
 * API FUNCTIONS
 */

/*
 * ble_dev_info_add_service- Initializes the Device Information service by registering
 *          GATT attributes with the GATT server.
 *
 */

extern bStatus_t ble_dev_info_add_service(void);

/*********************************************************************
 * @fn      ble_dev_info_set_param
 *
 * @brief   Set a Device Information parameter.
 *
 * @param   param - Profile parameter ID
 * @param   len - length of data to right
 * @param   value - pointer to data to write.  This is dependent on
 *          the parameter ID and WILL be cast to the appropriate
 *          data type (example: data type of uint16_t will be cast to
 *          uint16_t pointer).
 *
 * @return  bStatus_t
 */
bStatus_t ble_dev_info_set_param(uint8_t param, uint8_t len, void *value);

/*
 * ble_dev_info_get_param - Get a Device Information parameter.
 *
 *    param - Profile parameter ID
 *    value - pointer to data to write.  This is dependent on
 *          the parameter ID and WILL be cast to the appropriate
 *          data type (example: data type of uint16_t will be cast to
 *          uint16_t pointer).
 */
extern bStatus_t ble_dev_info_get_param(uint8_t param, void *value);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* BLE_DEV_INFO_H */
