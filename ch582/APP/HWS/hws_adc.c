/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_adc.c
 * Author             : at-node
 * Description        : HWS ADC — external single-ended channel reads.
 *
 *   Reuses the hws_adc_sample() helper from hws_core (save/restore of
 *   the ADC registers, so BLE temp/battery sampling is unaffected).
 *
 *   Conversion formulas follow CH58x datasheet Table 15-2 (Vref=VINTA).
 *   For accuracy, calibrate HWS_ADC_VREF_MV per board (see config.h).
 ********************************************************************************/

#include "hws_adc.h"
#include "hws.h"

#if(defined(HWS_ADC)) && (HWS_ADC == TRUE)

static uint8_t s_ch;
static uint8_t s_pga;

static void adc_ext_init(void)
{
    /* Map user PGA (0-3) to WCH ADC_PGA_* constants:
        0=-12dB → ADC_PGA_0, 1=-6dB → ADC_PGA_1,
        2=0dB   → ADC_PGA_2, 3=6dB  → ADC_PGA_3  */
    ADC_ExtSingleChSampInit(SampleFreq_8, s_pga);
    ADC_ChannelCfg(s_ch);
}

/* Convert raw ADC to mV per datasheet Table 15-2, using VINTA=Vref. */
static uint16_t pga_raw_to_mv(uint16_t raw, uint8_t pga)
{
    uint32_t vref = HWS_ADC_VREF_MV;

    switch (pga) {
    case 0: /* -12 dB: Vin = (raw/512 − 3) × Vref  →  mV = raw×Vref/512 − 3×Vref */
        return (uint16_t)((raw * vref / 512) - 3 * vref);
    case 1: /* -6 dB:  Vin = (raw/1024 − 1) × Vref →  mV = raw×Vref/1024 − 1×Vref */
        return (uint16_t)((raw * vref / 1024) - 1 * vref);
    case 2: /* 0 dB:   Vin = raw/2048 × Vref       →  mV = raw×Vref/2048 */
        return (uint16_t)(raw * vref / 2048);
    case 3: /* 6 dB:   Vin = (raw+2048)/4096 × Vref →  mV = (raw+2048)×Vref/4096 */
        return (uint16_t)(((uint32_t)(raw + 2048)) * vref / 4096);
    default:
        return 0xFFFF;
    }
}

uint16_t hws_adc_read_mv(uint8_t ch, uint8_t pga, uint16_t *raw_out)
{
    if (ch > 13 || pga > 3)
        return 0xFFFF;
    s_ch  = ch;
    s_pga = pga;
    uint16_t raw = hws_adc_sample(adc_ext_init) & 0x0FFF;
    if (raw_out) *raw_out = raw;
    return pga_raw_to_mv(raw, pga);
}

#endif /* HWS_ADC == TRUE */
