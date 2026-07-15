/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : at-node
 * Description        : AT-Node firmware entry point.
 *
 *   BLE HID keyboard + USB CDC+HID composite device.
 *   AT-command driven AI agent peripheral, CH582F RISC-V MCU.
 *
 *   Init sequence (7 stages, linear dependency):
 *     1. hws_platform_init()     — power, clock, GPIO, debug UART
 *     2. ble_stack_init()        — BLE protocol stack (initializes TMOS!)
 *     3. hws_init(key_cb)        — hardware services + HWS TMOS task
 *     4. at_init()               — AT command parser (UART1 + CDC)
 *     5. ble_peripheral_init()   — GAP role + GATT services + advertising
 *     6. usb_init()              — USB composite device OR sleep mode
 *     7. main_loop()             — TMOS scheduler (never returns)
 *
 *   Stage 2 MUST come before stage 3: ble_stack_init() → BLE_LibInit()
 *   initializes TMOS. hws_init() calls TMOS_ProcessEventRegister()
 *   which requires TMOS to be running. Stages can't be reordered
 *   without breaking dependencies (documented in each init function).
 *
 *   USB vs Sleep: mutually exclusive on CH582F. HWS_SLEEP=TRUE
 *   disables USB at compile time (USB clock stops in sleep).
 *   Controlled by config.h HWS_SLEEP macro.
 ********************************************************************************/

#include "config.h"
#include "hws.h"
#include "ble_stack.h"
#include "ble_init.h"
#include "ble_hid_dev.h"
#include "hidkbd.h"
#include "usb_dev.h"
#include "at_parser.h"
#include "hidkbd_common.h"


/* ===== BLE heap ===== */

/* BLE protocol stack heap — allocated at top of RAM (32 KB).
   Minimum 6 KB (BLE_MEMHEAP_SIZE). Size checked at compile time.
   Must be 4-byte aligned and globally visible (used by ble_stack_init). */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
/* Custom BLE MAC address (bottom 6 bytes become BD_ADDR).
   When BLE_MAC=FALSE, chip factory MAC is used instead.
   Stored in little-endian order in advertising packets. */
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/* ===== Main loop ===== */

/*******************************************************************************
 * @fn      main_loop
 *
 * @brief   Enter TMOS cooperative scheduler loop. Never returns.
 *
 *   Init stage: 7 — final call in main(). No code after this runs.
 *   TMOS_SystemProcess() dispatches all registered task event processors
 *   (HWS, AT, BLE HID Dev, BLE HID Emu) in priority order.
 *
 *   Must be __HIGH_CODE (placed in flash code region, not RAM).
 *   Must be noinline to prevent compiler from inlining the while(1)
 *   loop into main() — keeps the call stack clean for debugging.
 */
__HIGH_CODE
__attribute__((noinline))
void main_loop(void)
{
    while(1) {
        TMOS_SystemProcess();
    }
}

/* ===== Hardware key callback ===== */

/*******************************************************************************
 * @fn      key_press
 *
 * @brief   Physical button callback — registered with HWS key scanner.
 *
 *   Called by: HWS TMOS task (hws_process_event → hws_key_poll) when
 *   a key state change is detected. Runs in TMOS context (not ISR).
 *
 *   Current behavior (test/demo code):
 *     - SW_1 press:   sends F1 key (0x3A) via BOTH USB and BLE routing,
 *                     turns LED1 on.
 *     - SW_1 release: releases all keys, turns LED1 off.
 *
 *   NOTE: USB_HID_SendReport() is called directly in addition to
 *   kb_press_and_release(). This is test code — in production,
 *   only kb_*() routing layer should be used, and key mapping
 *   should be configurable via AT commands.
 *
 *   @param  key  Bitmask of keys that changed (HWS_KEY_SW_1..4).
 *                Each bit indicates a key that was pressed or released
 *                since the last poll.
 */
static void key_press(uint8_t key)
{
    if(key & HWS_KEY_SW_1) {
        PRINT("KEY PRESS\n\n");
        uint8_t rpt[8] = {0, 0, 0x3A, 0, 0, 0, 0, 0};
        USB_HID_SendReport(rpt, 8);
        kb_press_and_release(0x3A);
        hws_led_set(HWS_LED_1, HWS_LED_MODE_ON);
    } else {
        PRINT("KEY RELEASE\n\n");
        uint8_t rpt[8] = {0};
        USB_HID_SendReport(rpt, 8);
        kb_release_all();
        hws_led_set(HWS_LED_1, HWS_LED_MODE_OFF);
    }
}

/* ===== USB initialization ===== */

/*******************************************************************************
 * @fn      usb_init
 *
 * @brief   Initialize USB composite device OR configure sleep mode.
 *
 *   Init stage: 6 — called after ble_peripheral_init(), before main_loop().
 *   Depends on: hws_platform_init() — clock must be 60 MHz for USB 48 MHz PHY.
 *               hws_init()          — sleep config must be ready if HWS_SLEEP=TRUE.
 *
 *   Branch:
 *     HWS_SLEEP=TRUE:  USB disabled. Device enters low-power sleep
 *                       under BLE stack control (sleepCB callback).
 *                       USB clock stops in sleep → enumeration lost.
 *     HWS_SLEEP=FALSE: USB CDC ACM (EP1 BULK) + HID Keyboard (EP2 INT)
 *                       composite device enabled. PFIC USB interrupt
 *                       handles setup/IN/OUT transfers.
 *
 *   Side effects when USB enabled:
 *     - Pulls D+ high via internal 1.5k pull-up (signals host).
 *     - Responds to USB reset/setup/IN/OUT via USB_IRQn handler.
 *     - CDC receives AT commands; HID sends keyboard reports to host.
 */
static void usb_init(void)
{
#if(defined(HWS_SLEEP)) && (HWS_SLEEP == TRUE)
    PRINT("HWS_SLEEP enabled - USB disabled\n");
#else
    /* USB device setup: configures endpoints EP0–EP3, sets D+ pull-up,
       starts enumeration. Must be called once. */
    USB_Device_Setup();

    /* Enable USB interrupt (PFIC = Platform Interrupt Controller).
       Handles: reset, setup packets, EP0 control transfers, EP1/EP2
       bulk/interrupt transfers. Must be after USB_Device_Setup(). */
    PFIC_EnableIRQ(USB_IRQn);

    PRINT("USB HID Keyboard Init OK\n");
#endif
}

/* ===== Entry point ===== */

/*******************************************************************************
 * @fn      main
 *
 * @brief   Firmware entry point. 7-stage linear initialization.
 *
 *   Each stage's dependencies and side effects are documented in the
 *   respective init function. Stages must execute in this order:
 *
 *     Stage 1  hws_platform_init()      Power → Clock → GPIO → UART
 *     Stage 2  ble_stack_init()         BLE protocol stack + TMOS init
 *     Stage 3  hws_init(key_press)      RTC → Sleep → LED → KEY + callback
 *     Stage 4  at_init()                AT command parser (UART1 + CDC)
 *     Stage 5  ble_peripheral_init()    GAP → HID Device → HID Emu → Advertise
 *     Stage 6  usb_init()               USB composite OR sleep mode
 *     Stage 7  main_loop()              TMOS cooperative scheduler
 *
 *   After main_loop(), TMOS_SystemProcess() dispatches HWS/AT/BLE tasks
 *   cooperatively. No blocking — all I/O is event-driven via TMOS timers
 *   and interrupt handlers.
 */
int main(void)
{
    /* Stage 1: Platform — power, clock tree, GPIO pads, debug UART.
       Must run first. No dependencies. All subsequent stages require
       the 60 MHz clock and configured GPIO. */
    hws_platform_init();
    PRINT("%s\n", VER_LIB);   /* Library version check — confirms UART works */

    /* Stage 2: BLE protocol stack — memory heap, SNV flash, MAC address,
       Tx power, connection limits. Must run before any BLE API calls
       AND before hws_init(): BLE_LibInit() inside ble_stack_init()
       initializes the TMOS scheduler, which hws_init() depends on.
       Uses hws_get_temp and hws_sleep_enter as callbacks (function
       pointers — the functions exist at link time, so this is safe). */
    ble_stack_init();

    /* Stage 3: Hardware services — RTC, sleep, LED, key scanning.
       Registers the HWS TMOS task (TMOS must already be running from
       ble_stack_init). Pass key_press callback so it's registered
       before the first key poll fires (no missed events). */
    hws_init(key_press);

    /* Stage 4: AT command parser — UART1 + CDC dual-channel line parser.
       Polls at 10 ms via TMOS. AT commands become available immediately.
       Uses the built-in command table from at_cmds.c. */
    at_init();

    /* Stage 5: BLE peripheral — GAP role (bonding, advertising params),
       GATT services (HID Device, HID Keyboard, Battery, Device Info).
       Starts BLE advertising. Host connections begin arriving. */
    ble_peripheral_init();

    /* Stage 6: USB or Sleep — USB CDC+HID composite OR low-power sleep.
       Mutually exclusive (compile-time choice via HWS_SLEEP). */
    usb_init();

    /* Stage 7: Enter TMOS scheduler — never returns.
       All further execution is event-driven via TMOS tasks. */
    main_loop();
}
