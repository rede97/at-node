/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_wdg.h
 * Author             : at-node
 * Description        : HWS watchdog — runtime-armable hardware watchdog.
 *
 *   Default DISARMED at boot; armed explicitly via AT+WDG=1.
 *   Feed runs as a periodic HWS table task (100 ms) while armed.
 *
 *   NOTE (2026-07-22): the CH582 hardware dog tops out at ~0.56 s, so
 *   it is only safe because every stall source is now bounded — CDC TX
 *   is async (TMOS drain task), UART TX is hardware-clocked. Any new
 *   unbounded busy-wait WILL trip it (which is exactly its purpose).
 ********************************************************************************/

#ifndef HWS_WDG_H
#define HWS_WDG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

void    hws_wdg_arm(void);
void    hws_wdg_disarm(void);
uint8_t hws_wdg_armed(void);

/* periodic feed — HWS table task, do not call directly */
void    hws_wdg_feed_task(void);

#ifdef __cplusplus
}
#endif

#endif /* HWS_WDG_H */
