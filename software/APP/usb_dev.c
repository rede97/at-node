/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_dev.c
 * Author             : at-node
 * Version            : V2.0
 * Description        : USB composite device — CDC ACM + HID Keyboard
 *                      CDC:  EP1 BULK data + EP3 interrupt notification
 *                      HID:  EP2 interrupt keyboard + LED
 *                      Based on WCH EVT HID_CompliantDev structure
 ********************************************************************************/

#include "CH58x_common.h"
#include <string.h>

#define EP0_SIZE 0x40
#define EP1_SIZE 0x40   // CDC BULK
#define EP2_SIZE 8       // HID Keyboard interrupt
#define EP3_SIZE 8       // CDC notification

/* === Descriptor type codes === */
#define USB_DESCR_TYP_DEVICE     0x01
#define USB_DESCR_TYP_CONFIG     0x02
#define USB_DESCR_TYP_STRING     0x03
#define USB_DESCR_TYP_HID        0x21
#define USB_DESCR_TYP_REPORT     0x22

/* === HID class requests === */
#define DEF_USB_SET_IDLE         0x0A
#define DEF_USB_SET_REPORT       0x09
#define DEF_USB_SET_PROTOCOL     0x0B
#define DEF_USB_GET_IDLE         0x02
#define DEF_USB_GET_PROTOCOL     0x03
#define DEF_USB_GET_REPORT       0x01

/* === CDC class requests === */
#define CDC_SET_LINE_CODING      0x20
#define CDC_GET_LINE_CODING      0x21
#define CDC_SET_CONTROL_LINE     0x22
#define CDC_SEND_BREAK           0x23

/* === CDC Line Coding struct === */
typedef struct __attribute__((packed)) {
    uint32_t BaudRate;
    uint8_t  StopBits;
    uint8_t  ParityType;
    uint8_t  DataBits;
} LINE_CODING;

static LINE_CODING lineCoding = { 115200, 0, 0, 8 };
static uint8_t    cdcReady = 0;

/*********************************************************************
 *  Endpoint DMA buffers
 */
__attribute__((aligned(4))) uint8_t EP0_Buf[64 + 64 + 64];   // EP0 + EP4_out + EP4_in
__attribute__((aligned(4))) uint8_t EP1_Buf[64 + 64];       // EP1 CDC OUT + IN
__attribute__((aligned(4))) uint8_t EP2_Buf[64 + 64];       // EP2 HID OUT + IN
__attribute__((aligned(4))) uint8_t EP3_Buf[64 + 64];       // EP3 CDC notify

/*********************************************************************
 *  String descriptors
 */
static const uint8_t LangDescr[]  = { 0x04, 0x03, 0x09, 0x04 };
static const uint8_t ManuInfo[]   = { 0x0C, 0x03, 'a',0,'t',0,'-',0,'n',0,'o',0,'d',0,'e',0 };
static const uint8_t ProdInfo[]   = { 0x24, 0x03,
    'a',0,'t',0,'-',0,'n',0,'o',0,'d',0,'e',0,' ',0,'C',0,'D',0,'C',0,'+',0,'H',0,'I',0,'D',0 };
static const uint8_t SerialInfo[] = { 0x1A, 0x03,
    '2',0,'0',0,'2',0,'6',0,'0',0,'7',0,'0',0,'1',0,'A',0,'B',0,'C',0,'D',0,0,0 };

/*********************************************************************
 *  HID Report Descriptor — standard keyboard (68 bytes)
 */
static const uint8_t HIDDescr[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x26,0xFF,0x00,
    0x05,0x07, 0x19,0x00, 0x29,0xFF, 0x81,0x00,
    0x05,0x08, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01, 0xC0
};

/*********************************************************************
 *  Device descriptor — class 0xEF for IAD composite
 */
static const uint8_t MyDevDescr[] = {
    0x12,0x01,0x00,0x02,0xEF,0x02,0x01,EP0_SIZE,
    0x86,0x1A,0x07,0x21,0x00,0x00,0x01,0x02,0x00,0x01
};

/*********************************************************************
 *  Configuration descriptor — IAD + CDC + HID
 *  Layout: Config(9) + IAD(8) + CDC_Comm(9+5+4+5+5+7=35) +
 *          CDC_Data(9+7+7=23) + HID(9+9+7+7=32) = 107 bytes = 0x6B
 */
static const uint8_t MyCfgDescr[] = {
    0x09,0x02,0x6B,0x00,0x03,0x01,0x00,0x80,0x32,
    // IAD: interfaces 0+1 = CDC
    0x08,0x0B,0x00,0x02,0x02,0x02,0x01,0x00,
    // Interface 0: CDC Communication
    0x09,0x04,0x00,0x00,0x01,0x02,0x02,0x01,0x00,
    0x05,0x24,0x00,0x10,0x01,
    0x04,0x24,0x02,0x02,
    0x05,0x24,0x06,0x00,0x01,
    0x05,0x24,0x01,0x01,0x00,
    0x07,0x05,0x83,0x03,EP3_SIZE,0x00,0x01,   // EP3 IN, interrupt
    // Interface 1: CDC Data
    0x09,0x04,0x01,0x00,0x02,0x0A,0x00,0x00,0x00,
    0x07,0x05,0x01,0x02,EP1_SIZE,0x00,0x00,    // EP1 OUT, bulk
    0x07,0x05,0x81,0x02,EP1_SIZE,0x00,0x00,    // EP1 IN, bulk
    // Interface 2: HID Keyboard
    0x09,0x04,0x02,0x00,0x02,0x03,0x01,0x01,0x00,
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,
        sizeof(HIDDescr),0x00,
    0x07,0x05,0x82,0x03,EP2_SIZE,0x00,0x0A,    // EP2 IN, interrupt
    0x07,0x05,0x02,0x03,EP2_SIZE,0x00,0x0A,    // EP2 OUT, interrupt
};

/*********************************************************************
 *  Global state
 */
static uint8_t  DevConfig = 0;
uint8_t         Ready = 0;
static uint8_t  SetupReqCode;
static uint16_t SetupReqLen;
const uint8_t  *pDescr;
static uint8_t  Idle_Value[2] = {0,0};

/*********************************************************************
 *  CDC data buffer for UART bridge
 */
static uint8_t  cdc_rx_buf[64];
static uint16_t cdc_rx_len = 0;

/*********************************************************************
 *  CDC data receiver (called from USB_DevTransProcess)
 *  Copies received EP1 OUT data to cdc_rx_buf for UART forwarding
 */
static void cdc_recv_data(uint8_t len)
{
    if (len > sizeof(cdc_rx_buf)) len = sizeof(cdc_rx_buf);
    memcpy(cdc_rx_buf, pEP1_OUT_DataBuf, len);
    cdc_rx_len = len;
}

/*********************************************************************
 *  USB_DevTransProcess — handles all USB token processing
 */
void USB_DevTransProcess(void)
{
    uint8_t len, chtype;
    uint8_t intflag, errflag = 0;

    intflag = R8_USB_INT_FG;

    if (intflag & RB_UIF_TRANSFER)
    {
        /* Token dispatch (non-idle) */
        if ((R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN)
        {
            switch (R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
            {
                case UIS_TOKEN_IN:
                {
                    switch (SetupReqCode) {
                        case USB_GET_DESCRIPTOR:
                            len = SetupReqLen >= EP0_SIZE ? EP0_SIZE : SetupReqLen;
                            memcpy(EP0_Buf, pDescr, len);
                            SetupReqLen -= len;
                            pDescr += len;
                            R8_UEP0_T_LEN = len;
                            R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                            break;
                        case USB_SET_ADDRESS:
                            R8_USB_DEV_AD = (R8_USB_DEV_AD & ~0x7F) | (uint8_t)SetupReqLen;
                            R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                            break;
                        default:
                            R8_UEP0_T_LEN = 0;
                            R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                            Ready = 1;
                            break;
                    }
                    break;
                }

                case UIS_TOKEN_OUT:
                    len = R8_USB_RX_LEN;
                    break;

                /* EP1 — CDC data */
                case UIS_TOKEN_IN | 1:
                    R8_UEP1_CTRL ^= RB_UEP_T_TOG;
                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                    break;

                case UIS_TOKEN_OUT | 1:
                    if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                        R8_UEP1_CTRL ^= RB_UEP_R_TOG;
                        cdc_recv_data(R8_USB_RX_LEN);
                    }
                    break;

                /* EP2 — HID keyboard + LED */
                case UIS_TOKEN_IN | 2:
                    R8_UEP2_CTRL ^= RB_UEP_T_TOG;
                    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                    break;

                case UIS_TOKEN_OUT | 2:
                    if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                        R8_UEP2_CTRL ^= RB_UEP_R_TOG;
                    }
                    break;

                default:
                    break;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }

        /* SETUP packet processing */
        if (R8_USB_INT_ST & RB_UIS_SETUP_ACT)
        {
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;

            USB_SETUP_REQ *pReq = (USB_SETUP_REQ *)EP0_Buf;
            SetupReqLen = pReq->wLength;
            SetupReqCode = pReq->bRequest;
            chtype = pReq->bRequestType;
            len = 0;
            errflag = 0;

            /* --- Non-standard requests --- */
            if ((chtype & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD)
            {
                uint8_t recipient = chtype & 0x1F;
                uint8_t wIndexLo  = pReq->wIndex & 0xFF;

                if (recipient == 0x01) /* Interface recipient */
                {
                    if (wIndexLo == 2) /* HID interface 2 */
                    {
                        switch (SetupReqCode) {
                            case DEF_USB_SET_IDLE:
                                Idle_Value[wIndexLo] = (uint8_t)(pReq->wValue>>8); break;
                            case DEF_USB_SET_REPORT: break;
                            case DEF_USB_SET_PROTOCOL: break;
                            case DEF_USB_GET_IDLE:
                                EP0_Buf[0] = Idle_Value[wIndexLo]; len = 1; break;
                            case DEF_USB_GET_PROTOCOL:
                                EP0_Buf[0] = 0; len = 1; break;
                            default: errflag = 0xFF; break;
                        }
                    }
                    else if (wIndexLo == 0) /* CDC Communication interface 0 */
                    {
                        switch (SetupReqCode) {
                            case CDC_SET_LINE_CODING:
                            case CDC_SET_CONTROL_LINE:
                                cdcReady = 1; break;
                            case CDC_GET_LINE_CODING:
                                memcpy(EP0_Buf, &lineCoding, sizeof(LINE_CODING));
                                len = sizeof(LINE_CODING); break;
                            case CDC_SEND_BREAK:
                                break;
                            default: errflag = 0xFF; break;
                        }
                    }
                    else errflag = 0xFF;
                }
                else errflag = 0xFF;
            }
            /* --- Standard requests --- */
            else
            {
                switch (SetupReqCode)
                {
                    case USB_GET_DESCRIPTOR:
                    {
                        uint8_t dt = (pReq->wValue) >> 8;
                        switch (dt) {
                            case USB_DESCR_TYP_DEVICE:
                                pDescr = MyDevDescr; len = MyDevDescr[0]; break;
                            case USB_DESCR_TYP_CONFIG:
                                pDescr = MyCfgDescr; len = MyCfgDescr[2]; break;
                            case USB_DESCR_TYP_HID:
                                pDescr = (uint8_t *)(&MyCfgDescr[84]); len = 9; break;
                            case USB_DESCR_TYP_REPORT:
                                pDescr = HIDDescr; len = sizeof(HIDDescr); break;
                            case USB_DESCR_TYP_STRING:
                                switch (pReq->wValue & 0xFF) {
                                    case 0: pDescr = LangDescr;  len = LangDescr[0];  break;
                                    case 1: pDescr = ManuInfo;   len = ManuInfo[0];   break;
                                    case 2: pDescr = ProdInfo;   len = ProdInfo[0];   break;
                                    case 3: pDescr = SerialInfo; len = SerialInfo[0]; break;
                                    default: errflag = 0xFF; break;
                                } break;
                            default: errflag = 0xFF; break;
                        }
                        if (errflag != 0xFF) {
                            if (SetupReqLen > len) SetupReqLen = len;
                            len = (SetupReqLen > EP0_SIZE) ? EP0_SIZE : SetupReqLen;
                            memcpy(EP0_Buf, pDescr, len);
                            pDescr += len;
                        }
                        break;
                    }

                    case USB_SET_ADDRESS:
                        SetupReqLen = (pReq->wValue) & 0xFF; break;
                    case USB_GET_CONFIGURATION:
                        EP0_Buf[0] = DevConfig;
                        if (SetupReqLen > 1) SetupReqLen = 1; break;
                    case USB_SET_CONFIGURATION:
                        DevConfig = 1; Ready = 1; break;
                    case USB_GET_INTERFACE:
                        EP0_Buf[0] = 0;
                        if (SetupReqLen > 1) SetupReqLen = 1; break;
                    case USB_SET_INTERFACE: break;
                    case USB_CLEAR_FEATURE:
                        if ((chtype & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                            switch (pReq->wIndex & 0xFF) {
                                case 0x81: /* EP1 IN */
                                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG|MASK_UEP_T_RES)) | UEP_T_RES_NAK; break;
                                case 0x01: /* EP1 OUT */
                                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG|MASK_UEP_R_RES)) | UEP_R_RES_ACK; break;
                                case 0x82:
                                    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG|MASK_UEP_T_RES)) | UEP_T_RES_NAK; break;
                                case 0x02:
                                    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_R_TOG|MASK_UEP_R_RES)) | UEP_R_RES_ACK; break;
                                default: errflag = 0xFF; break;
                            }
                        } break;
                    case USB_SET_FEATURE:
                        if ((chtype & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                            switch (pReq->wIndex & 0xFF) {
                                case 0x81: R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG|MASK_UEP_T_RES)) | UEP_T_RES_STALL; break;
                                case 0x01: R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG|MASK_UEP_R_RES)) | UEP_R_RES_STALL; break;
                                case 0x82: R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG|MASK_UEP_T_RES)) | UEP_T_RES_STALL; break;
                                case 0x02: R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_R_TOG|MASK_UEP_R_RES)) | UEP_R_RES_STALL; break;
                                default: errflag = 0xFF; break;
                            }
                        } break;
                    case USB_GET_STATUS:
                        EP0_Buf[0] = 0x00; EP0_Buf[1] = 0;
                        if (SetupReqLen >= 2) SetupReqLen = 2; break;
                    default:
                        errflag = 0xFF; break;
                }
            }

            /* Complete SETUP */
            if (errflag == 0xFF)
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
            else {
                if (chtype & 0x80) {
                    len = (SetupReqLen > EP0_SIZE) ? EP0_SIZE : SetupReqLen;
                    SetupReqLen -= len;
                } else
                    len = 0;
                R8_UEP0_T_LEN = len;
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    }
    else if (intflag & RB_UIF_BUS_RST)
    {
        R8_USB_DEV_AD = 0;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP3_CTRL = UEP_T_RES_NAK;
        DevConfig = 0;
        Ready = 0;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    }
    else if (intflag & RB_UIF_SUSPEND)
    {
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
    else
    {
        R8_USB_INT_FG = intflag;
    }
}

/*********************************************************************
 *  CDC data write — sends data up to host via EP1 IN
 */
static void cdc_send_packet(const uint8_t *data, uint16_t len)
{
    while (len > 0) {
        uint16_t chunk = len > EP1_SIZE ? EP1_SIZE : len;
        while ((R8_UEP1_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK);
        memcpy(pEP1_IN_DataBuf, data, chunk);
        R8_UEP1_T_LEN = (uint8_t)chunk;
        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
        data += chunk;
        len -= chunk;
    }
}

/*********************************************************************
 *  CDC data read — get received data from EP1 OUT
 */
uint16_t USB_CDC_Read(uint8_t *buf, uint16_t maxlen)
{
    uint16_t n = cdc_rx_len;
    if (n > maxlen) n = maxlen;
    if (n > 0) {
        memcpy(buf, cdc_rx_buf, n);
        cdc_rx_len = 0;
    }
    return n;
}

/*********************************************************************
 *  CDC data write entry point
 */
void USB_CDC_Write(const uint8_t *data, uint16_t len)
{
    if (DevConfig && cdcReady)
        cdc_send_packet(data, len);
}

/*********************************************************************
 *  USB HID keyboard report send (same API as before)
 */
void USB_HID_SendReport(uint8_t *buf, uint8_t len)
{
    PRINT("USB_HID: key=%02x\n", buf[2]);
    uint8_t i;
    for (i = 0; i < len && i < EP2_SIZE; i++)
        pEP2_IN_DataBuf[i] = buf[i];
    if (len > EP2_SIZE) len = EP2_SIZE;
    R8_UEP2_T_LEN = len;
    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
}

/*********************************************************************
 *  USB device initialization
 */
void USB_Device_Setup(void)
{
    pEP0_RAM_Addr = EP0_Buf;
    pEP1_RAM_Addr = EP1_Buf;
    pEP2_RAM_Addr = EP2_Buf;
    pEP3_RAM_Addr = EP3_Buf;

    R8_USB_CTRL = 0x00;

    R8_UEP4_1_MOD = RB_UEP1_TX_EN | RB_UEP1_RX_EN;           // EP1: CDC bulk
    R8_UEP2_3_MOD = RB_UEP2_TX_EN | RB_UEP2_RX_EN | RB_UEP3_TX_EN; // EP2: HID, EP3: notify

    R16_UEP0_DMA = (uint16_t)(uint32_t)EP0_Buf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)EP1_Buf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)EP2_Buf;
    R16_UEP3_DMA = (uint16_t)(uint32_t)EP3_Buf;

    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP3_CTRL = UEP_T_RES_NAK;

    R8_USB_DEV_AD = 0x00;
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;

    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;
    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;

    R8_USB_INT_FG = 0xFF;
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_BUS_RST | RB_UIE_TRANSFER;

    DevConfig = 0;
    cdcReady = 0;
    cdc_rx_len = 0;
}

/*********************************************************************
 *  USB interrupt handler
 */
__attribute__((interrupt("WCH-Interrupt-fast")))
__attribute__((section(".highcode")))
void USB_IRQHandler(void)
{
    USB_DevTransProcess();
}

/******************************** endfile @ usb_dev.c ******************************/