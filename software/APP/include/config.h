/********************************** (C) COPYRIGHT *******************************
 * File Name          : config.h
 * Author             : at-node
 * Description        : Global compile-time configuration.
 *
 *   All configuration macros use the #ifndef pattern: define a default
 *   here, but allow override via compiler -D flag (MounRiver Studio:
 *   Project → Properties → C/C++ Build → Settings → GNU C → Symbols).
 *   This avoids editing this file for per-build customization.
 *
 *   Config groups (see detailed sections below):
 *     PLATFORM  — chip identity, power, sleep, hardware subsystems
 *     BLE RADIO — MAC address, Tx power, RF/temperature calibration
 *     BLE STACK — SNV bonding storage, memory heap, buffer sizing
 *     BLE TIMING — RTC clock source, connection parameters
 *     USB       — mutually exclusive with sleep (hardware constraint)
 *
 *   File is UTF-8 without BOM. Comments in English only.
 ********************************************************************************/

#ifndef __CONFIG_H
#define __CONFIG_H

/* ====================================================================
 * 1. PLATFORM — chip identity and board-level config
 * ====================================================================
 *
 *   These must be set before any SDK includes. They identify the
 *   MCU variant and pull in the correct register definitions.
 */

/* CH583 is the superset chip; CH582F is a subset with fewer peripherals.
   The SDK treats them identically for core peripherals (GPIO, UART, USB, BLE). */
#define ID_CH583  0x83
#define CHIP_ID   ID_CH583

/* BLE stack header — ROM version uses on-die mask ROM (smaller flash footprint
   but fixed version); LIB version links against libCH58xBLE.a (larger but
   upgradeable). Default: LIB for development flexibility. */
#ifdef CH58xBLE_ROM
#include "CH58xBLE_ROM.H"
#else
#include "CH58xBLE_LIB.H"
#endif

/* Peripheral register definitions and utility macros (BV, ACTIVE_LOW, etc.) */
#include "CH58x_common.h"

/* ====================================================================
 * 2. POWER — DC-DC converter and sleep
 * ====================================================================
 */

/*********************************************************************
 * DCDC_ENABLE — DC-DC converter for internal 1.5V core supply.
 *
 *   TRUE:  DC-DC mode. Higher efficiency (~85%), lower power.
 *          Requires external inductor on LX pin.
 *   FALSE: LDO mode. Lower efficiency (~50%), simpler BOM.
 *          Slightly better ripple for ADC measurements.
 *
 *   Must be configured before any high-current peripheral starts.
 *   Called in hws_platform_init(). Default: TRUE for battery operation.
 */
#ifndef DCDC_ENABLE
#define DCDC_ENABLE  TRUE
#endif

/*********************************************************************
 * HWS_SLEEP — BLE-controlled low-power sleep mode.
 *
 *   TRUE:  Device enters RTC-timed sleep between BLE connection events.
 *          Wakes on RTC trigger, services BLE, goes back to sleep.
 *          USB is COMPLETELY DISABLED — sleep stops USB clock, Windows
 *          loses enumeration (code 43 on device manager).
 *   FALSE: Device runs continuously. USB available. Higher power draw.
 *
 *   USB + Sleep mutual exclusion is a hardware limitation:
 *   USB PHY needs a running 48 MHz clock derived from PLL, but sleep
 *   gates PLL to save power. Waking for each SOF (every 1 ms on USB FS)
 *   would be slower than just staying awake — sleep is not useful with
 *   USB active. Consequently: HWS_SLEEP=TRUE disables USB at compile time.
 *
 *   Default: FALSE (development with USB debug/AT commands).
 *   Set TRUE for battery-powered BLE-only deployment.
 */
#ifndef HWS_SLEEP
#define HWS_SLEEP  FALSE
#endif

/*********************************************************************
 * SLEEP_RTC_MIN_TIME — minimum sleep duration in RTC ticks.
 *
 *   If projected sleep time < this value, skip sleep (wake overhead
 *   costs more than staying awake). Default: 1000 µs.
 */
#ifndef SLEEP_RTC_MIN_TIME
#define SLEEP_RTC_MIN_TIME  US_TO_RTC(1000)
#endif

/*********************************************************************
 * SLEEP_RTC_MAX_TIME — maximum sleep duration in RTC ticks.
 *
 *   Safety cap to prevent arithmetic overflow in sleep time calculation.
 *   Default: RTC max minus 1 hour (leaves headroom for timer wrap).
 */
#ifndef SLEEP_RTC_MAX_TIME
#define SLEEP_RTC_MAX_TIME  MS_TO_RTC(RTC_TO_MS(RTC_TIMER_MAX_VALUE) - 1000 * 60 * 60)
#endif

/*********************************************************************
 * WAKE_UP_RTC_MAX_TIME — HSE stabilization delay after wake.
 *
 *   After sleep, the 32 MHz crystal oscillator needs time to stabilize
 *   before RF or high-speed peripherals are used. This value is the
 *   guard interval in RTC ticks added to the wake timer.
 *
 *   Default: 1400 µs (45 RTC cycles at 32 kHz).
 *   Typical values by sleep depth:
 *     Sleep/Power-down: 45  RTC cycles (HSE restart needed)
 *     Halt:             45  RTC cycles (HSE restart needed)
 *     Idle:              5  RTC cycles (HSE kept running)
 */
#ifndef WAKE_UP_RTC_MAX_TIME
#define WAKE_UP_RTC_MAX_TIME  US_TO_RTC(1400)
#endif

/* ====================================================================
 * 3. HARDWARE SERVICES — compile-time enable/disable subsystems
 * ====================================================================
 *
 *   Disabling unused subsystems saves flash (code excluded by #if)
 *   and RAM (no TMOS timer entries). Each flag gates the corresponding
 *   hws_*_init() call in hws_init() and its TMOS event processing.
 */

/*********************************************************************
 * HWS_KEY — key scanning subsystem (polled, 100 ms interval).
 *
 *   Disable if board has no physical buttons or if GPIO pins used for
 *   key inputs are needed for other peripherals.
 */
#ifndef HWS_KEY
#define HWS_KEY  TRUE
#endif

/*********************************************************************
 * HWS_LED — LED control subsystem (ON/OFF/BLINK/FLASH/TOGGLE).
 *
 *   Disable if board has no LEDs or LED GPIO pins are repurposed.
 *   Saves one TMOS timer when disabled (HWS_LED_BLINK_EVENT not scheduled).
 */
#ifndef HWS_LED
#define HWS_LED  TRUE
#endif

/* ====================================================================
 * 4. DEBUG — printf output over UART1
 * ====================================================================
 */

/*********************************************************************
 * DEBUG — enable debug output on UART1 (PA8=RX, PA9=TX).
 *
 *   When defined: PRINT() macro maps to printf() with \r line ending.
 *   When undefined: PRINT() is a no-op (saves flash and UART1 pins).
 *
 *   UART1 is shared with AT command interface — if DEBUG is on,
 *   AT commands must also use UART1 (not CDC-only mode).
 *
 *   Default: defined via -DDEBUG=1 in compiler flags (MounRiver Studio).
 *   Override in Project → Properties → Symbols to disable.
 */
#ifdef DEBUG
#undef PRINT
#define PRINT(fmt, ...)  printf(fmt "\r", ##__VA_ARGS__)
#endif

/* ====================================================================
 * 5. BLE RADIO — MAC address, Tx power, RF calibration
 * ====================================================================
 */

/*********************************************************************
 * BLE_MAC — use a custom Bluetooth MAC address.
 *
 *   FALSE: use factory-programmed chip MAC address (unique per device).
 *   TRUE:  use MacAddr[] array defined in main.c.
 *          Must be a valid IEEE 802.15.4 address (MSB = 0b10 for static
 *          random, or 0b00 for public). Used in BLE advertising.
 *
 *   Custom MAC is useful when: (a) multiple units share firmware image
 *   but need known addresses, (b) factory MAC is not accessible.
 *   Default: FALSE (use chip MAC).
 */
#ifndef BLE_MAC
#define BLE_MAC  FALSE
#endif

/*********************************************************************
 * BLE_TX_POWER — BLE transmit power in dBm.
 *
 *   Options (from BLE stack lib): LL_TX_POWEER_0_DBM (0 dBm),
 *   LL_TX_POWEER_MINUS_3_DBM, LL_TX_POWEER_MINUS_6_DBM, etc.
 *
 *   Trade-off: higher power = longer range (up to ~30m at 0 dBm),
 *   lower power = longer battery life. Each -3 dB roughly doubles
 *   battery life on Tx.
 *
 *   Default: 0 dBm (good range for desktop peripheral use).
 */
#ifndef BLE_TX_POWER
#define BLE_TX_POWER  LL_TX_POWEER_0_DBM
#endif

/*********************************************************************
 * TEM_SAMPLE — temperature-triggered RF calibration.
 *
 *   TRUE:  BLE stack monitors internal temperature sensor via
 *          cfg.tsCB (→ hws_get_temp). When temperature changes
 *          by ~7°C, triggers RF frontend re-calibration (BLE_RegInit)
 *          and optionally LSI re-calibration (cfg.rcCB).
 *          Keeps BLE connection stable across environment changes.
 *   FALSE: Skip temperature monitoring. Saves ADC power but risks
 *          BLE connection drops if device moves between hot/cold areas.
 *
 *   Default: TRUE. Required for reliable BLE in non-climate-controlled
 *   environments (outdoors, near heat sources, direct sunlight).
 */
#ifndef TEM_SAMPLE
#define TEM_SAMPLE  TRUE
#endif

/*********************************************************************
 * BLE_CALIBRATION_ENABLE — periodic RF + LSI calibration.
 *
 *   TRUE:  HWS TMOS task fires HWS_CALIB_EVENT every
 *          BLE_CALIBRATION_PERIOD ms. Each calibration calls
 *          BLE_RegInit() (RF frontend) + hws_ble_calibrate_lsi()
 *          (32 kHz oscillator). Single calib takes <10 ms.
 *   FALSE: No periodic calibration. BLE may drift over hours.
 *          Only disable for power-constrained devices that
 *          connect briefly and disconnect (< 1 minute sessions).
 *
 *   Default: TRUE. Needed for stable BLE connections lasting
 *   more than a few minutes (temperature drift accumulates).
 */
#ifndef BLE_CALIBRATION_ENABLE
#define BLE_CALIBRATION_ENABLE  TRUE
#endif

/*********************************************************************
 * BLE_CALIBRATION_PERIOD — interval between periodic calibrations.
 *
 *   Unit: milliseconds. Default: 120000 (2 minutes).
 *   Shorter = more stable BLE, more power consumed in calibration.
 *   Longer = less power, risk of drift between calibrations.
 *
 *   The first calibration fires 800 ms after boot (hardcoded in
 *   hws_init). Subsequent calibrations use this period.
 */
#ifndef BLE_CALIBRATION_PERIOD
#define BLE_CALIBRATION_PERIOD  120000
#endif

/* ====================================================================
 * 6. BLE STACK — memory, buffers, connections
 * ====================================================================
 */

/*********************************************************************
 * BLE_MEMHEAP_SIZE — BLE protocol stack heap allocation.
 *
 *   Unit: bytes. Minimum: 6144 (6 KB). Must be 4-byte aligned.
 *   This heap is allocated at the top of RAM → main.c MEM_BUF[].
 *
 *   The BLE stack uses this for: connection contexts, GATT database,
 *   ATT/GATT transaction buffers, security keys, advertising data.
 *
 *   Increasing this value: supports more concurrent connections,
 *   larger GATT databases, or longer ATT MTU.
 *   Decreasing: frees RAM for application, but must stay ≥ 6 KB.
 *
 *   Default: 6 KB (minimum). AT-Node is a single-connection peripheral
 *   with a small GATT database, so minimum is sufficient.
 */
#ifndef BLE_MEMHEAP_SIZE
#define BLE_MEMHEAP_SIZE  (1024 * 6)
#endif

/*********************************************************************
 * BLE_BUFF_MAX_LEN — maximum data packet length per connection.
 *
 *   Unit: bytes. Range: 27–516.
 *   27 = ATT_MTU of 23 bytes (minimum per BLE spec).
 *   Larger values allow bigger GATT notifications (e.g., HID reports
 *   with raw input data). Requires both sides to support larger MTU
 *   via ATT MTU exchange.
 *
 *   Default: 27 (minimum). HID keyboard reports are 8 bytes — no
 *   benefit from larger buffers for this application.
 */
#ifndef BLE_BUFF_MAX_LEN
#define BLE_BUFF_MAX_LEN  27
#endif

/*********************************************************************
 * BLE_BUFF_NUM — number of packets the controller can buffer.
 *
 *   Higher values smooth out BLE traffic bursts but consume more RAM.
 *   For HID keyboard (low data rate, periodic reports), 5 is plenty.
 *
 *   Default: 5.
 */
#ifndef BLE_BUFF_NUM
#define BLE_BUFF_NUM  5
#endif

/*********************************************************************
 * BLE_TX_NUM_EVENT — packets per connection event.
 *
 *   Maximum number of data packets the controller can send in a single
 *   BLE connection event. Higher = lower latency, higher power.
 *   For HID keyboard at 7.5 ms connection interval, 1 packet/event is
 *   sufficient (next event is only 7.5 ms away).
 *
 *   Default: 1.
 */
#ifndef BLE_TX_NUM_EVENT
#define BLE_TX_NUM_EVENT  1
#endif

/* ====================================================================
 * 7. BLE BONDING — SNV (Simplified Non-Volatile) storage
 * ====================================================================
 *
 *   SNV stores bonding information (LTK, IRK, CSRK, peer address)
 *   in on-chip Data Flash so the device remembers paired hosts
 *   across power cycles. Without SNV, re-pairing is required
 *   after every reset.
 */

/*********************************************************************
 * BLE_SNV — enable bonded device storage in Data Flash.
 *
 *   TRUE:  Bonding info saved to flash at BLE_SNV_ADDR.
 *          New pairing overwrites old bond (BLE_SNV_NUM=1).
 *   FALSE: No persistent bonding. Device forgets all pairings on reset.
 *          Host must re-pair after each power cycle.
 *
 *   Default: TRUE (normal HID keyboard behavior — pair once).
 */
#ifndef BLE_SNV
#define BLE_SNV  TRUE
#endif

/*********************************************************************
 * BLE_SNV_ADDR — SNV storage offset within Data Flash.
 *
 *   EEPROM_* commands take Data-Flash-RELATIVE offsets, not absolute
 *   addresses: offset 0x0000–0x7FFF maps to absolute 0x70000–0x77FFF
 *   (32 KB Data Flash on CH582F).
 *
 *   Default: 0x77E00 - FLASH_ROM_MAX_SIZE = 0x7E00 offset = absolute
 *   0x77E00 (last 512 bytes of Data Flash). Stays clear of application
 *   code (grows up from 0x00000) and BLE heap (in RAM, not flash).
 *   Bounds check in ble_stack_init() validates against the 32 KB
 *   Data Flash limit (0x78000 - FLASH_ROM_MAX_SIZE = 0x8000).
 */
#ifndef BLE_SNV_ADDR
#define BLE_SNV_ADDR  0x77E00 - FLASH_ROM_MAX_SIZE
#endif

/*********************************************************************
 * BLE_SNV_BLOCK — SNV block size in bytes.
 *
 *   Default: 256 (matches Data Flash page size on CH582F).
 */
#ifndef BLE_SNV_BLOCK
#define BLE_SNV_BLOCK  256
#endif

/*********************************************************************
 * BLE_SNV_NUM — number of bonded devices to store.
 *
 *   Each bond uses 2 × BLE_SNV_BLOCK bytes (active + backup page).
 *   Default: 1 (single bonded host). For a keyboard that pairs with
 *   one computer, this is sufficient. Increase if switching between
 *   multiple hosts (e.g., desktop + laptop + tablet).
 */
#ifndef BLE_SNV_NUM
#define BLE_SNV_NUM  1
#endif

/* ====================================================================
 * 8. BLE TIMING — RTC clock and connection multiplexing
 * ====================================================================
 */

/*********************************************************************
 * CLK_OSC32K — RTC (32 kHz) clock source.
 *
 *   0: External 32.768 kHz crystal (LSE). Higher accuracy (±20 ppm),
 *      required if device acts as BLE Central (needs precise timing
 *      for connection supervision). Consumes ~1 µA extra.
 *   1: Internal 32.0 kHz RC oscillator (LSI). Lower accuracy (±1000 ppm
 *      after calibration), but no external crystal needed. Suitable for
 *      Peripheral-only devices with periodic calibration enabled.
 *   2: Internal 32.768 kHz RC (same as LSI but trimmed to nominal).
 *
 *   IMPORTANT: Do NOT override this here. Set via compiler -D flag
 *   (MounRiver Studio project settings → Symbols). The SDK build
 *   system expects this to be a compiler define, not a source change.
 *   If device includes Central role, must use 0 (external crystal).
 *
 *   Default: 1 (internal 32 kHz, Peripheral-only).
 */
#ifndef CLK_OSC32K
#define CLK_OSC32K  1
#endif

/*********************************************************************
 * PERIPHERAL_MAX_CONNECTION — maximum simultaneous Peripheral roles.
 *
 *   Default: 1 (single BLE Peripheral connection). AT-Node is a
 *   keyboard — one host at a time.
 */
#ifndef PERIPHERAL_MAX_CONNECTION
#define PERIPHERAL_MAX_CONNECTION  1
#endif

/*********************************************************************
 * CENTRAL_MAX_CONNECTION — maximum simultaneous Central roles.
 *
 *   Default: 3 (reserved but unused in Peripheral-only config).
 *   Only relevant if application also scans/connects as Central.
 *   AT-Node does not use Central role; value is a SDK default.
 */
#ifndef CENTRAL_MAX_CONNECTION
#define CENTRAL_MAX_CONNECTION  3
#endif

/* ====================================================================
 * 9. GLOBALS — declared here, defined in main.c
 * ====================================================================
 */

/* BLE protocol stack heap. Allocated in main.c at file scope.
   Must be 4-byte aligned and sized to match BLE_MEMHEAP_SIZE.
   Referenced by ble_stack_init() via extern declaration. */
extern uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/* Custom BLE MAC address. Defined in main.c only when BLE_MAC=TRUE.
   Extern here so ble_stack_init() can reference it unconditionally —
   the #if guard in main.c ensures the definition exists only when needed. */
extern const uint8_t MacAddr[6];

#endif /* __CONFIG_H */
