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
#include "ble_dongle.h"
#include "role.h"
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

/* KEY_STR state — full definitions live after at_cmd_KEY_SEQ */
static char    keystr_buf[];
static int     keystr_len, keystr_idx;
static uint8_t keystr_active;
static int keystr_map(char c, uint8_t *mods, uint8_t *key);

static tmosEvents kb_seq_process_event(tmosTaskID tid, tmosEvents evt)
{
    (void)tid;
    if (evt & SEQ_EVENT) {
        /* KEY_STR playback interleaves press/release: even idx = press
           char[idx/2], odd idx = release. */
        if (keystr_active) {
            int ci = keystr_idx / 2;
            if (ci >= keystr_len) {
                keystr_active = 0;
                return evt ^ SEQ_EVENT;
            }
            if (keystr_idx & 1) {
                uint8_t zero[6] = { 0 };
                kb_flush(0, zero, 0);
            } else {
                uint8_t mods, key, keys[6] = { 0 };
                if (keystr_map(keystr_buf[ci], &mods, &key)) {
                    keys[0] = key;
                    kb_flush(mods, keys, 1);
                }
            }
            keystr_idx++;
            tmos_start_task(seq_task_id, SEQ_EVENT, MS1_TO_SYSTEM_TIME(15));
            return evt ^ SEQ_EVENT;
        }
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
   to mix up; AT+VER tells which role is flashed (DUAL builds report
   the RUNTIME role from the DataFlash flag). */
static int at_cmd_VER(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("AT-Node v1.0 [%s] BLE: %s", role_name(role_current()), VER_LIB); return 0; }
static int at_cmd_HELP(int argc, char *argv[])  {
    (void)argc; (void)argv;
    /* AT+HELP=<CMD> — single-command help (F3.10) */
    if (argc > 1) {
        extern const at_cmd_t cmd_table[];
        extern const int cmd_table_count;
        for (int i = 0; i < cmd_table_count; i++) {
            if (strcasecmp(argv[1], cmd_table[i].name) == 0) {
                AT_Response("%s - %s", cmd_table[i].name, cmd_table[i].help);
                return 0;
            }
        }
        AT_Response("ERROR: unknown command (try AT+HELP)");
        return -1;
    }
    /* Chunked: AT_Response buffer is 256 B — one giant call would be
       truncated (and used to leak stack garbage before the clamp fix) */
    AT_Response("AT-Node Commands:\r\n"
        "  [Core]\r\n"
        "  AT       - handshake\r\n"
        "  AT+VER   - version\r\n"
        "  AT+HELP  - this help\r\n"
        "  AT+ECHO  - echo <text>\r\n"
        "  AT+STATUS - role/kb/ble/batt\r\n"
        "  AT+RST   - software reset\r\n"
        "  AT+ISP   - enter ISP bootloader (erases app!)");
    AT_Response(
        "  AT+ROLE  - role KBD|DONGLE (DUAL)\r\n"
        "  [Keyboard]\r\n"
        "  AT+KB    - keyboard mode USB|BLE|BOTH\r\n"
        "  AT+KEY   - raw HID <mods>,<k1>,..,<k6>\r\n"
        "  AT+TAP   - press+release <ms>,<mods>,<k1>..<k6>\r\n"
        "  AT+MOD   - modifiers <mask>\r\n"
        "  AT+KEY_SEQ  - batch HID <delay>,<mods>,<k1>..<k6>,...");
    AT_Response(
        "  [GPIO]\r\n"
        "  AT+GPIO_W   - write <pin>,<level> (PA0-15,PB16-39)\r\n"
        "  AT+GPIO_R   - read <pin>\r\n"
        "  [Sensor]\r\n"
        "  AT+ADC      - read <ch 0-13> -> mV\r\n"
        "  AT+I2C_SCAN - scan bus");
    AT_Response(
        "  AT+I2C_R    - read <addr>,<reg>,<len> (hex)\r\n"
        "  AT+I2C_W    - write <addr>,<reg>,<data> (hex)\r\n"
        "  [Power]\r\n"
        "  AT+SLEEP    - sleep <mode> [stub]");
    AT_Response(
        "  [Wireless]\r\n"
        "  AT+BT_SCAN  - scan HID devices [sec] (dongle)\r\n"
        "  AT+BT_CONN  - connect <idx|addr|name> (dongle)\r\n"
        "  AT+BT_DISC  - drop BLE link, re-advertise\r\n"
        "  AT+BT_PAIR  - drop link + erase bonds");
    AT_Response(
        "  AT+BT_STATE - diag dongle state (dongle)\r\n"
        "  AT+BT_PASSKEY - SMP passkey <6digits> (dongle)\r\n"
        "  AT+BT_LIST  - bonded devices (dongle)\r\n"
        "  AT+BT_AUTO  - auto-reconnect [0|1] (dongle)");
    AT_Response(
        "  [Infrared]\r\n"
        "  AT+IR=NEC   - send NEC <hex>\r\n"
        "  AT+IR=SIRC  - send SIRC <hex>,<bits>\r\n"
        "  AT+IR=RAW   - send raw <t1>,<t2>,...");
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
/* AT+TAP=<ms>,<mods>,<k1>,..,<k6> — press-and-release in ONE command:
   the report is queued followed by an all-zero release after <ms> on
   the KEY_SEQ timer (non-blocking). This is the everyday "type a key"
   command; AT+KEY (manual press / manual release) is for special cases
   like held modifiers. ms: 0 = 50 default, clamped 10..1000. */
static int at_cmd_TAP(int argc, char *argv[])
{
    if (argc < 3) { AT_Response("usage: AT+TAP=<ms>,<mods>,<k1>,..,<k6>"); return -1; }
    int hold_ms = atoi(argv[1]);
    if (hold_ms == 0)   hold_ms = 50;
    if (hold_ms < 10)   hold_ms = 10;
    if (hold_ms > 1000) hold_ms = 1000;

    for (int i = 0; i < 7; i++) { seq_buf[0][i] = 0; seq_buf[1][i] = 0; }
    for (int i = 2; i < argc && i <= 8; i++)
        seq_buf[0][i - 2] = (uint8_t)atoi(argv[i]);
    /* seq_buf[1] stays all-zero = release report */

    if (seq_task_id == INVALID_TASK_ID)
        seq_task_id = TMOS_ProcessEventRegister(kb_seq_process_event);
    seq_count    = 2;
    seq_idx      = 0;
    seq_delay_ms = (uint16_t)hold_ms;
    tmos_start_task(seq_task_id, SEQ_EVENT, 0);  /* press next tick */
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

/* ===== KEY_STR playback — ASCII text via the SEQ timer =====
 *
 *   AT+KEY_STR=<text> types a string (US layout). Text is copied into
 *   keystr_buf and played one char per SEQ_EVENT tick (~15 ms/char,
 *   non-blocking). Re-issuing replaces the running string.
 *   Limitations (tokenizer): no ',' or '=' inside the text.
 */
#define KEYSTR_MAX  96
static char    keystr_buf[KEYSTR_MAX + 1];
static int     keystr_len, keystr_idx;
static uint8_t keystr_active;

/* US-layout char -> mods(2=Shift), HID key. Returns 1 if mapped. */
static int keystr_map(char c, uint8_t *mods, uint8_t *key)
{
    *mods = 0;
    if (c >= 'a' && c <= 'z') { *key = (uint8_t)(c - 'a') + 4;  return 1; }
    if (c >= 'A' && c <= 'Z') { *mods = 2; *key = (uint8_t)(c - 'A') + 4; return 1; }
    if (c >= '1' && c <= '9') { *key = (uint8_t)(c - '1') + 30; return 1; }
    if (c == '0') { *key = 39; return 1; }
    switch (c) {
        case ' ':  *key = 44; return 1;
        case '\t': *key = 43; return 1;
        case '-':  *key = 45; return 1;
        case '=':  *key = 46; return 1;
        case '[':  *key = 47; return 1;
        case ']':  *key = 48; return 1;
        case '\\': *key = 49; return 1;
        case ';':  *key = 51; return 1;
        case '\'': *key = 52; return 1;
        case '`':  *key = 53; return 1;
        case ',':  *key = 54; return 1;
        case '.':  *key = 55; return 1;
        case '/':  *key = 56; return 1;
    }
    {   /* shifted digit row: !@<#>$%^&*() -> Shift+1..0 */
        static const char sym[] = "!@<#>$%^&*()";
        for (int i = 0; sym[i]; i++)
            if (c == sym[i]) { *mods = 2; *key = (i == 9) ? 39 : (uint8_t)(30 + i); return 1; }
    }
    *mods = 2;
    switch (c) {
        case '_': *key = 45; return 1;
        case '+': *key = 46; return 1;
        case '{': *key = 47; return 1;
        case '}': *key = 48; return 1;
        case '|': *key = 49; return 1;
        case ':': *key = 51; return 1;
        case '"': *key = 52; return 1;
        case '~': *key = 53; return 1;
        case '<': *key = 54; return 1;
        case '>': *key = 55; return 1;
        case '?': *key = 56; return 1;
    }
    return 0;
}

static int at_cmd_KEY_STR(int argc, char *argv[])
{
    if (argc < 2) { AT_Response("usage: AT+KEY_STR=<text> (no ',' or '=')"); return -1; }
    int n = 0;
    while (argv[1][n] && n < KEYSTR_MAX) { keystr_buf[n] = argv[1][n]; n++; }
    keystr_buf[n] = '\0';
    keystr_len  = n;
    keystr_idx  = 0;
    if (seq_task_id == INVALID_TASK_ID)
        seq_task_id = TMOS_ProcessEventRegister(kb_seq_process_event);
    keystr_active = 1;
    tmos_start_task(seq_task_id, SEQ_EVENT, 0);
    if (argv[1][n]) AT_Response("truncated to %d chars", KEYSTR_MAX);
    return 0;
}


/* ===== Stub commands — registered for protocol compatibility, TODO implement ===== */

/* Core */
static int at_cmd_STATUS(int argc, char *argv[])  {
    (void)argc; (void)argv;
    const char *mode = (kb_get_mode() == KB_USB) ? "USB" :
                       (kb_get_mode() == KB_BLE) ? "BLE" : "BOTH";
    AT_Response("role=%s kb=%s ble=%s batt=%umV",
                role_name(role_current()), mode,
                kb_ble_connected() ? "connected" : "disconnected",
                hws_batt_read_mv());
    return 0;
}
/* AT+RST / AT+RESET — software reset. Reply first, then reset after the
   response has flushed over UART/CDC. */
static int at_cmd_RST(int argc, char *argv[])     {
    (void)argc; (void)argv;
    AT_Response("resetting...");
#ifdef DEBUG
    while ((R8_UART1_LSR & RB_LSR_TX_ALL_EMP) == 0) __nop();   /* flush UART1 */
#endif
    for (volatile uint32_t d = 0; d < 60000; d++) __nop();     /* ~5ms for CDC DMA */
    SYS_ResetExecute();
    return 0;   /* unreachable */
}

/* AT+ISP — erase app page 0 and reset into the ROM ISP bootloader.
   Chip re-enumerates as WCH ISP (VID:PID 1a86:8010) for wchisp flashing —
   no BOOT button or WCH-Link needed.

   With page 0 erased the chip looks blank, so EVERY reset re-enters ISP
   (10 s window each boot) until reflashed. Recovery if flashing fails:
   power-cycle re-enters ISP; wlink (debug wire) works regardless of
   flash contents. */

/* app_jump_boot — must run from RAM (__HIGH_CODE): page 0 is erased
   under our feet, executing this from flash would crash mid-erase.
   Sequence from the WCH EVT IAP example. After the erase begins, only
   ROM routines (FLASH_ROM_*) and inline SFR writes execute — no
   flash-resident code may be called. */
__HIGH_CODE
static void app_jump_boot(void)
{
    uint32_t irqv;
    SYS_DisableAllIrq(&irqv);     /* IRQ handlers live in the flash being erased */
    while (FLASH_ROM_ERASE(0, EEPROM_BLOCK_SIZE)) {
        ;                         /* ROM erases in 4K units from address 0 */
    }
    FLASH_ROM_SW_RESET();
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG1;
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG2;
    SAFEOPERATE;
    R16_INT32K_TUNE = 0xFFFF;
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET;
    R8_SAFE_ACCESS_SIG = 0;       /* power-on-type reset: boot ROM sees blank app */
    while (1) ;                   /* never reached — reset fires first */
}

static int at_cmd_ISP(int argc, char *argv[])     {
    (void)argc; (void)argv;
    AT_Response("entering ISP — app flash erased, reflash via wchisp (1a86:8010)");
#ifdef DEBUG
    while ((R8_UART1_LSR & RB_LSR_TX_ALL_EMP) == 0) __nop();   /* flush UART1 */
#endif
    for (volatile uint32_t d = 0; d < 60000; d++) __nop();     /* ~5ms for CDC DMA */
    app_jump_boot();
    return 0;   /* unreachable */
}

/* Keyboard */
/* GPIO */
#if(defined(HWS_GPIO)) && (HWS_GPIO == TRUE)
static int at_cmd_GPIO_W(int argc, char *argv[]) {
    if (argc < 3) { AT_Response("usage: AT+GPIO_W=<pin>,<level>"); return -1; }
    if (hws_gpio_write((uint8_t)atoi(argv[1]), (uint8_t)atoi(argv[2])) < 0) {
        AT_Response("ERROR: bad pin (0-15=PA, 16-39=PB)");
        return -1;
    }
    return 0;
}
static int at_cmd_GPIO_R(int argc, char *argv[]) {
    if (argc < 2) { AT_Response("usage: AT+GPIO_R=<pin>"); return -1; }
    int v = hws_gpio_read((uint8_t)atoi(argv[1]));
    if (v < 0) { AT_Response("ERROR: bad pin (0-15=PA, 16-39=PB)"); return -1; }
    AT_Response("%d", v);
    return 0;
}
#else
static int at_cmd_GPIO_W(int argc, char *argv[])  { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_GPIO=FALSE)"); return -1; }
static int at_cmd_GPIO_R(int argc, char *argv[])  { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_GPIO=FALSE)"); return -1; }
#endif

/* Sensor */
#if(defined(HWS_ADC)) && (HWS_ADC == TRUE)
static int at_cmd_ADC(int argc, char *argv[]) {
    if (argc < 2) { AT_Response("usage: AT+ADC=<ch 0-13>"); return -1; }
    uint16_t mv = hws_adc_read_mv((uint8_t)atoi(argv[1]));
    if (mv == 0xFFFF) { AT_Response("ERROR: bad channel (0-13)"); return -1; }
    AT_Response("%u mV", mv);
    return 0;
}
#else
static int at_cmd_ADC(int argc, char *argv[])     { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_ADC=FALSE)"); return -1; }
#endif
#if(defined(HWS_I2C)) && (HWS_I2C == TRUE)
static uint8_t i2c_inited = 0;
static void i2c_lazy_init(void) { if (!i2c_inited) { hws_i2c_init(); i2c_inited = 1; } }
static int at_cmd_I2C_SCAN(int argc, char *argv[]) {
    (void)argc; (void)argv;
    i2c_lazy_init();
    int found = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        if (hws_i2c_probe(a) == 0) {
            AT_Response("+I2C: 0x%02X", a);
            found++;
        }
    }
    if (!found) AT_Response("no devices");
    return 0;
}
static int at_cmd_I2C_R(int argc, char *argv[]) {
    if (argc < 4) { AT_Response("usage: AT+I2C_R=<addr>,<reg>,<len> (hex)"); return -1; }
    uint8_t addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtoul(argv[2], NULL, 0);
    int     len  = atoi(argv[3]);
    if (len < 1 || len > 32) { AT_Response("ERROR: len 1-32"); return -1; }
    uint8_t buf[32];
    i2c_lazy_init();
    if (hws_i2c_read(addr, reg, buf, (uint8_t)len) < 0) {
        AT_Response("ERROR: i2c read failed");
        return -1;
    }
    char hex[32 * 3 + 1];
    static const char nib[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        hex[i * 3]     = nib[buf[i] >> 4];
        hex[i * 3 + 1] = nib[buf[i] & 0xF];
        hex[i * 3 + 2] = (i + 1 < len) ? ' ' : '\0';
    }
    hex[len * 3] = '\0';
    AT_Response("%s", hex);
    return 0;
}
static int at_cmd_I2C_W(int argc, char *argv[]) {
    if (argc < 4) { AT_Response("usage: AT+I2C_W=<addr>,<reg>,<d0>[,<d1>,...] (hex)"); return -1; }
    uint8_t addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtoul(argv[2], NULL, 0);
    uint8_t data[16];
    int     n = argc - 3;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++)
        data[i] = (uint8_t)strtoul(argv[3 + i], NULL, 0);
    i2c_lazy_init();
    if (hws_i2c_write(addr, reg, data, (uint8_t)n) < 0) {
        AT_Response("ERROR: i2c write failed");
        return -1;
    }
    return 0;
}
#else
static int at_cmd_I2C_SCAN(int argc, char *argv[]) { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_I2C=FALSE)"); return -1; }
static int at_cmd_I2C_R(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_I2C=FALSE)"); return -1; }
static int at_cmd_I2C_W(int argc, char *argv[])   { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_I2C=FALSE)"); return -1; }
#endif

/* Power */
static int at_cmd_SLEEP(int argc, char *argv[])   { (void)argc; (void)argv; return 0; }

/* Wireless — role dispatch.
   Dongle-role bodies compile when BLE_HAS_DONGLE, kbd-role bodies when
   BLE_HAS_KBD. DUAL builds have both: commands check the RUNTIME role
   (role_current()) and dispatch or reject with a switch hint.
   Single-role builds fold to a constant branch. */

#if BLE_HAS_DONGLE
/* ---- dongle role: Central, connects TO a BLE keyboard ---- */
extern uint8_t ble_dongle_state_debug(void);   /* DIAG: current dgl_state */
extern uint8_t ble_dongle_start_status_debug(void);
/* AT+BT_SCAN[=<sec>[,<filter>]] — scan for HID-advertising devices.
   Results arrive asynchronously as +BT_SCAN lines over this channel,
   sorted strongest-first (idx 0 = nearest). <filter>: case-insensitive
   name substring, or "HID" for HID-flagged devices only. */
static int bt_scan_dgl(int argc, char *argv[]) {
    int sec = (argc > 1) ? atoi(argv[1]) : 5;
    if (sec < 1)  sec = 1;
    if (sec > 30) sec = 30;
    const char *filter = (argc > 2) ? argv[2] : NULL;
    if (ble_dongle_scan((uint8_t)sec, filter) < 0) {
        AT_Response("ERROR: busy state=%d", ble_dongle_state_debug());
        return -1;
    }
    AT_Response("scanning %ds%s%s...", sec,
                filter ? " filter=" : "", filter ? filter : "");
    return 0;
}
/* AT+BT_CONN=<idx|addr|name> — connect a scan result. Name is a
   case-insensitive substring (strongest RSSI wins); 12-hex is an
   address; small decimal is an index. Outcome is async:
   +BT_CONN: connected / armed (boot mode) / err ... */
static int bt_conn_dgl(int argc, char *argv[]) {
    if (argc < 2) { AT_Response("usage: AT+BT_CONN=<idx|addr|name>"); return -1; }
    int idx = ble_dongle_find(argv[1]);
    if (idx < 0 || ble_dongle_connect((uint8_t)idx) < 0) {
        AT_Response("ERROR: no match or busy — run AT+BT_SCAN first");
        return -1;
    }
    AT_Response("connecting #%d...", idx);
    return 0;
}
/* AT+BT_DISC (dongle) — drop the link to the keyboard (holds auto-
   reconnect once). */
static int bt_disc_dgl(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (ble_dongle_disconnect() < 0) {
        AT_Response("ERROR: not connected");
        return -1;
    }
    return 0;
}
/* AT+BT_PASSKEY=<6digits> — answer live SMP request, or preset the
   passkey used for the NEXT pairing attempt (default 123456) */
static int bt_passkey_dgl(int argc, char *argv[]) {
    if (argc < 2) { AT_Response("usage: AT+BT_PASSKEY=<6digits>"); return -1; }
    ble_dongle_passkey((uint32_t)atol(argv[1]));
    AT_Response("passkey set");
    return 0;
}
/* AT+BT_LIST — bonded keyboards stored in SNV */
static int bt_list_dgl(int argc, char *argv[]) {
    (void)argc; (void)argv;
    ble_dongle_list_bonds();
    return 0;
}
/* AT+BT_AUTO[=0|1] — auto-reconnect to bonded keyboards. No arg: query
   (auto=<0|1> state=<dgl_state>). Manual AT+BT_DISC holds it once. */
static int bt_auto_dgl(int argc, char *argv[]) {
    if (argc < 2) {
        AT_Response("auto=%d state=%d", ble_dongle_auto(-1), ble_dongle_state_debug());
        return 0;
    }
    AT_Response("auto=%d", ble_dongle_auto(atoi(argv[1]) != 0));
    return 0;
}
/* DIAG: AT+BT_STATE — dongle state, StartDevice status */
static int bt_state_dgl(int argc, char *argv[]) {
    (void)argc; (void)argv;
    AT_Response("state=%d startDev=%d (0=ok,24=badTask,0xFF=never)",
                ble_dongle_state_debug(), ble_dongle_start_status_debug());
    return 0;
}
/* AT+BT_PAIR (dongle) — drop the link AND erase the bonded keyboard.
   The dongle auto-reconnects to its single bond, so replacing the
   keyboard requires erasing the old bond first, then scan+conn the
   new one. */
static int bt_pair_dgl(int argc, char *argv[]) {
    (void)argc; (void)argv;
    ble_dongle_disconnect();      /* fine if idle; holds auto-reconnect */
    ble_dongle_forget_bonds();
    AT_Response("bonds erased — scan+conn to pair a new keyboard");
    return 0;
}
#endif /* BLE_HAS_DONGLE */

#if BLE_HAS_KBD
/* ---- kbd role: Peripheral, IS the keyboard ---- */
/* AT+BT_DISC (kbd) — actively drop the host link. Bond is kept and
   advertising restarts automatically, like a real keyboard's
   host-switch key: the same or a new host can reconnect. */
static int bt_disc_kbd(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (kb_ble_disconnect() < 0) {
        AT_Response("ERROR: not connected");
        return -1;
    }
    return 0;
}
/* AT+BT_PAIR — drop the link AND erase all bonds: back to a clean
   pairing mode, like long-pressing a real keyboard's pairing key. */
static int bt_pair_kbd(int argc, char *argv[]) {
    (void)argc; (void)argv;
    kb_ble_disconnect();      /* fine if not connected */
    kb_ble_forget_bonds();
    AT_Response("bonds erased — pairing mode");
    return 0;
}
#endif /* BLE_HAS_KBD */

/* Runtime-role guards — no-ops in single-role builds */
#if BLE_MODE == BLE_MODE_DUAL
#define ROLE_GUARD_DGL  if (role_current() != ROLE_DONGLE) { \
    AT_Response("ERROR: dongle-role command (AT+ROLE=DONGLE to switch)"); return -1; }
#define ROLE_GUARD_KBD  if (role_current() != ROLE_KBD) { \
    AT_Response("ERROR: kbd-role command (AT+ROLE=KBD to switch)"); return -1; }
#else
#define ROLE_GUARD_DGL
#define ROLE_GUARD_KBD
#endif

static int at_cmd_BT_SCAN(int argc, char *argv[]) {
#if BLE_HAS_DONGLE
    ROLE_GUARD_DGL;
    return bt_scan_dgl(argc, argv);
#else
    (void)argc; (void)argv;
    AT_Response("ERROR: scan needs dongle mode (BLE_DONGLE)"); return -1;
#endif
}
static int at_cmd_BT_CONN(int argc, char *argv[]) {
#if BLE_HAS_DONGLE
    ROLE_GUARD_DGL;
    return bt_conn_dgl(argc, argv);
#else
    (void)argc; (void)argv;
    AT_Response("ERROR: scan needs dongle mode (BLE_DONGLE)"); return -1;
#endif
}
static int at_cmd_BT_DISC(int argc, char *argv[]) {
#if BLE_MODE == BLE_MODE_DUAL
    return (role_current() == ROLE_DONGLE) ? bt_disc_dgl(argc, argv)
                                           : bt_disc_kbd(argc, argv);
#elif BLE_HAS_DONGLE
    return bt_disc_dgl(argc, argv);
#else
    return bt_disc_kbd(argc, argv);
#endif
}
static int at_cmd_BT_PASSKEY(int argc, char *argv[]) {
#if BLE_HAS_DONGLE
    ROLE_GUARD_DGL;
    return bt_passkey_dgl(argc, argv);
#else
    (void)argc; (void)argv;
    AT_Response("ERROR: dongle mode disabled (BLE_DONGLE=FALSE)"); return -1;
#endif
}
static int at_cmd_BT_LIST(int argc, char *argv[]) {
#if BLE_HAS_DONGLE
    ROLE_GUARD_DGL;
    return bt_list_dgl(argc, argv);
#else
    (void)argc; (void)argv;
    AT_Response("ERROR: dongle mode disabled (BLE_DONGLE=FALSE)"); return -1;
#endif
}
static int at_cmd_BT_AUTO(int argc, char *argv[]) {
#if BLE_HAS_DONGLE
    ROLE_GUARD_DGL;
    return bt_auto_dgl(argc, argv);
#else
    (void)argc; (void)argv;
    AT_Response("ERROR: dongle mode disabled (BLE_DONGLE=FALSE)"); return -1;
#endif
}
static int at_cmd_BT_STATE(int argc, char *argv[]) {
#if BLE_HAS_DONGLE
    ROLE_GUARD_DGL;
    return bt_state_dgl(argc, argv);
#else
    (void)argc; (void)argv;
    AT_Response("ERROR: dongle mode disabled (BLE_DONGLE=FALSE)"); return -1;
#endif
}
static int at_cmd_BT_PAIR(int argc, char *argv[]) {
#if BLE_MODE == BLE_MODE_DUAL
    return (role_current() == ROLE_DONGLE) ? bt_pair_dgl(argc, argv)
                                           : bt_pair_kbd(argc, argv);
#elif BLE_HAS_DONGLE
    return bt_pair_dgl(argc, argv);
#else
    return bt_pair_kbd(argc, argv);
#endif
}

/* AT+ROLE[=KBD|DONGLE] — query the runtime role; DUAL builds switch it
   (DataFlash flag + soft reset, <1 s). Single-role builds are query-only. */
static int at_cmd_ROLE(int argc, char *argv[]) {
    if (argc < 2) {
#if BLE_MODE == BLE_MODE_DUAL
        AT_Response("role=%s mode=DUAL", role_name(role_current()));
#elif BLE_HAS_DONGLE
        AT_Response("role=dongle mode=DONGLE");
#else
        AT_Response("role=kbd mode=KBD");
#endif
        return 0;
    }
    int r = role_parse(argv[1]);
    if (r < 0) { AT_Response("usage: AT+ROLE=KBD|DONGLE"); return -1; }
#if BLE_MODE == BLE_MODE_DUAL
    if (r == (int)role_current()) {
        AT_Response("role=%s (unchanged)", role_name((uint8_t)r));
        return 0;
    }
    if (role_request((uint8_t)r) != 0) {
        AT_Response("ERROR: role flag write failed");
        return -1;
    }
    AT_Response("switching to %s — resetting...", role_name((uint8_t)r));
    /* flush UART/CDC, then reset (same sequence as AT+RST) */
#ifdef DEBUG
    while ((R8_UART1_LSR & RB_LSR_TX_ALL_EMP) == 0) __nop();
#endif
    for (volatile uint32_t d = 0; d < 60000; d++) __nop();
    SYS_ResetExecute();
    return 0;   /* unreachable */
#else
    AT_Response("ERROR: single-role build (role=%s)", role_name(role_current()));
    return -1;
#endif
}

/* Infrared — AT+IR=<sub>,<args>... (sub parsed as argv[1]) */
#if(defined(HWS_IR)) && (HWS_IR == TRUE)
static uint8_t ir_inited = 0;
static int at_cmd_IR(int argc, char *argv[]) {
    if (argc < 3) { AT_Response("usage: AT+IR=NEC,<hex> | SIRC,<hex>,<bits> | RAW,<t1>,<t2>,..."); return -1; }
    if (!ir_inited) { hws_ir_init(); ir_inited = 1; }
    if (hws_ir_busy()) { AT_Response("ERROR: busy"); return -1; }
    const char *sub = argv[1];
    char c0 = sub[0] & ~0x20;   /* case-insensitive first letter */
    if (c0 == 'N') {            /* NEC,<hex> */
        if (hws_ir_nec((uint32_t)strtoul(argv[2], NULL, 0)) < 0) goto err;
    } else if (c0 == 'S') {     /* SIRC,<hex>,<bits> */
        if (argc < 4) { AT_Response("usage: AT+IR=SIRC,<hex>,<bits>"); return -1; }
        if (hws_ir_sirc((uint32_t)strtoul(argv[2], NULL, 0), (uint8_t)atoi(argv[3])) < 0) goto err;
    } else if (c0 == 'R') {     /* RAW,<t1>,<t2>,... */
        uint16_t us[64];
        int n = argc - 2;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++)
            us[i] = (uint16_t)strtoul(argv[2 + i], NULL, 0);
        if (hws_ir_raw(us, (uint8_t)n) < 0) goto err;
    } else {
        AT_Response("usage: AT+IR=NEC|SIRC|RAW,...");
        return -1;
    }
    return 0;
err:
    AT_Response("ERROR: ir send failed");
    return -1;
}
#else
static int at_cmd_IR(int argc, char *argv[])  { (void)argc; (void)argv; AT_Response("ERROR: disabled (HWS_IR=FALSE)"); return -1; }
#endif

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
 */
const at_cmd_t cmd_table[] = {
    /* Core */
    { "AT",         "handshake -> OK",                at_cmd_AT },
    { "AT+VER",     "firmware version",               at_cmd_VER },
    { "AT+HELP",    "command list",                   at_cmd_HELP },
    { "AT+ECHO",    "echo <text>",                    at_cmd_ECHO },
    { "AT+STATUS",  "device status (role/kb/ble/batt)", at_cmd_STATUS },
    { "AT+RST",     "software reset",                 at_cmd_RST },
    { "AT+ISP",     "enter ISP bootloader (erases app!)", at_cmd_ISP },
    { "AT+ROLE",    "query/switch role KBD|DONGLE (DUAL)", at_cmd_ROLE },
    /* Keyboard */
    { "AT+KB",      "keyboard mode USB|BLE|BOTH",     at_cmd_KB },
    { "AT+KEY",     "raw HID report <mods>,<k1>,..,<k6>", at_cmd_KEY },
    { "AT+TAP",     "press+release <ms>,<mods>,<k1>..<k6>", at_cmd_TAP },
    { "AT+MOD",     "set modifiers <mask>",           at_cmd_MOD },
    { "AT+KEY_SEQ", "batch HID <delay>,<mods>,<k1>..<k6>,...", at_cmd_KEY_SEQ },
    { "AT+KEY_STR", "type text <string> (US layout)", at_cmd_KEY_STR },
    /* GPIO */
    { "AT+GPIO_W",  "write <pin>,<level> (PA0-15,PB16-39)", at_cmd_GPIO_W },
    { "AT+GPIO_R",  "read <pin>",                      at_cmd_GPIO_R },
    /* Sensor */
    { "AT+ADC",     "read ADC <ch 0-13> -> mV",    at_cmd_ADC },
    { "AT+I2C_SCAN", "scan I2C bus",                at_cmd_I2C_SCAN },
    { "AT+I2C_R",   "I2C read <addr>,<reg>,<len> (hex)", at_cmd_I2C_R },
    { "AT+I2C_W",   "I2C write <addr>,<reg>,<data> (hex)", at_cmd_I2C_W },
    /* Power */
    { "AT+SLEEP",   "[stub] sleep <mode>",           at_cmd_SLEEP },
    /* Wireless */
    { "AT+BT_SCAN", "scan HID devices [sec] (dongle)", at_cmd_BT_SCAN },
    { "AT+BT_CONN", "connect <idx|addr|name> (dongle)", at_cmd_BT_CONN },
    { "AT+BT_DISC", "drop BLE link, re-advertise",   at_cmd_BT_DISC },
    { "AT+BT_PAIR", "drop link + erase bonds",       at_cmd_BT_PAIR },
    { "AT+BT_STATE","diag dongle state (dongle)",    at_cmd_BT_STATE },
    { "AT+BT_PASSKEY","SMP passkey <6digits> (dongle)", at_cmd_BT_PASSKEY },
    { "AT+BT_LIST", "bonded devices (dongle)",       at_cmd_BT_LIST },
    { "AT+BT_AUTO", "auto-reconnect [0|1] (dongle)", at_cmd_BT_AUTO },
    /* Infrared */
    { "AT+IR",      "IR=NEC|SIRC|RAW,...",         at_cmd_IR },
};
const int cmd_table_count = sizeof(cmd_table) / sizeof(cmd_table[0]);
