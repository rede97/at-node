/********************************** (C) COPYRIGHT *******************************
 * File Name          : at_parser.h
 * Author             : at-node
 * Version            : V1.0
 * Description        : AT command parser — UART1 + CDC dual channel,
 *                      independent TMOS task, not coupled to HAL
 ********************************************************************************/

#ifndef AT_PARSER_H
#define AT_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CH58x_common.h"
#include "usb_dev.h"

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

extern const at_cmd_t cmd_table[];
extern const int cmd_table_count;

#ifdef __cplusplus
}
#endif

#endif /* AT_PARSER_H */
