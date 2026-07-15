/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_led.h
 * Author             : at-node
 * Description        : LED control — ON/OFF/BLINK/FLASH/TOGGLE with TMOS-driven timing
 ********************************************************************************/

#ifndef __HWS_LED_H
#define __HWS_LED_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * LED bitmask
 */
#define HWS_LED_1   0x01
#define HWS_LED_2   0x02
#define HWS_LED_3   0x04
#define HWS_LED_4   0x08
#define HWS_LED_ALL (HWS_LED_1 | HWS_LED_2 | HWS_LED_3 | HWS_LED_4)

/* Modes */
#define HWS_LED_MODE_OFF    0x00
#define HWS_LED_MODE_ON     0x01
#define HWS_LED_MODE_BLINK  0x02
#define HWS_LED_MODE_FLASH  0x04
#define HWS_LED_MODE_TOGGLE 0x08

/* Defaults */
#define HWS_LED_DEFAULT_MAX_LEDS    4
#define HWS_LED_DEFAULT_DUTY_CYCLE  5
#define HWS_LED_DEFAULT_FLASH_COUNT 50
#define HWS_LED_DEFAULT_FLASH_TIME  1000

/*********************************************************************
 * LED pin definitions (hardware-specific)
 */
#define LED1_BV     BV(8)
#define LED2_BV     (0)
#define LED3_BV     (0)

#define LED1_OUT    (R32_PA_OUT)

#define LED1_DDR    (R32_PA_DIR |= LED1_BV)
#define LED2_DDR    0
#define LED3_DDR    0

#define HWS_TURN_OFF_LED1()  (LED1_OUT |= LED1_BV)
#define HWS_TURN_OFF_LED2()
#define HWS_TURN_OFF_LED3()
#define HWS_TURN_OFF_LED4()

#define HWS_TURN_ON_LED1()   (LED1_OUT &= (~LED1_BV))
#define HWS_TURN_ON_LED2()
#define HWS_TURN_ON_LED3()
#define HWS_TURN_ON_LED4()

/*********************************************************************
 * FUNCTIONS
 */

/** @brief  Initialize LED hardware — all off */
void hws_led_init(void);

/** @brief  TMOS-driven LED state update (blink/flash timing) */
void hws_led_update(void);

/** @brief  Set LED mode: ON, OFF, TOGGLE, BLINK, FLASH */
uint8_t hws_led_set(uint8_t led, uint8_t mode);

/** @brief  Start LED blink sequence */
void hws_led_blink(uint8_t leds, uint8_t cnt, uint8_t duty, uint16_t time);

/** @brief  Return current LED on/off state bitmask */
uint8_t hws_led_get_state(void);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* __HWS_LED_H */
