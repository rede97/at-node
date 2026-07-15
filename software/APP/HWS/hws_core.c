/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_core.c
 * Author             : at-node
 * Description        : HWS core — platform init, TMOS task processing,
 *                      temperature sensor, BLE LSI calibration.
 *
 *   This file implements the Hardware Services (HWS) task — the central
 *   TMOS event loop for all hardware subsystems. It also provides
 *   platform-level initialization that must run before any other init:
 *   power, clock tree, GPIO pads, and debug UART.
 *
 *   Init order (caller in main.c):
 *     1. hws_platform_init()     — power, clock, GPIO, debug UART
 *     2. ble_stack_init()        — BLE stack + TMOS (must be first!)
 *     3. hws_init(key_cb)        — registers HWS TMOS task (TMOS must be running)
 *
 *   ble_stack_init() MUST run before hws_init(): BLE_LibInit() inside
 *   ble_stack_init() initializes TMOS. hws_init() calls
 *   TMOS_ProcessEventRegister() which requires TMOS to be ready.
 *
 *   The calibration event (HWS_CALIB_EVENT) runs BLE RF and LSI calibration
 *   on a periodic timer. Both are hardware-level register maintenance even
 *   though the beneficiary is the BLE stack.
 ********************************************************************************/

#include "hws.h"

tmosTaskID hws_task_id;

/* ===== Platform initialization ===== */

/*******************************************************************************
 * @fn      hws_platform_init
 *
 * @brief   Initialize hardware platform: power, clock, GPIO, debug UART.
 *
 *   Init stage: 1 — called first, before anything else in main().
 *   Depends on: nothing (hardware registers accessible at power-on).
 *   Side effects:
 *     - Enables DC-DC converter if DCDC_ENABLE=TRUE (power saving).
 *     - Sets system clock to 60 MHz PLL (required by all peripherals).
 *     - If HWS_SLEEP=TRUE: configures all GPIO pins as input pull-up to
 *       minimize leakage during sleep. This must happen before any
 *       peripheral reconfigures specific pins.
 *     - If DEBUG: initializes UART1 TX (PA9) and RX (PA8) for printf output.
 *       UART1 is shared with the AT command interface.
 *
 *   CONSTRAINT: USB and sleep are mutually exclusive on CH582F.
 *   When HWS_SLEEP=TRUE, USB is disabled at compile time (clock stops
 *   in sleep → USB enumeration lost → Windows code 43).
 */
void hws_platform_init(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    /* DC-DC converter reduces power consumption vs LDO regulator.
       Must be configured before any high-current peripherals start. */
    PWR_DCDCCfg(ENABLE);
#endif

    /* 60 MHz PLL — required by BLE stack (timing-critical),
       USB (48 MHz derived clock), and all peripheral bus clocks. */
    SetSysClock(CLK_SOURCE_PLL_60MHz);

#if(defined(HWS_SLEEP)) && (HWS_SLEEP == TRUE)
    /* Set all GPIO to input pull-up before any peripheral uses them.
       Prevents floating inputs from causing leakage in sleep modes.
       Individual peripherals will reconfigure their pins later. */
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
#endif

#ifdef DEBUG
    /* UART1: TX=PA9 (push-pull), RX=PA8 (input pull-up).
       Used for PRINT() debug output and AT command UART channel.
       Baud rate and frame format are set by UART1_DefInit(). */
    GPIOA_SetBits(bTXD1);
    GPIOA_ModeCfg(bTXD1, GPIO_ModeOut_PP_5mA);
    GPIOA_ModeCfg(bRXD1, GPIO_ModeIN_PU);
    UART1_DefInit();
#endif
}

/* ===== BLE support utilities (called by BLE stack as callbacks) ===== */

/*******************************************************************************
 * @fn      hws_ble_calibrate_lsi
 *
 * @brief   Calibrate internal 32 kHz LSI oscillator using 32 MHz HSE as reference.
 *
 *   Called as: BLE stack callback (cfg.rcCB) — invoked by BLE lib when
 *   temperature change exceeds threshold (TEM_SAMPLE config).
 *   Also called by: periodic HWS_CALIB_EVENT task.
 *
 *   WHY THIS EXISTS:
 *     The CH582F internal 32 kHz RC oscillator (LSI) has poor accuracy
 *     (±2% uncalibrated) and drifts with temperature. BLE sleep timing
 *     requires ±500 ppm accuracy for stable connection events.
 *
 *     The 32 MHz external crystal (HSE) is ±20 ppm stable — 100× more
 *     accurate than uncalibrated LSI. This function uses HSE as a
 *     frequency counter gate: it counts LSI cycles within a known HSE
 *     period, then adjusts the LSI trim registers to match the nominal
 *     32.0 kHz target. The result is ±800–1000 ppm — good enough for
 *     BLE Peripheral operation.
 *
 *   DEPENDENCY CHAIN:
 *     32 MHz HSE crystal (±20 ppm)
 *       → calibrates 32 kHz LSI RC (±800 ppm after calib)
 *         → LSI clocks RTC (free-running during sleep)
 *           → RTC wakes MCU at precise BLE connection instants
 *             → BLE connection stays alive across temperature changes
 *
 *   Without this calibration, the LSI can drift enough after ~7°C
 *   temperature change that BLE connection events miss their timing
 *   windows → connection drops. Internal RC is fast-starting and
 *   low-power, but not inherently accurate — that's what the crystal fixes.
 *
 *   Side effects: Modifies LSI trim registers. Takes <10 ms.
 */
void hws_ble_calibrate_lsi(void)
{
    Calibration_LSI(Level_64);
}

/* ===== HWS TMOS task ===== */

/*******************************************************************************
 * @fn      hws_process_event
 *
 * @brief   HWS TMOS task event processor — dispatches hardware events.
 *
 *   Init stage: registered as TMOS task by hws_init(). Runs in TMOS loop.
 *   Events handled:
 *     SYS_EVENT_MSG       — TMOS message queue dispatch (currently unused).
 *     HWS_LED_BLINK_EVENT — LED blink/flash state machine update.
 *     HWS_KEY_EVENT       — Key scanning poll (100 ms periodic).
 *     HWS_CALIB_EVENT     — BLE RF + LSI calibration (periodic, default 120 s).
 *     HWS_TEST_EVENT      — Heartbeat PRINT("* ") every 1 s (debug aid).
 *
 *   Each event handler returns (events ^ handled_event) to mark it processed.
 *   Unhandled events are returned to TMOS for other tasks.
 *
 *   @param  task_id  TMOS task ID assigned at registration.
 *   @param  events   Bitmask of pending events.
 *   @return Bitmask of unprocessed events (0 = all handled).
 */
tmosEvents hws_process_event(tmosTaskID task_id, tmosEvents events)
{
    uint8_t *msgPtr;

    if(events & SYS_EVENT_MSG)
    {
        msgPtr = tmos_msg_receive(task_id);
        if(msgPtr)
        {
            tmos_msg_deallocate(msgPtr);
        }
        return events ^ SYS_EVENT_MSG;
    }
    if(events & HWS_LED_BLINK_EVENT)
    {
#if(defined HWS_LED) && (HWS_LED == TRUE)
        hws_led_update();
#endif
        return events ^ HWS_LED_BLINK_EVENT;
    }
    if(events & HWS_KEY_EVENT)
    {
#if(defined HWS_KEY) && (HWS_KEY == TRUE)
        hws_key_poll();
        /* Re-arm key poll timer: 100 ms periodic scan.
           Faster than debounce time (~50 ms), slower than TMOS tick (625 us). */
        tmos_start_task(hws_task_id, HWS_KEY_EVENT, MS1_TO_SYSTEM_TIME(100));
        return events ^ HWS_KEY_EVENT;
#endif
    }
    if(events & HWS_CALIB_EVENT)
    {
        uint8_t x32Kpw;
#if(defined BLE_CALIBRATION_ENABLE) && (BLE_CALIBRATION_ENABLE == TRUE)
        /* BLE RF calibration — compensates for temperature drift in the
           2.4 GHz frontend. BLE_RegInit() is provided by libCH58xBLE.a. */
        BLE_RegInit();
#if(CLK_OSC32K)
        /* Internal 32K RC oscillator calibration.
           When using external 32K crystal (CLK_OSC32K=0), adjust LSE drive
           current instead — reduces power without losing accuracy. */
        hws_ble_calibrate_lsi();
#else
        x32Kpw = (R8_XT32K_TUNE & 0xfc) | 0x01;
        sys_safe_access_enable();
        R8_XT32K_TUNE = x32Kpw;
        sys_safe_access_disable();
#endif
        /* Schedule next calibration. Default period: BLE_CALIBRATION_PERIOD (120 s).
           Each calibration takes <10 ms — negligible power/performance impact. */
        tmos_start_task(hws_task_id, HWS_CALIB_EVENT, MS1_TO_SYSTEM_TIME(BLE_CALIBRATION_PERIOD));
        return events ^ HWS_CALIB_EVENT;
#endif
    }
    if(events & HWS_TEST_EVENT)
    {
        /* Heartbeat — prints "* " every second. Useful for verifying
           TMOS scheduler is running and device hasn't crashed.
           Remove in production to save UART bandwidth. */
        PRINT("* \n");
        tmos_start_task(hws_task_id, HWS_TEST_EVENT, MS1_TO_SYSTEM_TIME(1000));
        return events ^ HWS_TEST_EVENT;
    }
    return 0;
}

/* ===== Hardware services initialization ===== */

/*******************************************************************************
 * @fn      hws_init
 *
 * @brief   Initialize all hardware subsystems and register HWS TMOS task.
 *
 *   Init stage: 3 — called after ble_stack_init(), before at_init().
 *   Depends on: ble_stack_init() — TMOS scheduler must be running
 *               (BLE_LibInit initializes TMOS inside ble_stack_init).
 *               hws_platform_init() — clock must be 60 MHz.
 *   Side effects:
 *     - Registers HWS TMOS task (hws_process_event) with TMOS scheduler.
 *     - Initializes RTC timer (TMOS timebase).
 *     - Configures sleep wake-up source (RTC trigger) if HWS_SLEEP=TRUE.
 *     - Initializes LED GPIO and sets all LEDs off.
 *     - Initializes key scanning GPIO, starts 100 ms poll timer.
 *     - Registers key callback (if non-NULL) before first poll fires.
 *     - Schedules first BLE calibration event (800 ms deferred).
 *
 *   @param  key_cb  Key change callback (hws_key_cb_t).
 *                   Called when a key state changes (press or release).
 *                   Pass NULL if no key callback is needed.
 */
void hws_init(hws_key_cb_t key_cb)
{
    /* Register HWS task with TMOS — must be first, all subsystems
       reference hws_task_id for scheduling timers. */
    hws_task_id = TMOS_ProcessEventRegister(hws_process_event);

    /* RTC timer — provides TMOS timebase (TMOS_GetSystemClock).
       Must be initialized before any subsystem that uses timers
       (sleep, LED blink, key poll, calibration). */
    hws_rtc_init();

#if(defined HWS_SLEEP) && (HWS_SLEEP == TRUE)
    /* Sleep config — enables RTC-triggered wake-up.
       Actual sleep entry is controlled by BLE stack via sleepCB callback.
       Must be called before ble_stack_init() so sleepCB is valid. */
    hws_sleep_init();
#endif

#if(defined HWS_LED) && (HWS_LED == TRUE)
    /* LED init — configures GPIO output, sets all LEDs off.
       TMOS-driven blink/flash timing uses hws_task_id. */
    hws_led_init();
#endif

#if(defined HWS_KEY) && (HWS_KEY == TRUE)
    /* Key init — configures GPIO inputs with pull-ups,
       starts the periodic key scan timer.
       Register callback immediately so no key event is missed. */
    hws_key_init();
    if(key_cb) {
        hws_key_config(key_cb);
    }
#endif

#if(defined BLE_CALIBRATION_ENABLE) && (BLE_CALIBRATION_ENABLE == TRUE)
    /* Schedule first RF calibration 800 ms after boot.
       Deferred: gives BLE stack time to initialize before calibrating.
       Subsequent calibrations every BLE_CALIBRATION_PERIOD ms. */
    tmos_start_task(hws_task_id, HWS_CALIB_EVENT, 800);
#endif
}

/* ===== Temperature sensor ===== */

/*******************************************************************************
 * @fn      hws_get_temp
 *
 * @brief   Read internal temperature sensor ADC value.
 *
 *   Called as: BLE stack callback (cfg.tsCB) — invoked by BLE lib to
 *   detect temperature changes that require RF re-calibration.
 *
 *   The CH582F internal temperature sensor shares the ADC with other
 *   peripherals. This function saves/restores ADC configuration to
 *   avoid interfering with concurrent ADC operations.
 *
 *   Side effects: Temporarily reconfigures ADC channel and trigger.
 *                 Restores original config before returning.
 *                 Blocks until ADC conversion completes (~10 µs).
 *
 *   @return Raw 12-bit ADC value (higher = warmer).
 *           BLE stack checks if delta exceeds ~7°C threshold.
 */
uint16_t hws_get_temp(void)
{
    uint8_t  sensor, channel, config, tkey_cfg;
    uint16_t adc_data;

    /* Save current ADC configuration */
    tkey_cfg = R8_TKEY_CFG;
    sensor   = R8_TEM_SENSOR;
    channel  = R8_ADC_CHANNEL;
    config   = R8_ADC_CFG;

    /* Configure ADC for internal temperature sensor */
    ADC_InterTSSampInit();
    R8_ADC_CONVERT |= RB_ADC_START;

    /* Wait for conversion complete */
    while(R8_ADC_CONVERT & RB_ADC_START);

    adc_data = R16_ADC_DATA;

    /* Restore previous ADC configuration */
    R8_TEM_SENSOR = sensor;
    R8_ADC_CHANNEL = channel;
    R8_ADC_CFG = config;
    R8_TKEY_CFG = tkey_cfg;

    return (adc_data);
}
