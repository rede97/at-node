/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_sleep.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/11/12
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
#ifndef __HWS_SLEEP_H
#define __HWS_SLEEP_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * GLOBAL VARIABLES
 */

/*********************************************************************
 * FUNCTIONS
 */

/**
 * @brief   配置睡眠唤醒的方式   - RTC唤醒，触发模式
 */
void hws_sleep_init(void);

/**
 * @brief   启动睡眠
 *
 * @param   time    - 唤醒的时间点（RTC绝对值）
 *
 * @return  state.
 */
uint32_t hws_sleep_enter(uint32_t time);

/* hws_sleep_at — timed low-power sleep for AT+SLEEP (runtime, any
   build). mode: 0=Idle 1=Sleep 2=Shutdown (RAM retained); wakes via
   RTC after `seconds`. Caller must drop BLE + detach USB first. */
uint32_t hws_sleep_at(uint8_t mode, uint32_t seconds);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif
