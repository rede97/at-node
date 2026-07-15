/********************************** (C) COPYRIGHT *******************************
 * File Name          : AT.c
 * Author             : at-node
 * Version            : V1.0
 * Description        : AT parser — UART1 + CDC dual channel, TMOS 10ms polling
 *
 *   UART1 RX → ring buffer   }  both feed into line parser
 *   CDC RX   → USB_CDC_Read  }
 *   Response routed back to originating channel.
 *   CDC input is echoed (line-buffered) before AT processing.
 ********************************************************************************/

#include "config.h"
#include "at_parser.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <strings.h>

/* --- ring buffer (UART1) --- */
#define AT_RX_BUF_SIZE  512
static uint8_t  at_rx_buf[AT_RX_BUF_SIZE];
static uint16_t at_rx_head = 0;
static uint16_t at_rx_tail = 0;

/* --- line buffer --- */
static char  at_line[AT_LINE_MAX];
static int   at_line_len = 0;

/* --- command table --- */
static at_cmd_t *atCmdTable = NULL;
static int       atCmdCount = 0;

/* --- channel tracking --- */
static int       at_channel = AT_CH_UART;

/* --- arg parsing --- */
static char *at_argv[8];
static char  at_token_buf[AT_LINE_MAX];

/* --- TMOS --- */
#define AT_EVENT  0x0001
static tmosTaskID at_task_id = INVALID_TASK_ID;

static tmosEvents AT_ProcessEvent(tmosTaskID tid, tmosEvents evt)
{
    (void)tid;
    if (evt & AT_EVENT) {
        AT_Poll();
        tmos_start_task(at_task_id, AT_EVENT, MS1_TO_SYSTEM_TIME(10));
        return evt ^ AT_EVENT;
    }
    return 0;
}

void AT_Init(at_cmd_t *table, int count)
{
    atCmdTable = table;
    atCmdCount = count;
    at_task_id = TMOS_ProcessEventRegister(AT_ProcessEvent);
    tmos_start_task(at_task_id, AT_EVENT, MS1_TO_SYSTEM_TIME(10));
}

/*******************************************************************************
 * @fn      at_init
 *
 * @brief   Initialize AT command parser with the built-in command table.
 *
 *   Init stage: 3 — called after hws_init(), before ble_peripheral_init().
 *   Depends on: hws_init() — TMOS scheduler must be running.
 *               UART1 — must be initialized (done by hws_platform_init if DEBUG).
 *   Side effects:
 *     - Registers AT TMOS task polling at 10 ms (UART1 + CDC dual channel).
 *     - AT commands become available immediately after this call.
 *     - cmd_table and cmd_table_count are defined in at_cmds.c.
 *
 *   This wrapper exists to keep main() clean: the command table and count
 *   are compilation-unit internals that callers shouldn't need to know.
 */
void at_init(void)
{
    AT_Init((at_cmd_t *)cmd_table, cmd_table_count);
}

static void at_write_uart(uint8_t ch)
{
    while (!(R8_UART1_LSR & RB_LSR_TX_FIFO_EMP));
    R8_UART1_THR = ch;
}

void AT_Response(const char *fmt, ...)
{
    char buf[256];
    va_list va;
    va_start(va, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);

    if (len < (int)sizeof(buf) - 2) {
        buf[len++] = '\r';
        buf[len++] = '\n';
        buf[len]   = '\0';
    }

    if (at_channel == AT_CH_CDC) {
        USB_CDC_Write((const uint8_t *)buf, len);
    } else {
        for (int i = 0; i < len; i++)
            at_write_uart(buf[i]);
    }
}

static void at_process_line(void)
{
    int len = strlen(at_line);
    if (len > 0 && at_line[len - 1] == '\r')
        at_line[--len] = '\0';
    if (len == 0) return;

    strcpy(at_token_buf, at_line);
    int argc = 0;
    char *save;
    char *tok = strtok_r(at_token_buf, "=", &save);
    if (tok) {
        at_argv[argc++] = tok;
        if (save && *save) {
            char *args_save;
            char *arg = strtok_r(save, ",", &args_save);
            while (arg && argc < 8) {
                at_argv[argc++] = arg;
                arg = strtok_r(NULL, ",", &args_save);
            }
        }
    }

    int found = 0;
    for (int i = 0; i < atCmdCount; i++) {
        if (strcasecmp(at_argv[0], atCmdTable[i].name) == 0) {
            int ret = atCmdTable[i].handler(argc, at_argv);
            AT_Response(ret == 0 ? "OK" : "ERROR");
            found = 1;
            break;
        }
    }
    if (!found)
        AT_Response("ERROR");
}

void AT_Poll(void)
{
    /* --- 1. UART1 ring buffer feed --- */
    while (R8_UART1_LSR & RB_LSR_DATA_RDY) {
        uint8_t ch = R8_UART1_RBR;
        uint16_t next = (at_rx_head + 1) % AT_RX_BUF_SIZE;
        if (next != at_rx_tail) {
            at_rx_buf[at_rx_head] = ch;
            at_rx_head = next;
        }
    }

    /* --- 2. Process UART ring buffer --- */
    while (at_rx_tail != at_rx_head) {
        uint8_t ch = at_rx_buf[at_rx_tail];
        at_rx_tail = (at_rx_tail + 1) % AT_RX_BUF_SIZE;
        if (ch == '\n') {
            at_channel = AT_CH_UART;
            at_line[at_line_len] = '\0';
            at_line_len = 0;
            at_process_line();
        } else {
            if (at_line_len < AT_LINE_MAX - 1)
                at_line[at_line_len++] = (char)ch;
        }
    }

    /* --- 3. Process CDC data (line-buffered echo) --- */
    uint8_t cdc_buf[64];
    uint16_t cdc_n = USB_CDC_Read(cdc_buf, sizeof(cdc_buf));
    for (uint16_t i = 0; i < cdc_n; i++) {
        if (cdc_buf[i] == '\n') {
            at_channel = AT_CH_CDC;
            at_line[at_line_len] = '\0';
            at_line_len = 0;
            /* echo line + newline */
            USB_CDC_Write((const uint8_t *)at_line, strlen(at_line));
            USB_CDC_Write((const uint8_t *)"\r\n", 2);
            at_process_line();
        } else if (cdc_buf[i] != '\r') {
            if (at_line_len < AT_LINE_MAX - 1)
                at_line[at_line_len++] = (char)cdc_buf[i];
        }
    }
}
/******************************** endfile @ AT.c ******************************/
