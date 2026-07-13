/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_dev.c
 * Author             : at-node
 * Version            : V1.0
 * Description        : USB 复合设备 — CDC 虚拟串口 + HID 键盘
 *                      仅实现枚举，不包含具体功能逻辑。
 *
 *                      端点分配：
 *                        EP0   控制端点（共享）
 *                        EP1 IN/OUT  CDC 数据（BULK, 64B）
 *                        EP3 IN      CDC 通知（Interrupt, 8B）
 *                        EP2 IN      HID 键盘输入（Interrupt, 8B）
 *                        EP2 OUT     HID LED 输出（Interrupt, 8B）
 ********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Reference: WCH CH583 EVT — USB/Device/COM + USB/Device/CompoundDev
 *******************************************************************************/

#include "usb_dev.h"
#include "HAL.h"
#include <string.h>

/*********************************************************************
 * 常量定义
 */
#define EP0_MAX_PACKET      64
#define EP1_BUF_SIZE        64
#define EP2_BUF_SIZE        8
#define EP3_BUF_SIZE        8

/* USB 描述符类型 */
#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIG             0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_IAD                0xEF
#define USB_DESC_HID_REPORT         0x22

/* HID 类请求 */
#define HID_REQ_GET_REPORT          0x01
#define HID_REQ_GET_IDLE            0x02
#define HID_REQ_GET_PROTOCOL        0x03
#define HID_REQ_SET_REPORT          0x09
#define HID_REQ_SET_IDLE            0x0A
#define HID_REQ_SET_PROTOCOL        0x0B

/* CDC 类请求 */
#define CDC_REQ_SET_LINE_CODING     0x20
#define CDC_REQ_GET_LINE_CODING     0x21
#define CDC_REQ_SET_CONTROL_LINE    0x22
#define CDC_REQ_SEND_BREAK          0x23

/*********************************************************************
 * 端点缓冲区 — 4 字节对齐（USB DMA 要求）
 */
__attribute__((aligned(4))) uint8_t EP0Buffer[EP0_MAX_PACKET];
__attribute__((aligned(4))) uint8_t EP1Buffer[EP1_BUF_SIZE * 2];
__attribute__((aligned(4))) uint8_t EP2Buffer[EP2_BUF_SIZE * 2];
__attribute__((aligned(4))) uint8_t EP3Buffer[EP3_BUF_SIZE];

/*********************************************************************
 * CDC Line Coding
 */
typedef struct __attribute__((packed)) _LINE_CODING {
    uint32_t BaudRate;
    uint8_t  StopBits;
    uint8_t  ParityType;
    uint8_t  DataBits;
} LINE_CODING;

static LINE_CODING lineCoding = { 115200, 0, 0, 8 };

/*********************************************************************
 * 状态标志
 */
static volatile uint8_t usbDevCfg  = 0;
static volatile uint8_t cdcReady   = 0;
static volatile uint8_t hidReady   = 0;

/*********************************************************************
 * USB 请求（Setup 包）
 */
typedef struct __attribute__((packed)) _USB_SETUP {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} USB_SETUP;

/*********************************************************************
 * 字符串描述符
 */
static const uint8_t strLang[]    = { 0x04, 0x03, 0x09, 0x04 };
static const uint8_t strMfr[]     = { 0x0C, 0x03, 'a',0,'t',0,'-',0,'n',0,'o',0,'d',0,'e',0 };
static const uint8_t strProduct[] = { 0x28, 0x03,
    'a',0,'t',0,'-',0,'n',0,'o',0,'d',0,'e',0,' ',0,'C',0,'D',0,'C',0,' ',0,'&',0,' ',0,
    'H',0,'I',0,'D',0,' ',0,'K',0,'e',0,'y',0,'b',0,'o',0,'a',0,'r',0,'d',0 };
static const uint8_t strSerial[]  = { 0x1A, 0x03,
    '2',0,'0',0,'2',0,'6',0,'0',0,'7',0,'0',0,'1',0,'A',0,'B',0,'C',0,'D',0,0,0 };

/*********************************************************************
 * USB 设备描述符
 */
static const uint8_t devDesc[] = {
    0x12,                       // bLength
    0x01,                       // bDescriptorType = DEVICE
    0x00, 0x02,                 // bcdUSB = 2.00
    0xEF,                       // bDeviceClass = Misc (IAD)
    0x02,                       // bDeviceSubClass = Common Class
    0x01,                       // bDeviceProtocol = IAD
    0x40,                       // bMaxPacketSize0 = 64
    0x86, 0x1A,                 // idVendor = 0x1A86 (WCH)
    0x40, 0x80,                 // idProduct = 0x8040
    0x00, 0x30,                 // bcdDevice
    0x01,                       // iManufacturer = 1
    0x02,                       // iProduct = 2
    0x03,                       // iSerialNumber = 3
    0x01                        // bNumConfigurations = 1
};

/*********************************************************************
 * HID Report Descriptor — 标准键盘（Boot Protocol）
 *   IN: 8 字节（修饰键 + 保留 + 6 键值）
 *   OUT: 1 字节（LED 状态）
 */
static const uint8_t hidReportDesc[] = {
    0x05, 0x01,                 // Usage Page (Generic Desktop)
    0x09, 0x06,                 // Usage (Keyboard)
    0xA1, 0x01,                 // Collection (Application)
    0x05, 0x07,                 //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,                 //   Usage Minimum (224)
    0x29, 0xE7,                 //   Usage Maximum (231)
    0x15, 0x00,                 //   Logical Minimum (0)
    0x25, 0x01,                 //   Logical Maximum (1)
    0x75, 0x01,                 //   Report Size (1)
    0x95, 0x08,                 //   Report Count (8)
    0x81, 0x02,                 //   Input (Data,Var,Abs)
    0x95, 0x01,                 //   Report Count (1)
    0x75, 0x08,                 //   Report Size (8)
    0x81, 0x01,                 //   Input (Const,Arr,Abs)
    0x95, 0x06,                 //   Report Count (6)
    0x75, 0x08,                 //   Report Size (8)
    0x15, 0x00,                 //   Logical Minimum (0)
    0x26, 0xFF, 0x00,           //   Logical Maximum (255)
    0x05, 0x07,                 //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,                 //   Usage Minimum (0)
    0x29, 0xFF,                 //   Usage Maximum (255)
    0x81, 0x00,                 //   Input (Data,Arr,Abs)
    0x05, 0x08,                 //   Usage Page (LEDs)
    0x19, 0x01,                 //   Usage Minimum (1 = Num Lock)
    0x29, 0x05,                 //   Usage Maximum (5 = Kana)
    0x15, 0x00,                 //   Logical Minimum (0)
    0x25, 0x01,                 //   Logical Maximum (1)
    0x75, 0x01,                 //   Report Size (1)
    0x95, 0x05,                 //   Report Count (5)
    0x91, 0x02,                 //   Output (Data,Var,Abs)
    0x95, 0x01,                 //   Report Count (1)
    0x75, 0x03,                 //   Report Size (3)
    0x91, 0x01,                 //   Output (Const,Arr,Abs)
    0xC0                        // End Collection
};

/*********************************************************************
 * USB 配置描述符（IAD + CDC Communication + CDC Data + HID Keyboard）
 * 总长度 = 106 字节
 */
static const uint8_t cfgDesc[] = {
    0x09, 0x02, 0x6A, 0x00,     // bLength=9, CONFIG, wTotalLength=106
    0x03,                       // bNumInterfaces = 3
    0x01,                       // bConfigurationValue = 1
    0x00,                       // iConfiguration
    0x80,                       // bmAttributes = Bus Powered
    0x32,                       // bMaxPower = 100mA

    /* IAD: 关联接口 0 + 1 为 CDC 功能 */
    0x08, 0xEF, 0x00, 0x02, 0x02, 0x02, 0x01, 0x00,

    /* 接口 0: CDC Communication */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,           // Header: CDC 1.10
    0x04, 0x24, 0x02, 0x02,                 // ACM
    0x05, 0x24, 0x06, 0x00, 0x01,           // Union: Comm=0, Data=1
    0x05, 0x24, 0x01, 0x01, 0x00,           // Call Mgmt
    0x07, 0x05, 0x83, 0x03, 0x08, 0x00, 0x01, // EP3 IN (Interrupt, 8B)

    /* 接口 1: CDC Data */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, // EP1 OUT (BULK, 64B)
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, // EP1 IN  (BULK, 64B)

    /* 接口 2: HID Keyboard */
    0x09, 0x04, 0x02, 0x00, 0x02, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
        sizeof(hidReportDesc), 0x00,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0A, // EP2 IN (Interrupt, 8B, 10ms)
    0x07, 0x05, 0x02, 0x03, 0x08, 0x00, 0x0A, // EP2 OUT (Interrupt, 8B, 10ms)
};

/*********************************************************************
 * 本地函数声明
 */
static void  usbStandardRequest(USB_SETUP *pReq);
static void  usbClassRequest(USB_SETUP *pReq);
static void  usbEp0SendData(const uint8_t *pBuf, uint16_t len);
static void  usbEp0SendZLP(void);
static void  usbEp0SetStall(void);
static void  usbEp0SetInNAK(void);

/*********************************************************************
 * @fn      USB_Composite_Init
 */
void USB_Composite_Init(void)
{
    /* 1. 清除 USB 状态 */
    R8_USB_CTRL = 0x00;

    /* 2. 设置端点模式寄存器
     *    UEP4_1_MOD:  bit[7:4]=EP4 (disabled), bit[3:0]=EP1 (TX|RX)
     *    UEP2_3_MOD:  bit[7:4]=EP2 (TX|RX),      bit[3:0]=EP3 (TX)
     */
    R8_UEP4_1_MOD = RB_UEP1_TX_EN | RB_UEP1_RX_EN;
    R8_UEP2_3_MOD = RB_UEP2_TX_EN | RB_UEP2_RX_EN | RB_UEP3_TX_EN;

    /* 3. 设置端点 DMA 缓冲区地址 */
    R16_UEP0_DMA = (uint16_t)(uint32_t)EP0Buffer;
    R16_UEP1_DMA = (uint16_t)(uint32_t)EP1Buffer;
    R16_UEP2_DMA = (uint16_t)(uint32_t)EP2Buffer;
    R16_UEP3_DMA = (uint16_t)(uint32_t)EP3Buffer;

    /* 4. 端点应答控制
     *    EP0: OUT=ACK, IN=NAK（等待 SETUP）
     *    EP1/EP2/EP3: OUT=ACK, IN=NAK, 自动翻转
     */
    R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP2_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP3_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;

    /* 5. 设备地址为 0 */
    R8_USB_DEV_AD = 0x00;

    /* 6. 启动 USB 设备 + INT_BUSY + DMA */
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;

    /* 7. 使能模拟 I/O，DP 上拉 */
    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;

    /* 8. 允许 USB 端口 */
    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;

    /* 9. 清中断标志，开启传输/SUSPEND/BUS_RST 中断 */
    R8_USB_INT_FG = 0xFF;
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_BUS_RST | RB_UIE_TRANSFER;

    /* 10. 清状态 */
    usbDevCfg = 0;
    cdcReady  = 0;
    hidReady  = 0;

    PRINT("USB Composite Init OK\n");
}

/*********************************************************************
 * @fn      USB_DevTransProcess
 *
 * @brief   USB 传输事件处理
 */
void USB_DevTransProcess(void)
{
    uint8_t  intst, token, ep;

    intst = R8_USB_INT_ST;
    token = intst & MASK_UIS_TOKEN;
    ep    = intst & MASK_UIS_ENDP;

    switch (token)
    {
        /* ---- SETUP 令牌 ---- */
        case UIS_TOKEN_SETUP:
        {
            USB_SETUP *pReq = (USB_SETUP *)EP0Buffer;

            if ((pReq->bmRequestType & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD)
                usbStandardRequest(pReq);
            else
                usbClassRequest(pReq);
            break;
        }

        /* ---- IN 令牌 ---- */
        case UIS_TOKEN_IN:
        {
            if (ep == 0)
            {
                /* EP0 IN 完成 → 返回 NAK */
                usbEp0SetInNAK();
            }
            break;
        }

        /* ---- OUT 令牌 ---- */
        case UIS_TOKEN_OUT:
        {
            if (ep == 0)
            {
                usbEp0SetInNAK();
            }
            else if (ep == 1)
            {
                /* EP1 OUT — CDC 数据（暂不处理） */
                R8_UEP1_CTRL &= ~MASK_UEP_T_RES;
                R8_UEP1_CTRL |= UEP_T_RES_NAK;
            }
            else if (ep == 2)
            {
                /* EP2 OUT — HID LED（暂不处理） */
                R8_UEP2_CTRL &= ~MASK_UEP_T_RES;
                R8_UEP2_CTRL |= UEP_T_RES_NAK;
            }
            break;
        }

        default:
            break;
    }
}

/*********************************************************************
 * USB 标准请求处理
 */
static void usbStandardRequest(USB_SETUP *pReq)
{
    uint16_t wLen = pReq->wLength;

    switch (pReq->bRequest)
    {
        case USB_GET_DESCRIPTOR:
        {
            uint8_t  descType = (pReq->wValue >> 8) & 0xFF;
            uint8_t  descIdx  = pReq->wValue & 0xFF;
            const uint8_t *pDesc = NULL;
            uint16_t descLen = 0;

            switch (descType)
            {
                case USB_DESC_DEVICE:
                    pDesc = devDesc;   descLen = sizeof(devDesc);   break;
                case USB_DESC_CONFIG:
                    pDesc = cfgDesc;   descLen = sizeof(cfgDesc);   break;
                case USB_DESC_STRING:
                    switch (descIdx)
                    {
                        case 0:  pDesc = strLang;   descLen = sizeof(strLang);   break;
                        case 1:  pDesc = strMfr;    descLen = sizeof(strMfr);    break;
                        case 2:  pDesc = strProduct; descLen = sizeof(strProduct); break;
                        case 3:  pDesc = strSerial;  descLen = sizeof(strSerial);  break;
                        default: usbEp0SetStall(); return;
                    }
                    break;
                default:
                    usbEp0SetStall();
                    return;
            }
            if (wLen > descLen) wLen = descLen;
            usbEp0SendData(pDesc, wLen);
            return;
        }

        case USB_SET_ADDRESS:
            R8_USB_DEV_AD = (R8_USB_DEV_AD & ~0x7F) | (pReq->wValue & 0x7F);
            usbEp0SendZLP();
            return;

        case USB_SET_CONFIGURATION:
            usbDevCfg = 1;
            usbEp0SendZLP();
            return;

        case USB_GET_CONFIGURATION:
            EP0Buffer[0] = usbDevCfg;
            usbEp0SendData(EP0Buffer, 1);
            return;

        case USB_GET_INTERFACE:
            EP0Buffer[0] = 0;
            usbEp0SendData(EP0Buffer, 1);
            return;

        case USB_SET_INTERFACE:
        case USB_SET_FEATURE:
        case USB_CLEAR_FEATURE:
            usbEp0SendZLP();
            return;

        case USB_GET_STATUS:
            EP0Buffer[0] = 0x00;
            EP0Buffer[1] = 0x00;
            usbEp0SendData(EP0Buffer, 2);
            return;

        default:
            usbEp0SetStall();
            return;
    }
}

/*********************************************************************
 * USB 类请求处理（CDC + HID）
 */
static void usbClassRequest(USB_SETUP *pReq)
{
    uint8_t  reqType = pReq->bmRequestType;
    uint8_t  req     = pReq->bRequest;
    uint16_t wVal    = pReq->wValue;
    uint16_t wIdx    = pReq->wIndex;
    uint16_t wLen    = pReq->wLength;

    /* HID 类请求（接口 2） */
    if ((reqType & 0x0F) == 0x01 && (reqType & 0x80) == 0x00 && wIdx == 2)
    {
        switch (req)
        {
            case HID_REQ_GET_REPORT:
            case HID_REQ_GET_IDLE:
            case HID_REQ_GET_PROTOCOL:
                EP0Buffer[0] = 0;
                usbEp0SendData(EP0Buffer, (wLen < 1) ? wLen : 1);
                return;
            case HID_REQ_SET_IDLE:
                hidReady = 1;
                usbEp0SendZLP();
                return;
            case HID_REQ_SET_REPORT:
            case HID_REQ_SET_PROTOCOL:
                usbEp0SendZLP();
                return;
            default:
                usbEp0SetStall();
                return;
        }
    }

    /* CDC 类请求（接口 0） */
    if ((reqType & 0x0F) == 0x01 && (reqType & 0x80) == 0x00 && wIdx == 0)
    {
        switch (req)
        {
            case CDC_REQ_SET_LINE_CODING:
                memcpy(&lineCoding, EP0Buffer, sizeof(LINE_CODING));
                cdcReady = 1;
                PRINT("CDC: %lu %d%c%d\n",
                      (unsigned long)lineCoding.BaudRate,
                      lineCoding.DataBits,
                      (lineCoding.ParityType == 0) ? 'N' :
                      (lineCoding.ParityType == 1) ? 'O' : 'E',
                      lineCoding.StopBits + 1);
                usbEp0SendZLP();
                return;
            case CDC_REQ_GET_LINE_CODING:
                memcpy(EP0Buffer, &lineCoding, sizeof(LINE_CODING));
                usbEp0SendData(EP0Buffer, sizeof(LINE_CODING));
                return;
            case CDC_REQ_SET_CONTROL_LINE:
            case CDC_REQ_SEND_BREAK:
                usbEp0SendZLP();
                return;
            default:
                usbEp0SetStall();
                return;
        }
    }

    usbEp0SetStall();
}

/*********************************************************************
 * EP0 操作辅助函数
 */
static void usbEp0SendData(const uint8_t *pBuf, uint16_t len)
{
    uint16_t i;

    if (pBuf != EP0Buffer)
        for (i = 0; i < len; i++)
            EP0Buffer[i] = pBuf[i];

    R8_UEP0_T_LEN = (uint8_t)len;
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_ACK;
}

static void usbEp0SendZLP(void)
{
    R8_UEP0_T_LEN = 0;
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_ACK;
}

static void usbEp0SetStall(void)
{
    R8_UEP0_CTRL = UEP_R_RES_STALL | UEP_T_RES_STALL;
}

static void usbEp0SetInNAK(void)
{
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
}

/*********************************************************************
 * 状态查询
 */
uint8_t USB_CDC_IsReady(void) { return cdcReady; }
uint8_t USB_HID_IsReady(void) { return hidReady; }

/*********************************************************************
 * CDC 数据发送（桩函数）
 */
void USB_CDC_SendData(uint8_t *pData, uint16_t len)
{
    (void)pData;
    (void)len;
}

/*********************************************************************
 * HID 报告发送（桩函数）
 */
void USB_HID_SendReport(uint8_t *pData, uint16_t len)
{
    (void)pData;
    (void)len;
}

/*********************************************************************
 * USB 中断服务函数
 */
__attribute__((interrupt("WCH-Interrupt-fast")))
__attribute__((section(".highcode")))
void USB_IRQHandler(void)
{
    R8_USB_INT_FG = 0xFF;
    USB_DevTransProcess();
}

/******************************** endfile @ usb_dev.c ******************************/
