/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_ir.h
 * Author             : at-node
 * Description        : HWS IR — 38 kHz infrared transmitter (PWM4 + TMR1).
 ********************************************************************************/

#ifndef HWS_IR_H
#define HWS_IR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

void hws_ir_init(void);
int  hws_ir_nec(uint32_t code);                 /* NEC 32-bit, LSB-first */
int  hws_ir_sirc(uint32_t code, uint8_t bits);  /* Sony SIRC */
int  hws_ir_raw(const uint16_t *us, uint8_t n); /* mark/space µs pairs  */
uint8_t hws_ir_busy(void);                      /* 1 while a frame is on air */

#ifdef __cplusplus
}
#endif

#endif /* HWS_IR_H */
