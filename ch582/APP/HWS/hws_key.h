/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_key.h
 * Author             : at-node
 * Description        : Hardware key scanning — polled input, change detection, callback
 ********************************************************************************/

#ifndef __HWS_KEY_H
#define __HWS_KEY_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * MACROS
 */
#define HWS_KEY_POLL_MS    100

/* Key bitmask */
#define HWS_KEY_SW_1  0x01
#define HWS_KEY_SW_2  0x02
#define HWS_KEY_SW_3  0x04
#define HWS_KEY_SW_4  0x08

/* Key pin definitions */
#define KEY1_BV        BV(22)
#define KEY2_BV        BV(4)
#define KEY3_BV        (0)
#define KEY4_BV        (0)

#define KEY1_PU        (R32_PB_PU |= KEY1_BV)
#define KEY2_PU        (R32_PB_PU |= KEY2_BV)
#define KEY3_PU        ()
#define KEY4_PU        ()

#define KEY1_DIR       (R32_PB_DIR &= ~KEY1_BV)
#define KEY2_DIR       (R32_PB_DIR &= ~KEY2_BV)
#define KEY3_DIR       ()
#define KEY4_DIR       ()

#define KEY1_IN        (ACTIVE_LOW(R32_PB_PIN & KEY1_BV))
#define KEY2_IN        (ACTIVE_LOW(R32_PB_PIN & KEY2_BV))
#define KEY3_IN        (0)
#define KEY4_IN        (0)

#define HWS_PUSH_BUTTON1()  (KEY1_IN)
#define HWS_PUSH_BUTTON2()  (KEY2_IN)
#define HWS_PUSH_BUTTON3()  (0)
#define HWS_PUSH_BUTTON4()  (0)

/*********************************************************************
 * TYPEDEFS
 */
typedef void (*hws_key_cb_t)(uint8_t keys);

/*********************************************************************
 * FUNCTIONS
 */

/** @brief  Initialize key scanning hardware and TMOS poll task */
void hws_key_init(void);

/** @brief  Poll keys — called periodically by HWS task */
void hws_key_poll(void);

/** @brief  Register key-change callback */
void hws_key_config(hws_key_cb_t cback);

/** @brief  Read current key state (no callback, no change detection) */
uint8_t hws_key_read(void);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* __HWS_KEY_H */
