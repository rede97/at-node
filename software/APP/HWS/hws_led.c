/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_led.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
#include "hws.h"

/* LED control structure */
typedef struct
{
    uint8_t  mode;  /* Operation mode */
    uint8_t  todo;  /* Blink cycles left */
    uint8_t  onPct; /* On cycle percentage */
    uint16_t time;  /* On/off cycle time (msec) */
    uint32_t next;  /* Time for next change */
} hws_led_ctrl_t;

typedef struct
{
    hws_led_ctrl_t ctrl[HWS_LED_DEFAULT_MAX_LEDS];
    uint8_t        sleep_active;
} hws_led_status_t;

/***************************************************************************************************
 *                                           GLOBAL VARIABLES
 ***************************************************************************************************/
static uint8_t           hws_led_state;       /* LED state at last set/clr/blink update */
static uint8_t           hws_led_pre_blink;   /* state saved before blink began */
static hws_led_status_t  hws_led_status;

/***************************************************************************************************
 *                                            LOCAL FUNCTION
 ***************************************************************************************************/
void hws_led_on_off(uint8_t leds, uint8_t mode);

/***************************************************************************************************
 *                                            FUNCTIONS - API
 ***************************************************************************************************/

/*********************************************************************
 * @fn      hws_led_init
 *
 * @brief   Initialize LED Service
 *
 * @return  none
 */
void hws_led_init(void)
{
    /* Initialize all LEDs to OFF */
    LED1_DDR;
    hws_led_set(HWS_LED_ALL, HWS_LED_MODE_OFF);
    // just test
    // hws_led_blink(HWS_LED_1, 1, 30, 000);
    /* Initialize sleepActive to FALSE */
    hws_led_status.sleep_active = FALSE;
}

/*********************************************************************
 * @fn      hws_led_set
 *
 * @brief   Turn ON/OFF/TOGGLE given LEDs
 *
 * @param   led     - bit mask value of leds to be turned ON/OFF/TOGGLE
 * @param   mode    - BLINK, FLASH, TOGGLE, ON, OFF
 *
 * @return  0
 */
uint8_t hws_led_set(uint8_t leds, uint8_t mode)
{
    uint8_t          led;
    hws_led_ctrl_t *sts;

    switch(mode)
    {
        case HWS_LED_MODE_BLINK:
        {
            /* Default blink, 1 time, D% duty cycle */
            hws_led_blink(leds, 1, HWS_LED_DEFAULT_DUTY_CYCLE, HWS_LED_DEFAULT_FLASH_TIME);
            break;
        }

        case HWS_LED_MODE_FLASH:
        {
            /* Default flash, N times, D% duty cycle */
            hws_led_blink(leds, HWS_LED_DEFAULT_FLASH_COUNT, HWS_LED_DEFAULT_DUTY_CYCLE, HWS_LED_DEFAULT_FLASH_TIME);
            break;
        }

        case HWS_LED_MODE_ON:
        case HWS_LED_MODE_OFF:
        case HWS_LED_MODE_TOGGLE:
        {
            led = HWS_LED_1;
            leds &= HWS_LED_ALL;
            sts = hws_led_status.ctrl;
            while(leds)
            {
                if(leds & led)
                {
                    if(mode != HWS_LED_MODE_TOGGLE)
                    {
                        sts->mode = mode; /* ON or OFF */
                    }
                    else
                    {
                        sts->mode ^= HWS_LED_MODE_ON; /* Toggle */
                    }
                    hws_led_on_off(led, sts->mode);
                    leds ^= led;
                }
                led <<= 1;
                sts++;
            }
            break;
        }

        default:
            break;
    }
    return (0);
}

/*********************************************************************
 * @fn      hws_led_blink
 *
 * @brief   Blink the leds
 *
 * @param   led         - bit mask value of leds to be turned ON/OFF/TOGGLE
 * @param   numBlinks   - number of blinks
 * @param   percent     - the percentage in each period where the led will be on
 * @param   period      - length of each cycle in milliseconds
 *
 * @return  none
 */
void hws_led_blink(uint8_t leds, uint8_t numBlinks, uint8_t percent, uint16_t period)
{
    uint8_t          led;
    hws_led_ctrl_t *sts;

    if(leds && percent && period)
    {
        if(percent < 100)
        {
            led = HWS_LED_1;
            leds &= HWS_LED_ALL;
            sts = hws_led_status.ctrl;
            while(leds)
            {
                if(leds & led)
                {
                    /* Store the current state of the led before going to blinking */
                    hws_led_pre_blink |= (led & hws_led_state);
                    sts->mode = HWS_LED_MODE_OFF; /* Stop previous blink */
                    sts->time = period;           /* Time for one on/off cycle */
                    sts->onPct = percent;         /* % of cycle LED is on */
                    sts->todo = numBlinks;        /* Number of blink cycles */
                    if(!numBlinks)
                    {
                        sts->mode |= HWS_LED_MODE_FLASH; /* Continuous */
                    }
                    sts->next = TMOS_GetSystemClock(); /* Start now */
                    sts->mode |= HWS_LED_MODE_BLINK;   /* Enable blinking */
                    leds ^= led;
                }
                led <<= 1;
                sts++;
            }
            tmos_start_task(hws_task_id, HWS_LED_BLINK_EVENT, 0);
        }
        else
        {
            hws_led_set(leds, HWS_LED_MODE_ON); /* >= 100%, turn on */
        }
    }
    else
    {
        hws_led_set(leds, HWS_LED_MODE_OFF); /* No on time, turn off */
    }
}

/*********************************************************************
 * @fn      hws_led_update
 *
 * @brief   Update leds to work with blink
 *
 * @return  none
 */
void hws_led_update(void)
{
    uint8_t          led, pct, leds;
    uint16_t         next, wait;
    uint32_t         time;
    hws_led_ctrl_t *sts;

    next = 0;
    led = HWS_LED_1;
    leds = HWS_LED_ALL;
    sts = hws_led_status.ctrl;

    /* Check if sleep is active or not */
    if(!hws_led_status.sleep_active)
    {
        while(leds)
        {
            if(leds & led)
            {
                if(sts->mode & HWS_LED_MODE_BLINK)
                {
                    time = TMOS_GetSystemClock();
                    if(time >= sts->next)
                    {
                        if(sts->mode & HWS_LED_MODE_ON)
                        {
                            pct = 100 - sts->onPct;             /* Percentage of cycle for off */
                            sts->mode &= ~HWS_LED_MODE_ON;      /* Say it's not on */
                            hws_led_on_off(led, HWS_LED_MODE_OFF); /* Turn it off */
                            if(!(sts->mode & HWS_LED_MODE_FLASH))
                            {
                                if(sts->todo != 0xff)
                                {
                                    sts->todo--; /* Not continuous, reduce count */
                                }
                                if(!sts->todo)
                                {
                                    sts->mode ^= HWS_LED_MODE_BLINK; /* No more blinks */
                                }
                            }
                        }
                        else
                        {
                            pct = sts->onPct;                  /* Percentage of cycle for on */
                            sts->mode |= HWS_LED_MODE_ON;      /* Say it's on */
                            hws_led_on_off(led, HWS_LED_MODE_ON); /* Turn it on */
                        }
                        if(sts->mode & HWS_LED_MODE_BLINK)
                        {
                            wait = (((uint32_t)pct * (uint32_t)sts->time) / 100);
                            sts->next = time + wait;
                        }
                        else
                        {
                            /* no more blink, no more wait */
                            wait = 0;
                            /* After blinking, set the LED back to the state before it blinks */
                            hws_led_set(led, ((hws_led_pre_blink & led) != 0) ? HWS_LED_MODE_ON : HWS_LED_MODE_OFF);
                            /* Clear the saved bit */
                            hws_led_pre_blink &= (led ^ 0xFF);
                        }
                    }
                    else
                    {
                        wait = sts->next - time; /* Time left */
                    }
                    if(!next || (wait && (wait < next)))
                    {
                        next = wait;
                    }
                }
                leds ^= led;
            }
            led <<= 1;
            sts++;
        }
        if(next)
        {
            tmos_start_task(hws_task_id, HWS_LED_BLINK_EVENT, next); /* Schedule event */
        }
    }
}

/*********************************************************************
 * @fn      hws_led_on_off
 *
 * @brief   Turns specified LED ON or OFF
 *
 * @param   led         - LED bit mask
 * @param   mode        - LED_ON,LED_OFF,
 *
 * @return  none
 */
void hws_led_on_off(uint8_t leds, uint8_t mode)
{
    if(leds & HWS_LED_1)
    {
        if(mode == HWS_LED_MODE_ON)
        {
            HWS_TURN_ON_LED1();
        }
        else
        {
            HWS_TURN_OFF_LED1();
        }
    }
    if(leds & HWS_LED_2)
    {
        if(mode == HWS_LED_MODE_ON)
        {
            HWS_TURN_ON_LED2();
        }
        else
        {
            HWS_TURN_OFF_LED2();
        }
    }
    if(leds & HWS_LED_3)
    {
        if(mode == HWS_LED_MODE_ON)
        {
            HWS_TURN_ON_LED3();
        }
        else
        {
            HWS_TURN_OFF_LED3();
        }
    }
    if(leds & HWS_LED_4)
    {
        if(mode == HWS_LED_MODE_ON)
        {
            HWS_TURN_ON_LED4();
        }
        else
        {
            HWS_TURN_OFF_LED4();
        }
    }
    /* Remember current state */
    if(mode)
    {
        hws_led_state |= leds;
    }
    else
    {
        hws_led_state &= (leds ^ 0xFF);
    }
}

/*********************************************************************
 * @fn      hws_led_get_state
 *
 * @brief   Return current LED on/off state bitmask.
 *********************************************************************/
uint8_t hws_led_get_state(void)
{
    return hws_led_state;
}

/******************************** endfile @ led ******************************/
