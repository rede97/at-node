/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_batt.c
 * Author             : at-node
 * Description        : HWS battery monitor — VDD measurement via internal
 *                      ADC channel CH_INTE_VBAT
 *
 *   Replaces the old ble_batt.c fake measurement (hardcoded adc=300,
 *   always reported ~20 %). Single conversion on demand via the shared
 *   hws_adc_sample() helper — no periodic task here; the BLE battery
 *   service schedules measurements.
 ********************************************************************************/

#include "config.h"
#include "hws.h"
#include "hws_batt.h"

/*********************************************************************
 * hws_batt_read_mv
 */
uint16_t hws_batt_read_mv(void)
{
    uint16_t adc_data = hws_adc_sample(ADC_InterBATSampInit) & 0x0FFF;

    /* raw → mV: see HWS_BATT_ADC_FULLSCALE_MV in config.h */
    return (uint16_t)(((uint32_t)adc_data * HWS_BATT_ADC_FULLSCALE_MV + 2047) / 4095);
}

/*********************************************************************
 * hws_batt_read_percent
 */
uint8_t hws_batt_read_percent(void)
{
    uint16_t mv = hws_batt_read_mv();

    if (mv >= HWS_BATT_MAX_MV) return 100;
    if (mv <= HWS_BATT_MIN_MV) return 0;
    return (uint8_t)(((uint32_t)(mv - HWS_BATT_MIN_MV) * 100 +
                      (HWS_BATT_MAX_MV - HWS_BATT_MIN_MV) / 2) /
                     (HWS_BATT_MAX_MV - HWS_BATT_MIN_MV));
}
