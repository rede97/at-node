/********************************** (C) COPYRIGHT *******************************
 * File Name          : at_cmds.c
 * Author             : at-node
 * Description        : AT command handlers and command table
 ********************************************************************************/

#include "config.h"
#include "hws.h"
#include "hidkbd_common.h"
#include "usb_dev.h"
#include "at_parser.h"
#include <stdlib.h>

/* ===== Keyboard router ===== */
static kb_mode_t kb_mode = KB_BOTH;

void kb_set_mode(kb_mode_t m) { kb_mode = m; }
kb_mode_t kb_get_mode(void)   { return kb_mode; }

extern void kb_ble_send_report(uint8_t mods, uint8_t *keys, int count);
extern void kb_usb_send_report(uint8_t mods, uint8_t *keys, int count);

/* Dispatch one HID report to active output(s).
   keys[] is always 6 bytes (zero-padded); count = significant keys.
   Returns 0 if sent on at least one channel, -1 if no output active. */
static int kb_flush(uint8_t mods, uint8_t *keys, int count)
{
    int sent = 0;
    if (kb_mode & KB_BLE && kb_ble_connected()) {
        PRINT("KB> BLE:%02X%02X%02X%02X%02X%02X mods=%02X\n",
              keys[0],keys[1],keys[2],keys[3],keys[4],keys[5], mods);
        kb_ble_send_report(mods, keys, count);
        sent = 1;
    }
    if (kb_mode & KB_USB && usb_ready()) {
        PRINT("KB> USB:%02X%02X%02X%02X%02X%02X mods=%02X\n",
              keys[0],keys[1],keys[2],keys[3],keys[4],keys[5], mods);
        kb_usb_send_report(mods, keys, count);
        sent = 1;
    }
    return sent ? 0 : -1;
}

int kb_press(uint8_t keycode)
{
    uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
    return kb_flush(0, keys, 1);
}

int kb_release(void)
{
    uint8_t zero[6] = { 0 };
    return kb_flush(0, zero, 0);
}

int kb_set_mods(uint8_t mods)
{
    uint8_t zero[6] = { 0 };
    return kb_flush(mods, zero, 0);
}

/* ===== KEY_SEQ playback — TMOS timer driven =====
 *
 *   Parsed reports are queued in seq_buf and played back one per TMOS
 *   timer event, so long sequences never block the cooperative scheduler
 *   (a busy-wait would starve BLE/USB tasks and could drop BLE packets).
 *
 *   SEQ_MAX_REPORTS=8 matches AT_LINE_MAX=256: 8 reports x 7 values at
 *   up to 3 digits each + separators fit one command line. Longer text
 *   must be split into multiple AT+KEY_SEQ commands by the host script.
 *
 *   Re-issuing AT+KEY_SEQ during playback replaces the running queue.
 */
#define SEQ_EVENT         0x0001
#define SEQ_MAX_REPORTS   8

static tmosTaskID seq_task_id  = INVALID_TASK_ID;
static uint8_t    seq_buf[SEQ_MAX_REPORTS][7];   /* mods + 6 keys per report */
static int        seq_count    = 0;
static int        seq_idx      = 0;
static uint16_t   seq_delay_ms = 0;

static tmosEvents kb_seq_process_event(tmosTaskID tid, tmosEvents evt)
{
    (void)tid;
    if (evt & SEQ_EVENT) {
        if (seq_idx < seq_count) {
            kb_flush(seq_buf[seq_idx][0], &seq_buf[seq_idx][1], 6);
            seq_idx++;
            if (seq_idx < seq_count)
                tmos_start_task(seq_task_id, SEQ_EVENT, MS1_TO_SYSTEM_TIME(seq_delay_ms));
        }
        return evt ^ SEQ_EVENT;
    }
    return 0;
}

/* ===== AT command handlers — implemented ===== */
static int at_cmd_AT(int argc, char *argv[])    { (void)argc; (void)argv; return 0; }
/* Role tag for firmware identification — two identical boards are easy
   to mix up; AT+VER tells which role is flashed (compile-time). */
#if(defined(BLE_DONGLE)) && (BLE_DONGLE == TRUE)
#define AT_ROLE_TAG  "dongle"
#else
#define AT_ROLE_TAG  "kbd"
#endif

static int at_cmd_VER(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("AT-Node v1.0 [%s] BLE: %s", AT_ROLE_TAG, VER_LIB); return 0; }
static int at_cmd_HELP(int argc, char *argv[])  {
    (void)argc; (void)argv;
    AT_Response("AT-Node Commands:\r\n"
        "  [Core]\r\n"
        "  AT       - handshake\r\n"
        "  AT+VER   - version\r\n"
        "  AT+HELP  - this help\r\n"
        "  AT+ECHO  - echo <text>\r\n"
        "  AT+STATUS - device status [stub]\r\n"
        "  AT+RST   - software reset [stub]\r\n"
        "  [Keyboard]\r\n"
        "  AT+KB    - keyboard mode USB|BLE|BOTH\r\n"
        "  AT+KEY   - raw HID <mods>,<k1>,..,<k6>\r\n"
        "  AT+MOD   - modifiers <mask>\r\n"
        "  AT+KEY_SEQ  - batch HID <delay>,<mods>,<k1>..<k6>,...\r\n"
        "  [GPIO]\r\n"
        "  AT+GPIO_W   - write <pin>,<level> [stub]\r\n"
        "  AT+GPIO_R   - read <pin> [stub]\r\n"
        "  [Sensor]\r\n"
        "  AT+ADC      - read <ch> [stub]\r\n"
        "  AT+I2C_SCAN - scan bus [stub]\r\n"
        "  AT+I2C_R    - read <addr>,<reg>,<len> [stub]\r\n"
        "  AT+I2C_W    - write <addr>,<reg>,<data> [stub]\r\n"
        "  [Power]\r\n"
        "  AT+SLEEP    - sleep <mode> [stub]\r\n"
        "  [Wireless]\r\n"
        "  AT+BT_SCAN  - BLE scan [dongle]\r\n"
        "  AT+BT_DISC  - drop host link, re-advertise\r\n"
        "  AT+BT_PAIR  - drop link + erase bonds\r\n"
        "  [Infrared]\r\n"
        "  AT+IR=NEC   - send NEC <hex> [stub]\r\n"
        "  AT+IR=SIRC  - send SIRC <hex>,<bits> [stub]\r\n"
        "  AT+IR=RAW   - send raw <t1>,<t2>,... [stub]");
    return 0;
}
static int at_cmd_ECHO(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+ECHO=text"); return -1; }
    AT_Response("%s", argv[1]);
    return 0;
}
static int at_cmd_KB(int argc, char *argv[])  {
    if (argc < 2) {
        kb_mode_t m = kb_get_mode();
        AT_Response("mode=%s BLE=%s USB=%s",
            m==KB_USB?"USB":m==KB_BLE?"BLE":"BOTH",
            kb_ble_connected()?"connected":"disconnected",
            usb_ready()?"ready":"-");
        return 0;
    }
    if (argv[1][0]=='U' && argv[1][1]=='S' && argv[1][2]=='B' && !argv[1][3])  { kb_set_mode(KB_USB); AT_Response("KB=USB"); return 0; }
    if (argv[1][0]=='B' && argv[1][1]=='L' && argv[1][2]=='E' && !argv[1][3])  { kb_set_mode(KB_BLE); AT_Response("KB=BLE"); return 0; }
    if (argv[1][0]=='B' && argv[1][1]=='O' && argv[1][2]=='T' && argv[1][3]=='H' && !argv[1][4]) { kb_set_mode(KB_BOTH); AT_Response("KB=BOTH"); return 0; }
    AT_Response("usage: AT+KB[=USB|BLE|BOTH]"); return -1;
}
/* AT+KEY=<mods>,<k1>,..,<k6> — raw HID report. Missing args = 0. */
static int at_cmd_KEY(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+KEY=<mods>,<k1>,..,<k6>"); return -1; }
    uint8_t mods   = (uint8_t)atoi(argv[1]);
    uint8_t keys[6] = { 0 };
    int     count  = 0;
    for (int i = 2; i <= 7; i++) {
        uint8_t kc = (i < argc) ? (uint8_t)atoi(argv[i]) : 0;
        if (kc) keys[count++] = kc;
    }
    if (kb_flush(mods, keys, count) < 0) {
        AT_Response("ERROR: no active output — check AT+KB status");
        return -1;
    }
    return 0;
}
static int at_cmd_MOD(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+MOD=<mask>"); return -1; }
    if (kb_set_mods(atoi(argv[1])) < 0) {
        AT_Response("ERROR: no active output — check AT+KB status");
        return -1;
    }
    return 0;
}

/* AT+KEY_SEQ=<delay_ms>,<mods>,<k1>..<k6>,<mods>,<k1>..<k6>,...
 * Batch HID report playback. Each report = exactly 7 values (1 mods + 6 keys),
 * all values DECIMAL. Reports are queued and played back by a TMOS timer
 * at delay_ms pacing — the AT task stays responsive during playback.
 * Script pre-translates text → HID report sequence.
 *
 * Example (Shift+h, release, e, release):
 *   AT+KEY_SEQ=5,2,11,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0
 *   → 4 reports at 5ms intervals: Shift+h down, all up, e down, all up
 */
static int at_cmd_KEY_SEQ(int argc, char *argv[])
{
    int nvals = argc - 2;   /* values after delay */
    if (nvals < 7 || nvals % 7) {
        AT_Response("usage: AT+KEY_SEQ=<delay_ms>,<mods>,<k1>..<k6>,...");
        return -1;
    }
    int n = nvals / 7;
    if (n > SEQ_MAX_REPORTS) {
        AT_Response("ERROR: max %d reports per command", SEQ_MAX_REPORTS);
        return -1;
    }

    int delay_ms = atoi(argv[1]);
    if (delay_ms < 1)   delay_ms = 1;
    if (delay_ms > 200) delay_ms = 200;  /* safety cap */

    for (int r = 0; r < n; r++)
        for (int i = 0; i < 7; i++)
            seq_buf[r][i] = (uint8_t)atoi(argv[2 + r * 7 + i]);

    /* Lazy TMOS task registration (TMOS running since ble_stack_init) */
    if (seq_task_id == INVALID_TASK_ID)
        seq_task_id = TMOS_ProcessEventRegister(kb_seq_process_event);

    seq_count    = n;
    seq_idx      = 0;
    seq_delay_ms = (uint16_t)delay_ms;
    tmos_start_task(seq_task_id, SEQ_EVENT, 0);  /* first report next tick */

    AT_Response("%d reports queued", n);
    return 0;
}

/* ===== Stub commands — registered for protocol compatibility, TODO implement ===== */

/* Core */
static int at_cmd_STATUS(int argc, char *argv[])  { (void)argc; (void)argv; return 0; }
static int at_cmd_RST(int argc, char *argv[])     { (void)argc; (void)argv; return 0; }  /* TODO: SYS_ResetExecute() */

/* Keyboard */
/* GPIO */
static int at_cmd_GPIO_W(int argc, char *argv[])  { (void)argc; (void)argv; return 0; }
static int at_cmd_GPIO_R(int argc, char *argv[])  { (void)argc; (void)argv; return 0; }

/* Sensor */
static int at_cmd_ADC(int argc, char *argv[])     { (void)argc; (void)argv; return 0; }
static int at_cmd_I2C_SCAN(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
static int at_cmd_I2C_R(int argc, char *argv[])   { (void)argc; (void)argv; return 0; }
static int at_cmd_I2C_W(int argc, char *argv[])   { (void)argc; (void)argv; return 0; }

/* Power */
static int at_cmd_SLEEP(int argc, char *argv[])   { (void)argc; (void)argv; return 0; }

/* Wireless */
static int at_cmd_BT_SCAN(int argc, char *argv[]) { (void)argc; (void)argv; AT_Response("ERROR: scan needs dongle mode (BLE_DONGLE)"); return -1; }
/* AT+BT_DISC — actively drop the host link. Bond is kept and
   advertising restarts automatically, like a real keyboard's
   host-switch key: the same or a new host can reconnect. */
static int at_cmd_BT_DISC(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (kb_ble_disconnect() < 0) {
        AT_Response("ERROR: not connected");
        return -1;
    }
    return 0;
}
/* AT+BT_PAIR — drop the link AND erase all bonds: back to a clean
   pairing mode, like long-pressing a real keyboard's pairing key. */
static int at_cmd_BT_PAIR(int argc, char *argv[]) {
    (void)argc; (void)argv;
    kb_ble_disconnect();      /* fine if not connected */
    kb_ble_forget_bonds();
    AT_Response("bonds erased — pairing mode");
    return 0;
}

/* Infrared */
static int at_cmd_IR_NEC(int argc, char *argv[])  { (void)argc; (void)argv; return 0; }
static int at_cmd_IR_SIRC(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
static int at_cmd_IR_RAW(int argc, char *argv[])  { (void)argc; (void)argv; return 0; }

/* ===== Command table =====
 *
 *   Organized by function group. Entries marked [stub] return OK but
 *   perform no action — they reserve the command name for future
 *   implementation without breaking host scripts that issue them.
 *
 *   Removed from requirements:
 *     AT+COMB     — redundant, merged into AT+KEY (<mods>,<k1>,..,<k6>)
 *     AT+KEY_DOWN — redundant, hold semantics via repeated AT+KEY reports
 *     AT+KEY_UP   — redundant, release = AT+KEY with zeroed report
 *     AT+WOL      — CH582F has no Ethernet MAC, not feasible
 *     AT+ISP      — not supported (no IAP bootloader planned)
 */
const at_cmd_t cmd_table[] = {
    /* Core */
    { "AT",         "handshake -> OK",                at_cmd_AT },
    { "AT+VER",     "firmware version",               at_cmd_VER },
    { "AT+HELP",    "command list",                   at_cmd_HELP },
    { "AT+ECHO",    "echo <text>",                    at_cmd_ECHO },
    { "AT+STATUS",  "[stub] device status",           at_cmd_STATUS },
    { "AT+RST",     "[stub] software reset",          at_cmd_RST },
    /* Keyboard */
    { "AT+KB",      "keyboard mode USB|BLE|BOTH",     at_cmd_KB },
    { "AT+KEY",     "raw HID report <mods>,<k1>,..,<k6>", at_cmd_KEY },
    { "AT+MOD",     "set modifiers <mask>",           at_cmd_MOD },
    { "AT+KEY_SEQ", "batch HID <delay>,<mods>,<k1>..<k6>,...", at_cmd_KEY_SEQ },
    /* GPIO */
    { "AT+GPIO_W",  "[stub] write <pin>,<level>",    at_cmd_GPIO_W },
    { "AT+GPIO_R",  "[stub] read <pin>",             at_cmd_GPIO_R },
    /* Sensor */
    { "AT+ADC",     "[stub] read ADC <ch>",          at_cmd_ADC },
    { "AT+I2C_SCAN","[stub] scan I2C bus",           at_cmd_I2C_SCAN },
    { "AT+I2C_R",   "[stub] I2C read <addr>,<reg>,<len>", at_cmd_I2C_R },
    { "AT+I2C_W",   "[stub] I2C write <addr>,<reg>,<data>", at_cmd_I2C_W },
    /* Power */
    { "AT+SLEEP",   "[stub] sleep <mode>",           at_cmd_SLEEP },
    /* Wireless */
    { "AT+BT_SCAN", "[dongle] BLE scan",             at_cmd_BT_SCAN },
    { "AT+BT_DISC", "drop host link, re-advertise",  at_cmd_BT_DISC },
    { "AT+BT_PAIR", "drop link + erase bonds",       at_cmd_BT_PAIR },
    /* Infrared */
    { "AT+IR",      "[stub] IR=NEC|SIRC|RAW,...",    at_cmd_IR_NEC },  /* sub-cmd parsed as arg1 */
};
const int cmd_table_count = sizeof(cmd_table) / sizeof(cmd_table[0]);
