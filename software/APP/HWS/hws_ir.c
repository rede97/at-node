/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_ir.c
 * Author             : at-node
 * Description        : HWS IR — 38 kHz infrared transmitter.
 *
 *   Carrier: PWM4 on PA12 (bPWM4), 60 MHz / 25 / 64 = 37.5 kHz, 50 %
 *   duty. Mark/space gating: TMR1 period-end interrupt walks a segment
 *   list of µs durations (even index = mark = carrier on, odd = space
 *   = carrier off). Sending is fully interrupt-driven; the AT command
 *   returns immediately and hws_ir_busy() reports on-air state.
 ********************************************************************************/

#include "hws_ir.h"

#if(defined(HWS_IR)) && (HWS_IR == TRUE)

#define IR_MAX_SEG   80   /* NEC needs 2+64+1 = 67; RAW is line-limited */

static volatile uint16_t ir_seg[IR_MAX_SEG];  /* µs, alternating mark/space */
static volatile uint8_t  ir_count, ir_idx, ir_busy;

#define IR_CARRIER_ON()   (R8_PWM_OUT_EN |=  RB_PWM4_OUT_EN)
#define IR_CARRIER_OFF()  (R8_PWM_OUT_EN &= ~RB_PWM4_OUT_EN)
#define US_TO_TMR(us)     ((uint32_t)(us) * (FREQ_SYS / 1000000))

void hws_ir_init(void)
{
    /* PWM4 pin + carrier (output stays disabled until a mark) */
    GPIOA_ModeCfg(bPWM4, GPIO_ModeOut_PP_5mA);
    PWMX_CLKCfg(25);                 /* base = 60 MHz / 25 = 2.4 MHz */
    PWMX_CycleCfg(PWMX_Cycle_64);    /* 2.4 MHz / 64 = 37.5 kHz */
    PWMX_ACTOUT(CH_PWM4, 32, High_Level, ENABLE);  /* 50 % duty, engine off */
    IR_CARRIER_OFF();

    PFIC_EnableIRQ(TMR1_IRQn);
}

static void ir_kick(void)
{
    ir_idx = 0;
    ir_busy = 1;
    IR_CARRIER_ON();                 /* first segment is always a mark */
    TMR1_TimerInit(US_TO_TMR(ir_seg[0]));
    TMR1_ClearITFlag(TMR0_3_IT_CYC_END);
    TMR1_ITCfg(ENABLE, TMR0_3_IT_CYC_END);
    TMR1_Enable();
}

static int ir_send(const uint16_t *us, uint8_t n)
{
    if (ir_busy || n == 0 || n > IR_MAX_SEG)
        return -1;
    for (uint8_t i = 0; i < n; i++)
        ir_seg[i] = us[i];
    ir_count = n;
    ir_kick();
    return 0;
}

int hws_ir_nec(uint32_t code)
{
    uint16_t s[2 + 64 + 1];
    uint8_t n = 0;
    s[n++] = 9000; s[n++] = 4500;              /* leader */
    for (uint8_t i = 0; i < 32; i++) {         /* LSB first */
        s[n++] = 560;
        s[n++] = (code & (1UL << i)) ? 1690 : 560;
    }
    s[n++] = 560;                              /* stop mark */
    return ir_send(s, n);
}

int hws_ir_sirc(uint32_t code, uint8_t bits)
{
    uint16_t s[2 + 2 * 20];
    uint8_t n = 0;
    if (bits == 0 || bits > 20)
        return -1;
    s[n++] = 2400; s[n++] = 600;               /* leader */
    for (uint8_t i = 0; i < bits; i++) {
        s[n++] = (code & (1UL << i)) ? 1200 : 600;
        s[n++] = 600;
    }
    return ir_send(s, n);
}

int hws_ir_raw(const uint16_t *us, uint8_t n)
{
    return ir_send(us, n);
}

uint8_t hws_ir_busy(void) { return ir_busy; }

__attribute__((interrupt("WCH-Interrupt-fast")))
__attribute__((section(".highcode")))
void TMR1_IRQHandler(void)
{
    if (TMR1_GetITFlag(TMR0_3_IT_CYC_END)) {
        TMR1_ClearITFlag(TMR0_3_IT_CYC_END);
        ir_idx++;
        if (ir_idx >= ir_count) {
            IR_CARRIER_OFF();
            TMR1_ITCfg(DISABLE, TMR0_3_IT_CYC_END);
            ir_busy = 0;
            return;
        }
        if (ir_idx & 1) IR_CARRIER_OFF();      /* odd = space */
        else            IR_CARRIER_ON();       /* even = mark */
        TMR1_TimerInit(US_TO_TMR(ir_seg[ir_idx]));
    }
}

#endif /* HWS_IR == TRUE */
