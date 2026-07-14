/********************************** (C) COPYRIGHT *******************************
 * File Name          : AT.h
 * Author             : at-node
 * Version            : V1.0
 * Description        : AT command parser — UART1 + CDC dual channel,
 *                      TMOS-scheduled, response routed to originating channel
 ********************************************************************************/

#ifndef HAL_AT_H
#define HAL_AT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "HAL.h"
#include "usb_dev.h"

/* TMOS event bit */
#define HAL_AT_EVENT    0x0200

/* Max line length */
#define AT_LINE_MAX     256

/* Input channel */
enum { AT_CH_UART, AT_CH_CDC };

typedef int (*at_cmd_handler_t)(int argc, char *argv[]);

typedef struct {
    const char *name;
    const char *help;
    at_cmd_handler_t handler;
} at_cmd_t;

void AT_Init(at_cmd_t *table, int count);
void AT_Response(const char *fmt, ...);
void AT_Poll(void);

#ifdef __cplusplus
}
#endif

#endif
