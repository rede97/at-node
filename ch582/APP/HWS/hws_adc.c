/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_adc.c
 * Author             : at-node
 * Description        : HWS ADC — external single-ended channel reads.
 *
 *   Reuses the hws_adc_sample() helper from hws_core (save/restore of
 *   the ADC registers, so BLE temp/battery sampling is unaffected).
 ********************************************************************************/

#include "hws_adc.h"
#include "hws.h"

#if(defined(HWS_ADC)) && (HWS_ADC == TRUE)

static uint8_t s_ch;

static void adc_ext_init(void)
{
    ADC_ExtSingleChSampInit(SampleFreq_8, ADC_PGA_0);
    ADC_ChannelCfg(s_ch);
}

uint16_t hws_adc_read_mv(uint8_t ch)
{
    if (ch > 13)
        return 0xFFFF;
    s_ch = ch;
    uint16_t raw = hws_adc_sample(adc_ext_init) & 0x0FFF;
    return (uint16_t)(((uint32_t)raw * HWS_ADC_FULLSCALE_MV + 2047) / 4095);
}

#endif /* HWS_ADC == TRUE */
