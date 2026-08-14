/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_batt.h
 * Author             : at-node
 * Description        : HWS battery monitor — VDD measurement via internal
 *                      ADC channel CH_INTE_VBAT
 ********************************************************************************/

#ifndef __HWS_BATT_H
#define __HWS_BATT_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * hws_batt_read_mv — measure battery (VDD) voltage in millivolts.
 *
 *   Uses the internal battery detect channel (CH_INTE_VBAT, -12 dB PGA).
 *   Saves/restores ADC registers — safe to call while the BLE stack
 *   uses the ADC for temperature sampling (hws_get_temp).
 *
 *   Accuracy depends on HWS_BATT_ADC_FULLSCALE_MV calibration
 *   (config.h) — the default is theoretical, calibrate per board.
 */
uint16_t hws_batt_read_mv(void);

/*********************************************************************
 * hws_batt_read_percent — battery charge as 0–100 %.
 *
 *   Linear map of HWS_BATT_MIN_MV..HWS_BATT_MAX_MV (config.h).
 *   Default Li-ion: 3.0 V = 0 %, 4.2 V = 100 %. Clamped at both ends.
 */
uint8_t hws_batt_read_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* __HWS_BATT_H */
