/********************************** (C) COPYRIGHT *******************************
 * File Name          : role.h
 * Author             : at-node
 * Description        : Runtime BLE role (DUAL builds) — DataFlash flag +
 *                      soft-reset switching, see REQUIREMENTS.md §3.1.2.
 *
 *   Single-role builds (BLE_MODE_KBD / BLE_MODE_DONGLE) compile this as
 *   constants: role_current() folds to the build role, role_request()
 *   always fails. Only BLE_MODE_DUAL persists a flag in DataFlash
 *   (APP_ROLE_FLASH_ADDR) and honors AT+ROLE.
 ********************************************************************************/

#ifndef ROLE_H
#define ROLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

/* Runtime roles (distinct from the BLE_MODE_* build modes) */
#define ROLE_KBD     0
#define ROLE_DONGLE  1

/*********************************************************************
 * role_current — resolved runtime role.
 *
 *   DUAL: read from the DataFlash flag (validated; corrupt/erased flag
 *   falls back to ROLE_KBD). Single-role builds: constant.
 */
uint8_t     role_current(void);

/*********************************************************************
 * role_name — "kbd" / "dongle" for AT+VER / AT+ROLE output.
 */
const char *role_name(uint8_t role);

/*********************************************************************
 * role_parse — "KBD"/"DONGLE" (any case) -> ROLE_*, else -1.
 */
int         role_parse(const char *s);

/*********************************************************************
 * role_request — stage a role switch (DUAL builds only).
 *
 *   Writes the DataFlash flag; the caller then soft-resets (flag is
 *   applied at boot). Returns 0 on success, -1 in single-role builds
 *   or for an invalid role.
 */
int         role_request(uint8_t role);

#ifdef __cplusplus
}
#endif

#endif /* ROLE_H */
