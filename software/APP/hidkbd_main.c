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

/* ===== AT command handlers ===== */
static int at_cmd_AT(int argc, char *argv[])    { (void)argc; (void)argv; return 0; }
static int at_cmd_VER(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("AT-Node v1.0 BLE: %s", VER_LIB); return 0; }
static int at_cmd_HELP(int argc, char *argv[])  {
    (void)argc; (void)argv;
    AT_Response("AT-Node Commands:\r\n  AT       - handshake\r\n  AT+VER   - version\r\n  AT+HELP  - this help\r\n  AT+ECHO  - echo <text>");
    return 0;
}
static int at_cmd_ECHO(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+ECHO=text"); return -1; }
    AT_Response("%s", argv[1]);
    return 0;
}

static const at_cmd_t cmd_table[] = {
    { "AT",       "handshake -> OK",     at_cmd_AT },
    { "AT+VER",   "firmware version",    at_cmd_VER },
    { "AT+HELP",  "command list",        at_cmd_HELP },
    { "AT+ECHO",  "echo <text>",         at_cmd_ECHO },
};

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
    GPIOA_ModeCfg(bRXD1, GPIO_ModeIN_PU);  // PA8 = UART1 RX for AT commands
    UART1_DefInit();
#endif
    PRINT("%s\n", VER_LIB);

    /* BLE 初始化（与 BleInputStick 顺序一致：BLE 先，USB 后） */
    CH58X_BLEInit();
    HAL_Init();

    /* AT command parser — UART1 serial input */
    AT_Init((at_cmd_t *)cmd_table, sizeof(cmd_table)/sizeof(cmd_table[0]));

    /* Always enable key scanning — works with USB even without BLE */
    HalKeyConfig(key_press);

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
