/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_adc.h
 * Author             : at-node
 * Description        : HWS ADC — external single-ended channel reads.
 ********************************************************************************/

#ifndef HWS_ADC_H
#define HWS_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

/* Read external ADC channel <ch>, PGA <pga>. Returns mV; *raw_out gets
   the 12-bit ADC reading. Returns 0xFFFF on bad channel. */
uint16_t hws_adc_read_mv(uint8_t ch, uint8_t pga, uint16_t *raw_out);

#ifdef __cplusplus
}
#endif

#endif /* HWS_ADC_H */
