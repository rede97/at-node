/*
 * features.h - compile-time feature switches for esp32_at_node.
 *
 * Override per build via compiler flags, e.g.
 *   arduino-cli compile --build-property \
 *     compiler.cpp.extra_flags="-DFEATURE_RATHOLE=0" ...
 * or simply: build.ps1 -Variant base|rathole   (see build.ps1)
 *
 * Disabling a feature removes its init, handlers, AT branches, REST
 * routes and config keys at compile time; the ability interface
 * (AT+ABILITY / GET /at-node/cmd/ability / MQTT "ability") reports
 * what each build actually contains, and the web UI hides the
 * corresponding tabs.
 */
#pragma once

#ifndef FEATURE_BLE
#define FEATURE_BLE        1   /* NimBLE HID keyboard + pairing */
#endif

#ifndef FEATURE_MQTT
#define FEATURE_MQTT       1   /* MQTT (TLS) control plane */
#endif

#ifndef FEATURE_RATHOLE
#define FEATURE_RATHOLE    1   /* rathole reverse-tunnel client */
#endif

#ifndef FEATURE_I2C
#define FEATURE_I2C        1   /* Wire on SDA=GPIO8, SCL=GPIO9 */
#endif

/* I2C off frees GPIO8, which is the onboard LED on common ESP32-C3
 * boards (SuperMini etc.). Use it as a breathing liveness indicator:
 * breathing = loop() alive; frozen/dark = wedged or dead. */
#ifndef FEATURE_BREATH_LED
#define FEATURE_BREATH_LED (!FEATURE_I2C)
#endif

#ifndef BREATH_LED_PIN
#define BREATH_LED_PIN     8
#endif

/* SuperMini C3 onboard blue LED is active-low; flip to 0 if your
 * board's LED is active-high. */
#ifndef BREATH_LED_ACTIVE_LOW
#define BREATH_LED_ACTIVE_LOW  1
#endif
