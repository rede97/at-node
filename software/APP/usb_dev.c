/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_dev.c
 * Author             : at-node (based on WCH EVT HID_CompliantDev)
 * Version            : V1.0
 * Description        : USB HID 键盘设备 — 直接复制 WCH EVT 例程，
 *                      仅修改描述符和产品字符串
 ********************************************************************************/

#include "CH58x_common.h"

/* ===== 描述符（修改为键盘） ===== */
#define EP0_SIZE    0x40
#define EP1_SIZE    8

/* HID Report Descriptor — 标准键盘 */
const uint8_t HIDDescr[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xC0
};

/* 设备描述符 */
const uint8_t MyDevDescr[] = {
    0x12,0x01,0x10,0x01,0x00,0x00,0x00,EP0_SIZE,
    0x86,0x1A,0x07,0x21,0x00,0x00,0x01,0x02,0x00,0x01
};

/* 配置描述符 — HID 键盘, EP1=8B */
const uint8_t MyCfgDescr[] = {
    0x09,0x02,0x29,0x00,0x01,0x01,0x04,0xA0,0x23,
    0x09,0x04,0x00,0x00,0x02,0x03,0x01,0x01,0x00,
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,
    sizeof(HIDDescr),0x00,  /* wDescriptorLength — 编译器自动计算 */
    0x07,0x05,0x81,0x03,EP1_SIZE,0x00,0x01,
    0x07,0x05,0x01,0x03,EP1_SIZE,0x00,0x01,
};

/* 字符串描述符 */
const uint8_t MyLangDescr[]  = { 0x04, 0x03, 0x09, 0x04 };
const uint8_t MyManuInfo[]   = { 0x0C, 0x03, 'a',0,'t',0,'-',0,'n',0,'o',0,'d',0,'e',0 };
const uint8_t MyProdInfo[]   = { 0x22, 0x03,
    'a',0,'t',0,'-',0,'n',0,'o',0,'d',0,'e',0,' ',0,'H',0,'I',0,'D',0,' ',0,'K',0,'e',0,
    'y',0,'b',0,'o',0,'a',0,'r',0,'d',0 };

/* ===================== 以下完全复制自 EVT 例程 ===================== */

#define USB_INTERFACE_MAX_NUM       1
#define USB_INTERFACE_MAX_INDEX     0

uint8_t        DevConfig, Ready = 0;
uint8_t        SetupReqCode;
uint16_t       SetupReqLen;
const uint8_t *pDescr;
uint8_t        Report_Value[USB_INTERFACE_MAX_INDEX+1] = {0x00};
uint8_t        Idle_Value[USB_INTERFACE_MAX_INDEX+1] = {0x00};
uint8_t        USB_SleepStatus = 0x00;

/******** 端点 RAM ********/
__attribute__((aligned(4))) uint8_t EP0_Databuf[64 + 64 + 64];
__attribute__((aligned(4))) uint8_t EP1_Databuf[64 + 64];
__attribute__((aligned(4))) uint8_t EP2_Databuf[64 + 64];
__attribute__((aligned(4))) uint8_t EP3_Databuf[64 + 64];

void USB_DevTransProcess(void)
{
    uint8_t len, chtype;
    uint8_t intflag, errflag = 0;

    intflag = R8_USB_INT_FG;

    if(intflag & RB_UIF_TRANSFER)
    {
        if((R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN)
        {
            switch(R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
            {
                case UIS_TOKEN_IN:
                {
                    switch(SetupReqCode)
                    {
                        case USB_GET_DESCRIPTOR:
                            len = SetupReqLen >= EP0_SIZE ? EP0_SIZE : SetupReqLen;
                            memcpy(pEP0_DataBuf, pDescr, len);
                            SetupReqLen -= len;
                            pDescr += len;
                            R8_UEP0_T_LEN = len;
                            R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                            break;
                        case USB_SET_ADDRESS:
                            R8_USB_DEV_AD = (R8_USB_DEV_AD & RB_UDA_GP_BIT) | SetupReqLen;
                            R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                            break;
                        case USB_SET_FEATURE:
                            break;
                        default:
                            R8_UEP0_T_LEN = 0;
                            R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                            Ready = 1;
                            break;
                    }
                }
                break;

                case UIS_TOKEN_OUT:
                {
                    len = R8_USB_RX_LEN;
                }
                break;

                case UIS_TOKEN_OUT | 1:
                {
                    if(R8_USB_INT_ST & RB_UIS_TOG_OK)
                    {
                        R8_UEP1_CTRL ^= RB_UEP_R_TOG;
                        len = R8_USB_RX_LEN;
                        /* EP1 OUT — LED 数据（暂不处理） */
                    }
                }
                break;

                case UIS_TOKEN_IN | 1:
                    R8_UEP1_CTRL ^= RB_UEP_T_TOG;
                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                    Ready = 1;
                    break;

                default:
                    break;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }

        if(R8_USB_INT_ST & RB_UIS_SETUP_ACT)
        {
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;

            SetupReqLen = pSetupReqPak->wLength;
            SetupReqCode = pSetupReqPak->bRequest;
            chtype = pSetupReqPak->bRequestType;

            len = 0;
            errflag = 0;

            if((pSetupReqPak->bRequestType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD)
            {
                /* 非标准请求 */
                if(pSetupReqPak->bRequestType & 0x40)
                {
                    /* 厂商请求 */
                }
                else if(pSetupReqPak->bRequestType & 0x20)
                {
                    /* HID 类请求 */
                    switch(SetupReqCode)
                    {
                        case DEF_USB_SET_IDLE:
                            Idle_Value[pSetupReqPak->wIndex] = (uint8_t)(pSetupReqPak->wValue>>8);
                            break;
                        case DEF_USB_SET_REPORT:
                            break;
                        case DEF_USB_SET_PROTOCOL:
                            Report_Value[pSetupReqPak->wIndex] = (uint8_t)(pSetupReqPak->wValue);
                            break;
                        case DEF_USB_GET_IDLE:
                            EP0_Databuf[0] = Idle_Value[pSetupReqPak->wIndex];
                            len = 1;
                            break;
                        case DEF_USB_GET_PROTOCOL:
                            EP0_Databuf[0] = Report_Value[pSetupReqPak->wIndex];
                            len = 1;
                            break;
                        default:
                            errflag = 0xFF;
                    }
                }
            }
            else
            {
                /* 标准请求 */
                switch(SetupReqCode)
                {
                    case USB_GET_DESCRIPTOR:
                    {
                        switch(((pSetupReqPak->wValue) >> 8))
                        {
                            case USB_DESCR_TYP_DEVICE:
                                pDescr = MyDevDescr;
                                len = MyDevDescr[0];
                                break;
                            case USB_DESCR_TYP_CONFIG:
                                pDescr = MyCfgDescr;
                                len = MyCfgDescr[2];
                                break;
                            case USB_DESCR_TYP_HID:
                                pDescr = (uint8_t *)(&MyCfgDescr[18]);
                                len = 9;
                                break;
                            case USB_DESCR_TYP_REPORT:
                                pDescr = HIDDescr;
                                len = sizeof(HIDDescr);
                                break;
                            case USB_DESCR_TYP_STRING:
                                switch((pSetupReqPak->wValue) & 0xff)
                                {
                                    case 0: pDescr = MyLangDescr; len = MyLangDescr[0]; break;
                                    case 1: pDescr = MyManuInfo;  len = MyManuInfo[0];  break;
                                    case 2: pDescr = MyProdInfo;  len = MyProdInfo[0];  break;
                                    default: errflag = 0xff; break;
                                }
                                break;
                            default:
                                errflag = 0xff;
                                break;
                        }
                        if(SetupReqLen > len)
                            SetupReqLen = len;
                        len = (SetupReqLen >= EP0_SIZE) ? EP0_SIZE : SetupReqLen;
                        memcpy(pEP0_DataBuf, pDescr, len);
                        pDescr += len;
                    }
                    break;

                    case USB_SET_ADDRESS:
                        SetupReqLen = (pSetupReqPak->wValue) & 0xff;
                        break;

                    case USB_GET_CONFIGURATION:
                        pEP0_DataBuf[0] = DevConfig;
                        if(SetupReqLen > 1)
                            SetupReqLen = 1;
                        break;

                    case USB_SET_CONFIGURATION:
                        DevConfig = (pSetupReqPak->wValue) & 0xff;
                        break;

                    case USB_CLEAR_FEATURE:
                        if((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                        {
                            switch((pSetupReqPak->wIndex) & 0xff)
                            {
                                case 0x81:
                                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                                    break;
                                case 0x01:
                                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK;
                                    break;
                                default:
                                    errflag = 0xFF;
                                    break;
                            }
                        }
                        else if((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                        {
                            if(pSetupReqPak->wValue == 1)
                                USB_SleepStatus &= ~0x01;
                        }
                        else
                            errflag = 0xFF;
                        break;

                    case USB_SET_FEATURE:
                        if((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                        {
                            switch(pSetupReqPak->wIndex)
                            {
                                case 0x81:
                                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL;
                                    break;
                                case 0x01:
                                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_STALL;
                                    break;
                                default:
                                    errflag = 0xFF;
                                    break;
                            }
                        }
                        else if((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                        {
                            if(pSetupReqPak->wValue == 1)
                                USB_SleepStatus |= 0x01;
                        }
                        else
                            errflag = 0xFF;
                        break;

                    case USB_GET_INTERFACE:
                        pEP0_DataBuf[0] = 0x00;
                        if(SetupReqLen > 1)
                            SetupReqLen = 1;
                        break;

                    case USB_SET_INTERFACE:
                        break;

                    case USB_GET_STATUS:
                        if((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                        {
                            pEP0_DataBuf[0] = 0x00;
                            switch(pSetupReqPak->wIndex)
                            {
                                case 0x81:
                                    if((R8_UEP1_CTRL & (RB_UEP_T_TOG | MASK_UEP_T_RES)) == UEP_T_RES_STALL)
                                        pEP0_DataBuf[0] = 0x01;
                                    break;
                                case 0x01:
                                    if((R8_UEP1_CTRL & (RB_UEP_R_TOG | MASK_UEP_R_RES)) == UEP_R_RES_STALL)
                                        pEP0_DataBuf[0] = 0x01;
                                    break;
                            }
                        }
                        else if((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                        {
                            pEP0_DataBuf[0] = USB_SleepStatus ? 0x02 : 0x00;
                        }
                        pEP0_DataBuf[1] = 0;
                        if(SetupReqLen >= 2)
                            SetupReqLen = 2;
                        break;

                    default:
                        errflag = 0xff;
                        break;
                }
            }

            if(errflag == 0xff)
            {
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
            }
            else
            {
                if(chtype & 0x80)
                {
                    len = (SetupReqLen > EP0_SIZE) ? EP0_SIZE : SetupReqLen;
                    SetupReqLen -= len;
                }
                else
                    len = 0;
                R8_UEP0_T_LEN = len;
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
            }

            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    }
    else if(intflag & RB_UIF_BUS_RST)
    {
        R8_USB_DEV_AD = 0;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    }
    else if(intflag & RB_UIF_SUSPEND)
    {
        if(R8_USB_MIS_ST & RB_UMS_SUSPEND)
        {
            Ready = 0;
        }
        else
        {
            Ready = 1;
        }
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
    else
    {
        R8_USB_INT_FG = intflag;
    }
}

/*********************************************************************
 * USB 设备初始化（替换原来的 USB_Composite_Init / USB_Keyboard_Init）
 */
void USB_Device_Setup(void)
{
    pEP0_RAM_Addr = EP0_Databuf;
    pEP1_RAM_Addr = EP1_Databuf;
    pEP2_RAM_Addr = EP2_Databuf;
    pEP3_RAM_Addr = EP3_Databuf;
    USB_DeviceInit();

    DevConfig = 0;
}

/*********************************************************************
 * USB keyboard report via EP1 IN
 */
void USB_HID_SendReport(uint8_t *buf, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len && i < 8; i++) {
        pEP1_IN_DataBuf[i] = buf[i];
    }
    DevEP1_IN_Deal(len);
}

/*********************************************************************
 * USB IRQ handler
 */
__attribute__((interrupt("WCH-Interrupt-fast")))
__attribute__((section(".highcode")))
void USB_IRQHandler(void)
{
    USB_DevTransProcess();
}

/******************************** endfile @ usb_dev.c ******************************/
