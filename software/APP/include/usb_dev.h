/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_dev.h
 * Author             : at-node
 * Version            : V1.0
 * Description        : USB HID 键盘设备头文件
 ********************************************************************************/

#ifndef USB_DEV_H
#define USB_DEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CH58x_common.h"

extern void USB_Device_Setup(void);
extern void USB_DevTransProcess(void);
extern void USB_HID_SendReport(uint8_t *buf, uint8_t len);
extern void USB_CDC_Write(const uint8_t *data, uint16_t len);
extern uint16_t USB_CDC_Read(uint8_t *buf, uint16_t maxlen);

#ifdef __cplusplus
}
#endif

#endif /* USB_DEV_H */
