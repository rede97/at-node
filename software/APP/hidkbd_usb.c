/********************************** (C) COPYRIGHT *******************************
 * File Name          : hidkbd_usb.c
 * Author             : at-node
 * Description        : USB keyboard report sender
 ********************************************************************************/

#include "usb_dev.h"

void kb_usb_send_report(uint8_t mods, uint8_t *keys, int count)
{
    uint8_t buf[8];
    buf[0] = mods;
    buf[1] = 0;
    for (int i = 0; i < 6; i++) buf[2+i] = (i < count) ? keys[i] : 0;

    USB_HID_SendReport(buf, 8);
}
