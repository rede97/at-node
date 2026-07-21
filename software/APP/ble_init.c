/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_init.c
 * Author             : at-node
 * Description        : BLE peripheral initialization — GAP role + GATT services
 *
 *   Groups all BLE service initialization into a single init-stage call.
 *   Called once during startup, after hws_init() and ble_stack_init().
 *
 *   Init order within this function matters:
 *     GAP role → HID device profile → HID keyboard emulation + advertising.
 *     If additional GATT services are added, register them between GAP and HID.
 ********************************************************************************/

#include "config.h"
#include "ble_hid_dev.h"
#include "ble_dongle.h"
#include "hidkbd.h"
#include "role.h"

#if BLE_HAS_KBD
/* Keyboard role: GAP Peripheral + HID GATT services + advertising. */
static void ble_kbd_init(void)
{
    /* GAP role — must be first: sets device name, advertising data,
       bonding parameters, connect/disconnect callbacks */
    GAPRole_PeripheralInit();

    /* HID Device GATT service — registers HID report characteristic,
       keyboard report descriptor, input/output/feature reports */
    ble_hid_dev_init();

    /* HID keyboard emulation — registers TMOS task, starts advertising,
       configures GAP bond manager, battery level, device info services */
    ble_hid_emu_init();
}
#endif

/*******************************************************************************
 * @fn      ble_peripheral_init
 *
 * @brief   Initialize BLE peripheral: GAP role, GATT services, start advertising.
 *
 *   Init stage: 4 — called after ble_stack_init(), before usb_init().
 *   Depends on: ble_stack_init() — BLE protocol stack must be running.
 *               hws_init()    — hardware services (timers, sleep) must be ready.
 *   Side effects:
 *     - Registers GAP peripheral role (bond manager, advertising parameters).
 *     - Registers HID Device GATT service (report callbacks, keyboard descriptor).
 *     - Starts BLE advertising and begins accepting connections.
 *     - Creates TMOS task for HID emulation event processing.
 *
 *   The HID emulation layer handles connection state, key report dispatch,
 *   parameter updates (connection interval, PHY), and LED output reports.
 *   No keys are sent until a host subscribes to the HID Input Report CCCD.
 */
void ble_peripheral_init(void)
{
#if BLE_MODE == BLE_MODE_DUAL
    /* DUAL build: both roles compiled in; the DataFlash flag picks the
       runtime role (AT+ROLE + soft reset, see role.c). */
    if (role_current() == ROLE_DONGLE)
        ble_dongle_init();
    else
        ble_kbd_init();
#elif BLE_HAS_DONGLE
    /* Dongle mode: Central role — scan for a BLE keyboard, connect as
       HID host, forward boot reports to USB. Replaces ALL Peripheral
       services in this build. */
    ble_dongle_init();
#else
    ble_kbd_init();
#endif
}
