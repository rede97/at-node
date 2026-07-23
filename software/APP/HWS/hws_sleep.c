/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_sleep.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : Sleep mode entry and wake-up configuration (RTC wake)
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
#include "hws.h"

/*******************************************************************************
 * @fn          hws_sleep_enter
 *
 * @brief       Enter sleep — called by the BLE stack as cfg.sleepCB.
 *
 * @param   time    - wake-up instant (absolute RTC value)
 *
 * @return      state (0 = slept, 2 = duration out of range, 3 = RTC flag pending)
 */
uint32_t hws_sleep_enter(uint32_t time)
{
    uint32_t time_sleep, time_curr;
    unsigned long irq_status;
    
    SYS_DisableAllIrq(&irq_status);
    
    SYS_DisableAllIrq(&irq_status);
    time_curr = RTC_GetCycle32k();
    // compute sleep duration (handle RTC counter wrap)
    if (time < time_curr) {
        time_sleep = time + (RTC_TIMER_MAX_VALUE - time_curr);
    } else {
        time_sleep = time - time_curr;
    }

    // reject sleep if duration is outside the allowed window
    if ((time_sleep < SLEEP_RTC_MIN_TIME) ||
        (time_sleep > SLEEP_RTC_MAX_TIME)) {
        SYS_RecoverIrq(irq_status);
        return 2;
    }

    hws_rtc_set_trigger(time);
    SYS_RecoverIrq(irq_status);
#if(DEBUG == Debug_UART1) // wait for UART1 TX flush before sleeping
    while((R8_UART1_LSR & RB_LSR_TX_ALL_EMP) == 0)
    {
        __nop();
    }
#endif
    // low-power sleep mode
    if(!RTCTigFlag)
    {
        LowPower_Sleep(RB_PWR_RAM2K | RB_PWR_RAM30K | RB_PWR_EXTEND);
        if(RTCTigFlag) // if woken by something other than RTC, note the 32M crystal may not be stable yet
        {
            time += WAKE_UP_RTC_MAX_TIME;
            if(time > 0xA8C00000)
            {
                time -= 0xA8C00000;
            }
            hws_rtc_set_trigger(time);
            LowPower_Idle();
        }
        HSECFG_Current(HSE_RCur_100); // back to rated current (low-power fn raised HSE bias)
    }
    else
    {
        return 3;
    }
    return 0;
}

/*******************************************************************************
 * @fn          hws_sleep_at
 *
 * @brief       Timed low-power sleep for AT+SLEEP (runtime, any build).
 *
 *   Unlike hws_sleep_enter (the BLE stack's sleepCB, HWS_SLEEP builds),
 *   this path is driven manually: the CALLER must first drop the BLE
 *   link and detach USB. Execution resumes here after the RTC wake.
 *
 *   @param  mode     - 0=Idle 1=Sleep 2=Shutdown (RAM retained)
 *   @param  seconds  - wake delay (caller clamps)
 *   @return 0 on wake
 */
uint32_t hws_sleep_at(uint8_t mode, uint32_t seconds)
{
    uint32_t wake = RTC_GetCycle32k() + MS_TO_RTC(seconds * 1000);
    hws_rtc_set_trigger(wake);

    if (mode == 0)      LowPower_Idle();
    else if (mode == 1) LowPower_Sleep(RB_PWR_RAM2K | RB_PWR_RAM30K | RB_PWR_EXTEND);
    else                LowPower_Shutdown(RB_PWR_RAM2K | RB_PWR_RAM30K | RB_PWR_EXTEND);

    HSECFG_Current(HSE_RCur_100);
    return 0;
}

/*******************************************************************************
 * @fn      hws_sleep_init
 *
 * @brief   Configure sleep wake-up source — RTC wake, trigger mode.
 *
 * @param   None.
 *
 * @return  None.
 */
void hws_sleep_init(void)
{
#if(defined(HWS_SLEEP)) && (HWS_SLEEP == TRUE)
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_SLP_RTC_WAKE; // RTC wake-up
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL |= RB_RTC_TRIG_EN;  // trigger mode
    sys_safe_access_disable();
    PFIC_EnableIRQ(RTC_IRQn);
#endif
}
