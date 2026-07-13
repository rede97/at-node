/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_dev.h
 * Author             : at-node
 * Version            : V1.0
 * Description        : USB 复合设备（CDC + HID 键盘）头文件
 ********************************************************************************/

#ifndef USB_DEV_H
#define USB_DEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CH58x_common.h"

/*********************************************************************
 * 初始化
 */
extern void USB_Composite_Init(void);

/*********************************************************************
 * USB 中断处理（由 USB_IRQHandler 调用）
 * 在中断中仅将事件入队，在主循环中处理
 */
extern void USB_DevTransProcess(void);

/*********************************************************************
 * USB 传输状态查询
 */
extern uint8_t USB_CDC_IsReady(void);    // CDC 接口是否已配置完成
extern uint8_t USB_HID_IsReady(void);    // HID 接口是否已配置完成

/*********************************************************************
 * 数据发送接口（上层调用）
 * 用于后续实现 CDC 收发和 HID 报告发送
 */
extern void USB_CDC_SendData(uint8_t *pData, uint16_t len);
extern void USB_HID_SendReport(uint8_t *pData, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* USB_DEV_H */
