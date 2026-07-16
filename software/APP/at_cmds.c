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

extern uint8_t Ready;

/* ===== Keyboard router ===== */
static kb_mode_t kb_mode = KB_BOTH;
static uint8_t  kbd_mods = 0;
static uint8_t  kbd_keys[6];
static int      kbd_count = 0;

void kb_set_mode(kb_mode_t m) { kb_mode = m; }
kb_mode_t kb_get_mode(void)   { return kb_mode; }

extern void kb_ble_send_report(uint8_t mods, uint8_t *keys, int count);
extern void kb_usb_send_report(uint8_t mods, uint8_t *keys, int count);

static int kb_flush(void)
{
    int sent = 0;
    if (kb_mode & KB_BLE && kb_ble_connected()) {
        PRINT("KB> BLE:%02X%02X%02X%02X%02X%02X mods=%02X\n",
              kbd_keys[0],kbd_keys[1],kbd_keys[2],kbd_keys[3],kbd_keys[4],kbd_keys[5], kbd_mods);
        kb_ble_send_report(kbd_mods, kbd_keys, kbd_count);
        sent = 1;
    }
    if (kb_mode & KB_USB && Ready) {
        PRINT("KB> USB:%02X%02X%02X%02X%02X%02X mods=%02X\n",
              kbd_keys[0],kbd_keys[1],kbd_keys[2],kbd_keys[3],kbd_keys[4],kbd_keys[5], kbd_mods);
        kb_usb_send_report(kbd_mods, kbd_keys, kbd_count);
        sent = 1;
    }
    return sent ? 0 : -1;
}

int kb_press_and_release(uint8_t keycode)
{
    kbd_keys[0] = keycode;
    for (int i = 1; i < 6; i++) kbd_keys[i] = 0;
    kbd_count = 1;
    int r = kb_flush();
    kbd_count = 0;
    int r2 = kb_flush();
    return (r < 0 && r2 < 0) ? -1 : 0;
}

int kb_key_down(uint8_t keycode)
{
    if (kbd_count >= 6) return -1;
    kbd_keys[kbd_count++] = keycode;
    return kb_flush();
}

int kb_key_up(uint8_t keycode)
{
    for (int i = 0; i < kbd_count; i++) {
        if (kbd_keys[i] == keycode) {
            kbd_keys[i] = kbd_keys[--kbd_count];
            break;
        }
    }
    return kb_flush();
}

int kb_set_mods(uint8_t mods) { kbd_mods = mods; return kb_flush(); }
int kb_release_all(void)      { kbd_count = 0; kbd_mods = 0; return kb_flush(); }

/* ===== AT command handlers — implemented ===== */
static int at_cmd_AT(int argc, char *argv[])    { (void)argc; (void)argv; return 0; }
static int at_cmd_VER(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("AT-Node v1.0 BLE: %s", VER_LIB); return 0; }
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
        "  AT+KEY_DOWN - hold key <kc>\r\n"
        "  AT+KEY_UP   - release key <kc>\r\n"
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
        "  AT+BT_SCAN  - BLE scan [stub]\r\n"
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
            Ready?"ready":"-");
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
    kbd_mods  = (argc > 1) ? atoi(argv[1]) : 0;
    kbd_count = 0;
    for (int i = 2; i <= 7; i++) {
        uint8_t kc = (argc > i) ? atoi(argv[i]) : 0;
        if (kc) kbd_keys[kbd_count++] = kc;
    }
    for (int i = kbd_count; i < 6; i++) kbd_keys[i] = 0;
    if (kb_flush() < 0) {
        AT_Response("ERROR: no active output — check AT+KB status");
        return -1;
    }
    return 0;
}
static int at_cmd_KEY_DOWN(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+KEY_DOWN=<kc>"); return -1; }
    if (kb_key_down(atoi(argv[1])) < 0) {
        AT_Response("ERROR: no active output — check AT+KB status");
        return -1;
    }
    return 0;
}
static int at_cmd_KEY_UP(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+KEY_UP=<kc>"); return -1; }
    if (kb_key_up(atoi(argv[1])) < 0) {
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
 * Batch HID report playback. Each report = exactly 7 values (1 mods + 6 keys).
 * Script pre-translates text → HID report sequence, firmware plays back with
 * delay_ms pacing between reports. Much faster than per-report AT commands.
 *
 * Example (Shift+h, release, e, release):
 *   AT+KEY_SEQ=5,2,0B,0,0,0,0,0,0,0,0,0,0,0,0,0,08,0,0,0,0,0,0,0,0,0,0,0,0
 *   → 4 reports at 5ms intervals: Shift+h down, all up, e down, all up
 */
static int at_cmd_KEY_SEQ(int argc, char *argv[])
{
    if (argc < 9) {  /* delay + at least 1 report (7 values) */
        AT_Response("usage: AT+KEY_SEQ=<delay_ms>,<mods>,<k1>..<k6>,...");
        return -1;
    }
    int delay_ms = atoi(argv[1]);
    if (delay_ms < 1)  delay_ms = 1;
    if (delay_ms > 200) delay_ms = 200;  /* safety cap */

    int sent = 0;
    int arg_idx = 2;
    while (arg_idx + 6 < argc) {  /* need 7 more values for a full report */
        kbd_mods = atoi(argv[arg_idx++]);
        kbd_count = 0;
        for (int i = 0; i < 6; i++) {
            uint8_t kc = (uint8_t)atoi(argv[arg_idx++]);
            if (kc) kbd_keys[kbd_count++] = kc;
        }
        for (int i = kbd_count; i < 6; i++) kbd_keys[i] = 0;
        if (kb_flush() == 0) sent++;

        /* Busy-wait between reports. TMOS is cooperative — this blocks
           other tasks briefly, but for batch HID reports (typically <50ms
           total) it's faster and simpler than TMOS timer chaining. */
        if (arg_idx + 6 < argc) {
            for (volatile uint32_t d = 0; d < (uint32_t)delay_ms * 6000; d++) {
                __asm volatile ("nop");
            }
        }
    }
    AT_Response("%d reports sent", sent);
    return sent > 0 ? 0 : -1;
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
static int at_cmd_BT_SCAN(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }

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
 *     AT+COMB — redundant, merged into AT+KEY (<mods>,<k1>,..,<k6>)
 *     AT+WOL  — CH582F has no Ethernet MAC, not feasible
 *     AT+ISP  — not supported (no IAP bootloader planned)
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
    { "AT+KEY_DOWN","hold key <kc>",                  at_cmd_KEY_DOWN },
    { "AT+KEY_UP",  "release key <kc>",               at_cmd_KEY_UP },
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
    { "AT+BT_SCAN", "[stub] BLE scan",               at_cmd_BT_SCAN },
    /* Infrared */
    { "AT+IR",      "[stub] IR=NEC|SIRC|RAW,...",    at_cmd_IR_NEC },  /* sub-cmd parsed as arg1 */
};
const int cmd_table_count = sizeof(cmd_table) / sizeof(cmd_table[0]);
