/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_batt.c
 * Author             : at-node
 * Description        : HWS battery monitor — VDD measurement via internal
 *                      ADC channel CH_INTE_VBAT
 *
 *   Replaces the old ble_batt.c fake measurement (hardcoded adc=300,
 *   always reported ~20 %). Single conversion on demand — no periodic
 *   task here; the BLE battery service schedules measurements.
 ********************************************************************************/

#include "config.h"
#include "hws_batt.h"

/*********************************************************************
 * hws_batt_read_mv
 *
 *   ADC save/restore pattern mirrors hws_get_temp() (hws_core.c) —
 *   the ADC is shared with the BLE stack's temperature sampler.
 */
uint16_t hws_batt_read_mv(void)
{
    uint8_t  channel, config, tkey_cfg;
    uint16_t adc_data;

    /* Save current ADC configuration */
    tkey_cfg = R8_TKEY_CFG;
    channel  = R8_ADC_CHANNEL;
    config   = R8_ADC_CFG;

    /* Configure ADC for internal battery channel, single conversion */
    ADC_InterBATSampInit();
    R8_ADC_CONVERT |= RB_ADC_START;
    while (R8_ADC_CONVERT & RB_ADC_START);
    adc_data = R16_ADC_DATA & 0x0FFF;   /* 12-bit result */

    /* Restore previous ADC configuration */
    R8_ADC_CHANNEL = channel;
    R8_ADC_CFG = config;
    R8_TKEY_CFG = tkey_cfg;

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
