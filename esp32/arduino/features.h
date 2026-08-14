/*
 * features.h - compile-time feature switches for the esp32/arduino variant.
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

/* Target gate: the Arduino variant supports classic ESP32 and ESP32-C3
 * only. ESP32-S3 is intentionally unsupported here — precompiled
 * esp32s3-libs ship CONFIG_SPIRAM_USE_MALLOC=y which crashes mbedTLS at
 * boot; S3 is carried by the Zephyr variant instead. Root cause:
 * esp32/COMPAT_REPORT.md. Fail at compile time, not in the field. */
#if !CONFIG_IDF_TARGET_ESP32 && !CONFIG_IDF_TARGET_ESP32C3
#error "Unsupported target: Arduino variant = classic ESP32 / ESP32-C3 only (S3 -> esp32/zephyr, see esp32/COMPAT_REPORT.md)"
#endif

/* Pin map is chip-conditional anywhere a GPIO number appears:
 * classic ESP32 reserves GPIO6-11 for internal SPI flash — driving any of
 * them (button, LED, I2C) corrupts flash access and watchdogs the chip
 * (~1s TG1WDT boot loop, 2026-08-15). Every pin #define must pick a value
 * per CONFIG_IDF_TARGET_*, never a single number for all chips. */

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

#ifndef FEATURE_HTTP
#define FEATURE_HTTP       1   /* LAN HTTP control plane on port 80 (incl. web UI).
                                * 0 = serial-only configuration: no routes are
                                * registered and the server never starts, removing
                                * the unauthenticated LAN attack surface. The
                                * button-triggered AP provisioning portal (8080)
                                * is independent and stays available. */
#endif

/* I2C off frees GPIO8, which is the onboard LED on common ESP32-C3
 * boards (SuperMini etc.). Use it as a breathing liveness indicator:
 * breathing = loop() alive; frozen/dark = wedged or dead. */
#ifndef FEATURE_BREATH_LED
#define FEATURE_BREATH_LED (!FEATURE_I2C)
#endif

/* Breath LED pin is chip-specific: GPIO8 is the C3 SuperMini onboard LED
 * but a flash data line (SD1) on classic ESP32 — ledcAttach(8) there would
 * break flash access just like the GPIO10 AP-button boot loop (2026-08-15).
 * Classic ESP32 devkits carry the onboard LED on GPIO2. */
#ifndef BREATH_LED_PIN
#if CONFIG_IDF_TARGET_ESP32
#define BREATH_LED_PIN     2
#else
#define BREATH_LED_PIN     8
#endif
#endif

/* SuperMini C3 onboard blue LED is active-low; flip to 0 if your
 * board's LED is active-high. */
#ifndef BREATH_LED_ACTIVE_LOW
#define BREATH_LED_ACTIVE_LOW  1
#endif
