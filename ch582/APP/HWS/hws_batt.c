/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_batt.c
 * Author             : at-node
 * Description        : HWS battery monitor — VDD measurement via internal
 *                      ADC channel CH_INTE_VBAT.
 *
 *   The VBAT channel uses -12 dB PGA (ADC_PGA_0). The -12 dB gain
 *   itself provides 1/4 attenuation, so no extra divisor is needed.
 *   Conversion (datasheet Table 15-2, pga=0):
 *     mV = raw × Vref / 512 − 3 × Vref
 *   Vref = HWS_ADC_VREF_MV (default 1050 mV = VINTA typ).
 ********************************************************************************/

#include "config.h"
#include "hws.h"
#include "hws_batt.h"

/*********************************************************************
 * hws_batt_read_mv
 */
uint16_t hws_batt_read_mv(void)
{
    uint16_t raw = hws_adc_sample(ADC_InterBATSampInit) & 0x0FFF;
    uint32_t vref = HWS_ADC_VREF_MV;

    /* VBAT: -12 dB PGA (Table 15-2, pga=0).  -12 dB already provides
       1/4 attenuation; no extra multiplier needed. */
    return (uint16_t)((raw * vref / 512) - 3 * vref);
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
