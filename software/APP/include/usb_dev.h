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

#ifdef __cplusplus
}
#endif

#endif /* USB_DEV_H */
