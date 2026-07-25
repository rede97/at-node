/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_gpio.c
 * Author             : at-node
 * Description        : HWS GPIO — linear-pin digital IO (see hws_gpio.h).
 ********************************************************************************/

#include "hws_gpio.h"

#if(defined(HWS_GPIO)) && (HWS_GPIO == TRUE)

#define HWS_GPIO_P_LAST  39   /* PB23 */

int hws_gpio_write(uint8_t pin, uint8_t level, uint8_t drive20)
{
    if (pin > HWS_GPIO_P_LAST)
        return -1;
    uint32_t mode = drive20 ? GPIO_ModeOut_PP_20mA : GPIO_ModeOut_PP_5mA;
    if (pin < 16) {
        GPIOA_ModeCfg(1 << pin, mode);
        if (level) GPIOA_SetBits(1 << pin);
        else       GPIOA_ResetBits(1 << pin);
    } else {
        GPIOB_ModeCfg(1 << (pin - 16), mode);
        if (level) GPIOB_SetBits(1 << (pin - 16));
        else       GPIOB_ResetBits(1 << (pin - 16));
    }
    return 0;
}

/* mode: 0=pull-up (default), 1=floating, 2=pull-down */
int hws_gpio_read(uint8_t pin, uint8_t mode)
{
    if (pin > HWS_GPIO_P_LAST || mode > 2)
        return -1;
    GPIOModeTypeDef m = (mode == 1) ? GPIO_ModeIN_Floating :
                        (mode == 2) ? GPIO_ModeIN_PD : GPIO_ModeIN_PU;
    if (pin < 16) {
        GPIOA_ModeCfg(1 << pin, m);
        return GPIOA_ReadPortPin(1 << pin) ? 1 : 0;
    } else {
        GPIOB_ModeCfg(1 << (pin - 16), m);
        return GPIOB_ReadPortPin(1 << (pin - 16)) ? 1 : 0;
    }
}

#endif /* HWS_GPIO == TRUE */
