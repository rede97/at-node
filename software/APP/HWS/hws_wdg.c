/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_wdg.c
 * Author             : at-node
 * Description        : HWS watchdog — runtime-armable hardware watchdog.
 ********************************************************************************/

#include "hws_wdg.h"

#if(defined(HWS_WDG)) && (HWS_WDG == TRUE)

static uint8_t wdg_armed = 0;

void hws_wdg_arm(void)
{
    WWDG_SetCounter(0);
    WWDG_ResetCfg(ENABLE);
    wdg_armed = 1;
}

void hws_wdg_disarm(void)
{
    WWDG_ResetCfg(DISABLE);
    wdg_armed = 0;
}

uint8_t hws_wdg_armed(void) { return wdg_armed; }

void hws_wdg_feed_task(void)
{
    if (wdg_armed)
        WWDG_SetCounter(0);
}

#endif /* HWS_WDG == TRUE */
