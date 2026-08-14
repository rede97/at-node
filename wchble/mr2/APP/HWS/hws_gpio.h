/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_gpio.h
 * Author             : at-node
 * Description        : HWS GPIO — linear-pin digital IO for AT commands.
 *
 *   Pin model: 0-15 = PA0-PA15, 16-39 = PB0-PB23.
 *   Modes are fixed per direction (write = push-pull out 5 mA,
 *   read = pull-up input) to keep the AT surface agent-simple.
 ********************************************************************************/

#ifndef HWS_GPIO_H
#define HWS_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

/* returns 0 ok, -1 bad pin */
int hws_gpio_write(uint8_t pin, uint8_t level, uint8_t drive20);
int hws_gpio_read(uint8_t pin, uint8_t mode);   /* 0/1, -1 bad; mode: 0=PU 1=FLOAT 2=PD */

#ifdef __cplusplus
}
#endif

#endif /* HWS_GPIO_H */
