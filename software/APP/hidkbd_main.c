/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : at-node
 * Version            : V1.0
 * Description        : BLE HID 蓝牙键盘 + USB HID 有线键盘
 *                      完全参照 BleInputStick 的初始化顺序
 ********************************************************************************/

#include "CONFIG.h"
#include "HAL.h"
#include "hiddev.h"
#include "hidkbd.h"
#include "usb_dev.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

__HIGH_CODE
__attribute__((noinline))
void Main_Circulation()
{
    while(1)
    {
        TMOS_SystemProcess();
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
    UART1_DefInit();
#endif
    PRINT("%s\n", VER_LIB);

    /* BLE 初始化（与 BleInputStick 顺序一致：BLE 先，USB 后） */
    CH58X_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();
    HidDev_Init();
    HidEmu_Init();

#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    /* 低功耗模式：USB 不可用，仅通过硬件串口通信 */
    PRINT("HAL_SLEEP enabled ― USB disabled\n");
#else
    /* USB HID 键盘 ― 来自 WCH EVT HID_CompliantDev */
    USB_Device_Setup();
    PFIC_EnableIRQ(USB_IRQn);
    PRINT("USB HID Keyboard Init OK\n");
#endif

    Main_Circulation();
}

/******************************** endfile @ main ******************************/
