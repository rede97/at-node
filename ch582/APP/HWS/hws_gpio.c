/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_gpio.c
 * Author             : at-node
 * Description        : HWS GPIO — linear-pin digital IO (see hws_gpio.h).
 ********************************************************************************/

#include "hws_gpio.h"

#if(defined(HWS_GPIO)) && (HWS_GPIO == TRUE)

#define HWS_GPIO_P_LAST  39   /* PB23 */

int hws_gpio_write(uint8_t pin, uint8_t level)
{
    if (pin > HWS_GPIO_P_LAST)
        return -1;
    if (pin < 16) {
        GPIOA_ModeCfg(1 << pin, GPIO_ModeOut_PP_5mA);
        if (level) GPIOA_SetBits(1 << pin);
        else       GPIOA_ResetBits(1 << pin);
    } else {
        GPIOB_ModeCfg(1 << (pin - 16), GPIO_ModeOut_PP_5mA);
        if (level) GPIOB_SetBits(1 << (pin - 16));
        else       GPIOB_ResetBits(1 << (pin - 16));
    }
    return 0;
}

int hws_gpio_read(uint8_t pin)
{
    if (pin > HWS_GPIO_P_LAST)
        return -1;
    if (pin < 16) {
        GPIOA_ModeCfg(1 << pin, GPIO_ModeIN_PU);
        return GPIOA_ReadPortPin(1 << pin) ? 1 : 0;
    } else {
        GPIOB_ModeCfg(1 << (pin - 16), GPIO_ModeIN_PU);
        return GPIOB_ReadPortPin(1 << (pin - 16)) ? 1 : 0;
    }
}

#endif /* HWS_GPIO == TRUE */
