/********************************** (C) COPYRIGHT *******************************
 * File Name          : role.c
 * Author             : at-node
 * Description        : Runtime BLE role flag (DUAL builds).
 *
 *   Storage: 4-byte record at APP_ROLE_FLASH_ADDR (DataFlash-relative,
 *   one 256-byte page below the SNV bond store):
 *
 *     [0] magic 'A' 'R'
 *     [2] role (ROLE_KBD / ROLE_DONGLE)
 *     [3] ~role (validation)
 *
 *   Erased flash reads 0xFF and fails validation -> ROLE_KBD default.
 *   Write path mirrors the BLE SNV store: EEPROM_ERASE + EEPROM_WRITE.
 ********************************************************************************/

#include "role.h"

#define ROLE_MAGIC0  'A'
#define ROLE_MAGIC1  'R'

uint8_t role_current(void)
{
#if BLE_MODE == BLE_MODE_DUAL
    uint8_t rec[4] = { 0 };
    EEPROM_READ(APP_ROLE_FLASH_ADDR, rec, sizeof(rec));
    if (rec[0] == ROLE_MAGIC0 && rec[1] == ROLE_MAGIC1 &&
        rec[2] == (uint8_t)(~rec[3]) && rec[2] <= ROLE_DONGLE)
        return rec[2];
    return ROLE_KBD;   /* erased/corrupt flag — safe default */
#elif BLE_HAS_DONGLE
    return ROLE_DONGLE;
#else
    return ROLE_KBD;
#endif
}

const char *role_name(uint8_t role)
{
    return (role == ROLE_DONGLE) ? "dongle" : "kbd";
}

int role_parse(const char *s)
{
    if (!s) return -1;
    /* case-insensitive without ctype.h */
    char c0 = s[0] & ~0x20;
    if (c0 == 'K') return ROLE_KBD;
    if (c0 == 'D') return ROLE_DONGLE;
    return -1;
}

int role_request(uint8_t role)
{
#if BLE_MODE == BLE_MODE_DUAL
    if (role > ROLE_DONGLE)
        return -1;
    uint8_t rec[4] = { ROLE_MAGIC0, ROLE_MAGIC1, role, (uint8_t)~role };
    if (EEPROM_ERASE(APP_ROLE_FLASH_ADDR, sizeof(rec)) != 0)
        return -1;
    if (EEPROM_WRITE(APP_ROLE_FLASH_ADDR, rec, sizeof(rec)) != 0)
        return -1;
    return 0;
#else
    (void)role;
    return -1;
#endif
}
