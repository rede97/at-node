/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : at-node
 * Version            : V1.0
 * Description        : BLE HID + USB HID keyboard firmware
 ********************************************************************************/

#include "config.h"
#include "HWS.h"
#include "hiddev.h"
#include "hidkbd.h"
#include "usb_dev.h"
#include "at_parser.h"
#include "hidkbd_common.h"


__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

__HIGH_CODE
__attribute__((noinline))
void Main_Circulation()
{
    while(1) TMOS_SystemProcess();
}

/* Hardware key callback — routes physical button to keyboard layer */
static void key_press(uint8_t key)
{
    if (key & HAL_KEY_SW_1) {
        PRINT("KEY PRESS\n\n");
        uint8_t rpt[8] = {0,0,0x3A,0,0,0,0,0};
        USB_HID_SendReport(rpt, 8);
        kb_press_and_release(0x3A);
        HalLedSet(HAL_LED_1, HAL_LED_MODE_ON);
    } else {
        PRINT("KEY RELEASE\n\n");
        uint8_t rpt[8] = {0};
        USB_HID_SendReport(rpt, 8);
        kb_release_all();
        HalLedSet(HAL_LED_1, HAL_LED_MODE_OFF);
    }
}

int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    SetSysClock(CLK_SOURCE_PLL_60MHz);

#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
#endif

#ifdef DEBUG
    GPIOA_SetBits(bTXD1);
    GPIOA_ModeCfg(bTXD1, GPIO_ModeOut_PP_5mA);
    GPIOA_ModeCfg(bRXD1, GPIO_ModeIN_PU);
    UART1_DefInit();
#endif
    PRINT("%s\n", VER_LIB);

    CH58X_BLEInit();
    HAL_Init();
    AT_Init((at_cmd_t *)cmd_table, cmd_table_count);
    HalKeyConfig(key_press);
    GAPRole_PeripheralInit();
    HidDev_Init();
    HidEmu_Init();

#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    PRINT("HAL_SLEEP enabled - USB disabled\n");
#else
    USB_Device_Setup();
    PFIC_EnableIRQ(USB_IRQn);
    PRINT("USB HID Keyboard Init OK\n");
#endif

    Main_Circulation();
}
