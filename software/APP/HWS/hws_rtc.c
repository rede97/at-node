/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_rtc.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : RTC配置及其初始化
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
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
 * @brief   配置RTC触发时间
 *
 * @param   time    - 触发时间.
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
 * @brief   RTC中断处理
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
 * @brief   系统定时器初始化
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
    RTC_InitTime(2020, 1, 1, 0, 0, 0); //RTC时钟初始化当前时间
    TMOS_TimerInit(0);
}

/******************************** endfile @ time ******************************/
