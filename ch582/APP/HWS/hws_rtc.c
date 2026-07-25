/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_rtc.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : RTC configuration and init (TMOS timebase, trigger)
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
#include "hws.h"

/*********************************************************************
 * CONSTANTS
 */
#define RTC_INIT_TIME_HOUR      0
#define RTC_INIT_TIME_MINUTE    0
#define RTC_INIT_TIME_SECEND    0

/***************************************************
 * Global variables
 */
volatile uint32_t RTCTigFlag;

/*******************************************************************************
 * @fn      hws_rtc_set_trigger
 *
 * @brief   Configure RTC trigger time.
 *
 * @param   time    - trigger instant (absolute RTC value).
 *
 * @return  None.
 */
void hws_rtc_set_trigger(uint32_t time)
{
    sys_safe_access_enable();
    R32_RTC_TRIG = time;
    sys_safe_access_disable();
    RTCTigFlag = 0;
}

/*******************************************************************************
 * @fn      RTC_IRQHandler
 *
 * @brief   RTC interrupt handler — sets trigger flag.
 *
 * @param   None.
 *
 * @return  None.
 */
__INTERRUPT
__HIGH_CODE
void RTC_IRQHandler(void)
{
    R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);
    RTCTigFlag = 1;
}

/*******************************************************************************
 * @fn      hws_rtc_init
 *
 * @brief   System timer init — 32 kHz clock source + TMOS timebase.
 *
 * @param   None.
 *
 * @return  None.
 */
void hws_rtc_init(void)
{
#if(CLK_OSC32K)
    sys_safe_access_enable();
    R8_CK32K_CONFIG &= ~(RB_CLK_OSC32K_XT | RB_CLK_XT32K_PON);
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_CK32K_CONFIG |= RB_CLK_INT32K_PON;
    sys_safe_access_disable();
    LSECFG_Current(LSE_RCur_100);
    hws_ble_calibrate_lsi();
#else
    sys_safe_access_enable();
    R8_CK32K_CONFIG |= RB_CLK_OSC32K_XT | RB_CLK_INT32K_PON | RB_CLK_XT32K_PON;
    sys_safe_access_disable();
#endif
    RTC_InitTime(2020, 1, 1, 0, 0, 0); // init RTC calendar to a fixed epoch
    TMOS_TimerInit(0);
}

/******************************** endfile @ time ******************************/
