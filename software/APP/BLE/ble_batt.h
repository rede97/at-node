/********************************** (C) COPYRIGHT *******************************
 * File Name          : battservice.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/11
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef BLE_BATT_H
#define BLE_BATT_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * INCLUDES
 */

/*********************************************************************
 * CONSTANTS
 */

// Battery Service Get/Set Parameters
#define BLE_BATT_PARAM_LEVEL                   0
#define BLE_BATT_PARAM_CRITICAL_LEVEL          1
#define BLE_BATT_PARAM_SERVICE_HANDLE          2
#define BLE_BLE_BATT_PARAM_LEVEL_IN_REPORT    3

// Callback events
#define BLE_BATT_LEVEL_NOTI_ENABLED            1
#define BLE_BATT_LEVEL_NOTI_DISABLED           2

// HID Report IDs for the service
#define BLE_HID_RPT_ID_BATT_LEVEL_IN           4  // Battery Level input report ID

/*********************************************************************
 * TYPEDEFS
 */

// Battery Service callback function
typedef void (*ble_batt_service_cb_t)(uint8_t event);

// Battery measure HW setup function
typedef void (*ble_batt_service_setup_cb_t)(void);

// Battery measure percentage calculation function
typedef uint8_t (*ble_batt_service_calc_cb_t)(uint16_t adcVal);

// Battery measure HW teardown function
typedef void (*ble_batt_service_teardown_cb_t)(void);

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * Profile Callbacks
 */

/*********************************************************************
 * API FUNCTIONS
 */

/*********************************************************************
 * @fn      ble_batt_add_service
 *
 * @brief   Initializes the Battery service by registering
 *          GATT attributes with the GATT server.
 *
 * @return  Success or Failure
 */
extern bStatus_t ble_batt_add_service(void);

/*********************************************************************
 * @fn      ble_batt_register
 *
 * @brief   Register a callback function with the Battery Service.
 *
 * @param   pfnServiceCB - Callback function.
 *
 * @return  None.
 */
extern void ble_batt_register(ble_batt_service_cb_t pfnServiceCB);

/*********************************************************************
 * @fn      ble_batt_set_param
 *
 * @brief   Set a Battery Service parameter.
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
extern bStatus_t ble_batt_set_param(uint8_t param, uint8_t len, void *value);

/*********************************************************************
 * @fn      ble_batt_get_param
 *
 * @brief   Get a Battery parameter.
 *
 * @param   param - Profile parameter ID
 * @param   value - pointer to data to get.  This is dependent on
 *          the parameter ID and WILL be cast to the appropriate
 *          data type (example: data type of uint16_t will be cast to
 *          uint16_t pointer).
 *
 * @return  bStatus_t
 */
extern bStatus_t ble_batt_get_param(uint8_t param, void *value);

/*********************************************************************
 * @fn          ble_batt_meas_level
 *
 * @brief       Measure the battery level and update the battery
 *              level value in the service characteristics.  If
 *              the battery level-state characteristic is configured
 *              for notification and the battery level has changed
 *              since the last measurement, then a notification
 *              will be sent.
 *
 * @return      Success or Failure
 */
extern bStatus_t ble_batt_meas_level(void);

/*********************************************************************
 * @fn      ble_batt_setup
 *
 * @brief   Set up which ADC source is to be used. Defaults to VDD/3.
 *
 * @param   adc_ch - ADC Channel, e.g. HAL_ADC_CHN_AIN6
 * @param   minVal - max battery level
 * @param   maxVal - min battery level
 * @param   sCB - HW setup callback
 * @param   tCB - HW tear down callback
 * @param   cCB - percentage calculation callback
 *
 * @return  none.
 */
extern void ble_batt_setup(uint8_t adc_ch, uint16_t minVal, uint16_t maxVal,
                       ble_batt_service_setup_cb_t sCB, ble_batt_service_teardown_cb_t tCB,
                       ble_batt_service_calc_cb_t cCB);

/*********************************************************************
 * @fn          ble_batt_handle_conn_status_cb
 *
 * @brief       Battery Service link status change handler function.
 *
 * @param       connHandle - connection handle
 * @param       changeType - type of change
 *
 * @return      none
 */
void ble_batt_handle_conn_status_cb(uint16_t connHandle, uint8_t changeType);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* BLE_BATT_H */
