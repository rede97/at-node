/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws.h
 * Author             : at-node
 * Description        : HWS (Hardware Services) umbrella header.
 *
 *   Includes all hardware service subsystems. Application code should
 *   include this header rather than individual hws_*.h files.
 *
 *   Subsystems:
 *     hws_core  — platform init, TMOS task, temp sensor, calibration
 *     hws_led   — LED ON/OFF/BLINK/FLASH/TOGGLE with TMOS-driven timing
 *     hws_key   — key scanning with change detection and callback
 *     hws_rtc   — RTC timer (TMOS timebase) and trigger configuration
 *     hws_sleep — low-power sleep mode entry (RTC wake-up)
 ********************************************************************************/

#ifndef __HWS_H
#define __HWS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "hws_rtc.h"
#include "hws_sleep.h"
#include "hws_led.h"
#include "hws_key.h"

/* HWS task events — bitmask flags for TMOS event dispatch.
   Each subsystem uses one event bit. Bits must not overlap
   with other TMOS tasks (AT, BLE HID) or SYS_EVENT_MSG. */
#define HWS_LED_BLINK_EVENT  0x0001  /* LED state machine update */
#define HWS_KEY_EVENT        0x0002  /* key scan poll (100 ms) */
#define HWS_CALIB_EVENT      0x2000  /* BLE RF + LSI calibration (periodic) */
#define HWS_TEST_EVENT       0x4000  /* heartbeat PRINT debug aid */

/*********************************************************************
 * GLOBAL VARIABLES
 */

/* TMOS task ID for the HWS event loop.
   Set by hws_init(), referenced by all HWS subsystems for
   scheduling timers via tmos_start_task(). */
extern tmosTaskID hws_task_id;

/*********************************************************************
 * FUNCTIONS — Init stages 1–2: platform then services
 */

/**
 * @brief   [Stage 1] Platform init: power, clock, GPIO pads, debug UART.
 *
 *   Called first in main(). No dependencies. Sets up everything
 *   needed before any peripheral or BLE stack initialization.
 *
 *   See hws_core.c for detailed constraints (sleep vs USB, UART sharing).
 */
void hws_platform_init(void);

/**
 * @brief   [Stage 3] Initialize all hardware services, register HWS TMOS task.
 *
 *   Called after ble_stack_init() (which initializes TMOS via BLE_LibInit),
 *   before at_init(). TMOS must be running before this call.
 *   Initializes RTC → sleep config → LED → KEY (with callback) →
 *   schedules calibration task.
 *
 *   @param  key_cb  Key-change callback (hws_key_cb_t signature).
 *                   NULL if no key monitoring needed.
 *                   Registered before the first key poll fires.
 */
void hws_init(hws_key_cb_t key_cb);

/* --- TMOS task event processor --- */

/**
 * @brief   HWS TMOS task event processor — dispatches LED/KEY/CALIB/TEST events.
 *
 *   Registered as TMOS task by hws_init(). Called by TMOS scheduler
 *   when any HWS event bit is set. Each handled event is cleared
 *   from the return value.
 *
 *   @return Bitmask of unhandled events (0 = all processed).
 */
tmosEvents hws_process_event(tmosTaskID task_id, tmosEvents events);

/* --- BLE support callbacks --- */

/**
 * @brief   Read internal temperature sensor (ADC).
 *
 *   Called by BLE stack as cfg.tsCB. Saves/restores ADC config
 *   to avoid interference with other ADC users. Blocks ~10 µs.
 *   @return Raw 12-bit ADC value.
 */
uint16_t hws_get_temp(void);

/**
 * @brief   Calibrate internal 32 kHz LSI oscillator for BLE sleep timing.
 *
 *   Called by BLE stack as cfg.rcCB and by HWS_CALIB_EVENT.
 *   Re-tunes the internal RC oscillator against the crystal HSE.
 *   Takes <10 ms. Required for stable BLE connections across
 *   temperature changes.
 */
void hws_ble_calibrate_lsi(void);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* __HWS_H */
