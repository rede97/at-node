/*
 * ap_portal.h — AP mode WiFi provisioning portal
 *
 * Provides a captive portal for first-time WiFi configuration.
 * Triggered by GPIO10 button hold (3s) or AT+AP=1 command.
 *
 * AP name: AT-NODE-{devname}
 * AP password: ATNODECFG
 */

#ifndef AP_PORTAL_H
#define AP_PORTAL_H

#include <Arduino.h>

/* Start AP mode provisioning portal.
 * Returns immediately; AP and HTTP server run in background.
 * AP name is derived from g_device_name: "AT-NODE-{devname}".
 * AP password is "ATNODECFG".
 */
void ap_portal_start(void);

/* Stop AP mode and resume normal operation.
 * Restarts the device to apply saved WiFi credentials.
 */
void ap_portal_stop(void);

/* Check if AP mode is currently active. */
bool ap_portal_active(void);

/* Poll AP portal (call from loop()). Handles DNS and HTTP. */
void ap_portal_poll(void);

/* Check GPIO10 button trigger. Call once from setup().
 * Returns true if AP mode was started by button.
 */
bool ap_portal_check_button(void);

#endif /* AP_PORTAL_H */
