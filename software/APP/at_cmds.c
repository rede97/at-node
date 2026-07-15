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

static void kb_flush(void)
{
    PRINT("KB: flush mods=%02X count=%d keys=%02X%02X%02X%02X%02X%02X\n",
          kbd_mods, kbd_count, kbd_keys[0],kbd_keys[1],kbd_keys[2],kbd_keys[3],kbd_keys[4],kbd_keys[5]);
    if (kb_mode & KB_BLE) kb_ble_send_report(kbd_mods, kbd_keys, kbd_count);
    if (kb_mode & KB_USB) kb_usb_send_report(kbd_mods, kbd_keys, kbd_count);
}

void kb_press_and_release(uint8_t keycode)
{
    PRINT("KB: press+release %02X mode=%d\n", keycode, kb_mode);
    kbd_keys[0] = keycode;
    for (int i = 1; i < 6; i++) kbd_keys[i] = 0;
    kbd_count = 1;
    kb_flush();
    kbd_count = 0;
    kb_flush();
}

void kb_key_down(uint8_t keycode)
{
    if (kbd_count >= 6) return;
    kbd_keys[kbd_count++] = keycode;
    kb_flush();
}

void kb_key_up(uint8_t keycode)
{
    for (int i = 0; i < kbd_count; i++) {
        if (kbd_keys[i] == keycode) {
            kbd_keys[i] = kbd_keys[--kbd_count];
            break;
        }
    }
    kb_flush();
}

void kb_set_mods(uint8_t mods) { kbd_mods = mods; kb_flush(); }
void kb_release_all(void) { kbd_count = 0; kbd_mods = 0; kb_flush(); }

/* ===== AT command handlers ===== */
static int at_cmd_AT(int argc, char *argv[])    { (void)argc; (void)argv; return 0; }
static int at_cmd_VER(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("AT-Node v1.0 BLE: %s", VER_LIB); return 0; }
static int at_cmd_HELP(int argc, char *argv[])  {
    (void)argc; (void)argv;
    AT_Response("AT-Node Commands:\r\n  AT       - handshake\r\n  AT+VER   - version\r\n  AT+HELP  - this help\r\n  AT+KB    - keyboard USB/BLE/BOTH\r\n  AT+KEY   - press+release <kc>\r\n  AT+KEY_DOWN - hold key <kc>\r\n  AT+KEY_UP - release key <kc>\r\n  AT+MOD   - set modifiers <mask>\r\n  AT+ECHO  - echo <text>");
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
static int at_cmd_KEY(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+KEY=<kc>"); return -1; }
    kb_press_and_release(atoi(argv[1]));
    return 0;
}
static int at_cmd_KEY_DOWN(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+KEY_DOWN=<kc>"); return -1; }
    kb_key_down(atoi(argv[1]));
    return 0;
}
static int at_cmd_KEY_UP(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+KEY_UP=<kc>"); return -1; }
    kb_key_up(atoi(argv[1]));
    return 0;
}
static int at_cmd_MOD(int argc, char *argv[])  {
    if (argc < 2) { AT_Response("usage: AT+MOD=<mask>"); return -1; }
    kb_set_mods(atoi(argv[1]));
    return 0;
}

const at_cmd_t cmd_table[] = {
    { "AT",         "handshake -> OK",                at_cmd_AT },
    { "AT+VER",     "firmware version",               at_cmd_VER },
    { "AT+HELP",    "command list",                   at_cmd_HELP },
    { "AT+KB",      "keyboard mode USB|BLE|BOTH",     at_cmd_KB },
    { "AT+KEY",     "press+release key <kc>",         at_cmd_KEY },
    { "AT+KEY_DOWN","hold key <kc>",                  at_cmd_KEY_DOWN },
    { "AT+KEY_UP",  "release key <kc>",               at_cmd_KEY_UP },
    { "AT+MOD",     "set modifiers <mask> (1=Ctrl 2=Shift 4=Alt 8=Win)", at_cmd_MOD },
    { "AT+ECHO",    "echo <text>",                    at_cmd_ECHO },
};
const int cmd_table_count = sizeof(cmd_table) / sizeof(cmd_table[0]);
