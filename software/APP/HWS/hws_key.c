/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_key.c
 * Author             : at-node
 * Description        : Key scanning — polled GPIO input with change detection callback
 ********************************************************************************/

#include "hws.h"

static uint8_t        hws_key_saved;
static hws_key_cb_t   hws_key_cb;

/*********************************************************************
 * @fn      hws_key_init
 *
 * @brief   Initialize key scanning hardware and start TMOS poll task.
 *********************************************************************/
void hws_key_init(void)
{
    hws_key_saved = 0;
    hws_key_cb    = NULL;
    KEY1_DIR; KEY1_PU;
    KEY2_DIR; KEY2_PU;
    tmos_start_task(hws_task_id, HWS_KEY_EVENT, HWS_KEY_POLL_MS);
}

/*********************************************************************
 * @fn      hws_key_config
 *
 * @brief   Register key-change callback.
 *********************************************************************/
void hws_key_config(hws_key_cb_t cback)
{
    hws_key_cb = cback;
}

/*********************************************************************
 * @fn      hws_key_read
 *
 * @brief   Read current key state (no callback, no change detection).
 *********************************************************************/
uint8_t hws_key_read(void)
{
    uint8_t keys = 0;
    if (HWS_PUSH_BUTTON1()) keys |= HWS_KEY_SW_1;
    if (HWS_PUSH_BUTTON2()) keys |= HWS_KEY_SW_2;
    if (HWS_PUSH_BUTTON3()) keys |= HWS_KEY_SW_3;
    if (HWS_PUSH_BUTTON4()) keys |= HWS_KEY_SW_4;
    return keys;
}

/*********************************************************************
 * @fn      hws_key_poll
 *
 * @brief   Poll keys — called periodically by HWS task.
 *          Invokes callback only on state change.
 *********************************************************************/
void hws_key_poll(void)
{
    uint8_t keys = 0;
    if (HWS_PUSH_BUTTON1()) keys |= HWS_KEY_SW_1;
    if (HWS_PUSH_BUTTON2()) keys |= HWS_KEY_SW_2;
    if (HWS_PUSH_BUTTON3()) keys |= HWS_KEY_SW_3;
    if (HWS_PUSH_BUTTON4()) keys |= HWS_KEY_SW_4;

    if (keys == hws_key_saved)
        return;

    hws_key_saved = keys;
    if (hws_key_cb)
        hws_key_cb(keys);
}

/******************************** endfile @ hws_key ******************************/
