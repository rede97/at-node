/********************************** (C) COPYRIGHT *******************************
 * File Name          : scanparamservice.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/11
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef BLE_SCAN_PARAM_H
#define BLE_SCAN_PARAM_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * INCLUDES
 */

/*********************************************************************
 * CONSTANTS
 */

// Scan Characteristic Lengths
#define BLE_SCAN_INTERVAL_WINDOW_CHAR_LEN    4
#define BLE_SCAN_PARAM_REFRESH_LEN           1

// Scan Parameter Refresh Values
#define BLE_SCAN_PARAM_REFRESH_REQ           0x00

// Callback events
#define BLE_SCAN_INTERVAL_WINDOW_SET         1

// Get/Set parameters
#define BLE_SCAN_PARAM_INTERVAL        0
#define BLE_SCAN_PARAM_WINDOW          1

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * Profile Callbacks
 */

// Scan Parameters Service callback function
typedef void (*ble_scan_param_service_cb_t)(uint8_t event);

/*********************************************************************
 * API FUNCTIONS
 */

/*********************************************************************
 * @fn      ble_scan_param_add_service
 *
 * @brief   Initializes the Service by registering
 *          GATT attributes with the GATT server.
 *
 * @return  Success or Failure
 */
extern bStatus_t ble_scan_param_add_service(void);

/*********************************************************************
 * @fn      ble_scan_param_register
 *
 * @brief   Register a callback function with the Scan Parameters Service.
 *
 * @param   pfnServiceCB - Callback function.
 *
 * @return  None.
 */
extern void ble_scan_param_register(ble_scan_param_service_cb_t pfnServiceCB);

/*********************************************************************
 * @fn      ble_scan_param_set_param
 *
 * @brief   Set a Scan Parameters Service parameter.
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
extern bStatus_t ble_scan_param_set_param(uint8_t param, uint8_t len, void *value);

/*********************************************************************
 * @fn      ble_scan_param_get_param
 *
 * @brief   Get a Scan Parameters Service parameter.
 *
 * @param   param - Profile parameter ID
 * @param   value - pointer to data to get.  This is dependent on
 *          the parameter ID and WILL be cast to the appropriate
 *          data type (example: data type of uint16_t will be cast to
 *          uint16_t pointer).
 *
 * @return  bStatus_t
 */
extern bStatus_t ble_scan_param_get_param(uint8_t param, void *value);

/*********************************************************************
 * @fn      ble_scan_param_refresh_notify
 *
 * @brief   Notify the peer to refresh the scan parameters.
 *
 * @param   connHandle - connection handle
 *
 * @return  None
 */
extern void ble_scan_param_refresh_notify(uint16_t connHandle);

extern void ble_scan_param_handle_conn_status_cb(uint16_t connHandle, uint8_t changeType);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCAN_PARAM_H */
