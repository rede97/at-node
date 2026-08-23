/*
 * features.h - compile-time feature switches for the esp32/arduino variant.
 *
 * Unified feature model (shared vocabulary with the rust-s3 variant):
 *
 *   Core (device capability)        Comm (control plane)
 *     FEATURE_BLE   NimBLE HID kbd    FEATURE_HTTP    LAN HTTP + web UI
 *     ATNODE_LED    LED (see below)   FEATURE_MQTT    MQTT (TLS)
 *     FEATURE_I2C   Wire bus          FEATURE_RATHOLE reverse tunnel
 *   WiFi is the transport base and is always on.
 *
 * Board profile: ATNODE_BOARD picks the chip profile (pin map + LED
 * defaults) in ONE place; it defaults to the compile target, so the
 * usual build-c3.ps1 / build-esp32.ps1 flow needs no extra flags.
 * An explicit -DATNODE_BOARD=... that mismatches the compile target
 * is a compile error.
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
 * corresponding tabs. Invalid feature/pin combinations are compile
 * errors (#error at the bottom of this file), never runtime surprises.
 */
#pragma once

/* Target gate: the Arduino variant supports classic ESP32 and ESP32-C3
 * only. ESP32-S3 is intentionally unsupported here — precompiled
 * esp32s3-libs ship CONFIG_SPIRAM_USE_MALLOC=y which crashes mbedTLS at
 * boot; S3 is carried by the Rust variant instead. Root cause:
 * esp32/COMPAT_REPORT.md. Fail at compile time, not in the field. */
#if !CONFIG_IDF_TARGET_ESP32 && !CONFIG_IDF_TARGET_ESP32C3
#error "Unsupported target: Arduino variant = classic ESP32 / ESP32-C3 only (S3 -> esp32/rust, see esp32/COMPAT_REPORT.md)"
#endif

/* ---------------------------------------------------------- board ------ */

#define ATNODE_BOARD_C3     1
#define ATNODE_BOARD_ESP32  2

/* Board profile selector. Defaults to the compile target; override ONLY
 * to cross-check a profile (mismatch with the real target = #error). */
#ifndef ATNODE_BOARD
#if CONFIG_IDF_TARGET_ESP32
#define ATNODE_BOARD  ATNODE_BOARD_ESP32
#else
#define ATNODE_BOARD  ATNODE_BOARD_C3
#endif
#endif

#if ATNODE_BOARD == ATNODE_BOARD_ESP32 && !CONFIG_IDF_TARGET_ESP32
#error "ATNODE_BOARD=ESP32 but the compile target is not classic ESP32"
#endif
#if ATNODE_BOARD == ATNODE_BOARD_C3 && !CONFIG_IDF_TARGET_ESP32C3
#error "ATNODE_BOARD=C3 but the compile target is not ESP32-C3"
#endif
#if ATNODE_BOARD != ATNODE_BOARD_C3 && ATNODE_BOARD != ATNODE_BOARD_ESP32
#error "ATNODE_BOARD must be ATNODE_BOARD_C3 or ATNODE_BOARD_ESP32"
#endif

/* Board profile pin map. Every chip-conditional pin in the firmware comes
 * from here — never put a bare GPIO number in the sketch:
 * classic ESP32 reserves GPIO6-11 for internal SPI flash — driving any of
 * them (button, LED, I2C) corrupts flash access and watchdogs the chip
 * (~1s TG1WDT boot loop, 2026-08-15). */
#if ATNODE_BOARD == ATNODE_BOARD_ESP32
#define PIN_I2C_SDA     21  /* classic: GPIO8/9 are flash lines */
#define PIN_I2C_SCL     22
#define PIN_AP_TRIGGER  0   /* BOOT button; GPIO10 is a flash line there */
#else
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9
#define PIN_AP_TRIGGER  10
#endif

/* ------------------------------------------------------- features ------ */

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
#define FEATURE_I2C        1   /* Wire on PIN_I2C_SDA / PIN_I2C_SCL */
#endif

#ifndef FEATURE_HTTP
#define FEATURE_HTTP       1   /* LAN HTTP control plane on port 80 (incl. web UI).
                                * 0 = serial-only configuration: no routes are
                                * registered and the server never starts, removing
                                * the unauthenticated LAN attack surface. The
                                * button-triggered AP provisioning portal (8080)
                                * is independent and stays available. */
#endif

/* SSDP/UPnP discovery (ssdp.cpp): Windows Explorer "Network" shows the
 * device and "View device webpage" opens the SPA. Requires the HTTP
 * service (serves /description.xml); at runtime it follows the HTTP
 * enable switch. Defaults on whenever HTTP is compiled in. */
#ifndef FEATURE_SSDP
#define FEATURE_SSDP       FEATURE_HTTP
#endif
#if FEATURE_SSDP && !FEATURE_HTTP
#error "FEATURE_SSDP requires FEATURE_HTTP (serves /description.xml)"
#endif

/* ------------------------------------------------------------ LED ------ */

/* Unified LED model (same vocabulary as the rust-s3 variant):
 *   ATNODE_LED = 0  none
 *              = 1  breath — PWM liveness on the onboard LED
 *                   (breathing = loop() alive; frozen/dark = wedged)
 *              = 2  color  — WS2812 (reserved model hook; the Arduino
 *                   driver is not implemented yet — use rust-s3 for color)
 * Defaults:
 *   classic ESP32: breath on GPIO2 — devkits (DevKit v1 etc.) carry the
 *     onboard blue LED there; it touches no firmware pin (I2C 21/22, AP
 *     trigger 0, flash 6-11, UART0 1/3). GPIO2 is a strapping pin but is
 *     don't-care for normal SPI boot, and its ADC2 role is moot (ADC2 is
 *     unusable while WiFi is up). Opt out with -DATNODE_LED=0.
 *   ESP32-C3: GPIO8 is both the SuperMini onboard LED and the default I2C
 *     SDA — breath XOR I2C, decided here by FEATURE_I2C. */
#ifndef ATNODE_LED
#if ATNODE_BOARD == ATNODE_BOARD_ESP32
#define ATNODE_LED       1
#else
#define ATNODE_LED       (FEATURE_I2C ? 0 : 1)
#endif
#endif

#define ATNODE_LED_NONE    0
#define ATNODE_LED_BREATH  1
#define ATNODE_LED_COLOR   2

#ifndef LED_BREATH_PIN
#if ATNODE_BOARD == ATNODE_BOARD_ESP32
#define LED_BREATH_PIN   2   /* DevKit v1 onboard blue LED */
#else
#define LED_BREATH_PIN   8   /* C3 SuperMini onboard LED */
#endif
#endif

/* DevKit v1 (classic) LED is active-high; SuperMini C3 is active-low. */
#ifndef LED_BREATH_ACTIVE_LOW
#if ATNODE_BOARD == ATNODE_BOARD_ESP32
#define LED_BREATH_ACTIVE_LOW  0
#else
#define LED_BREATH_ACTIVE_LOW  1
#endif
#endif

/* No default: a WS2812 pin is board-specific wiring, user picks a free
 * GPIO (validated below). */
#ifndef LED_COLOR_PIN
#define LED_COLOR_PIN    -1
#endif

/* ------------------------------------------------- conflict checks ----- */

#if ATNODE_LED != ATNODE_LED_NONE && ATNODE_LED != ATNODE_LED_BREATH && \
    ATNODE_LED != ATNODE_LED_COLOR
#error "ATNODE_LED must be 0 (none), 1 (breath) or 2 (color)"
#endif

#if ATNODE_LED == ATNODE_LED_BREATH
  /* flash data lines: classic GPIO6-11 (see profile comment above) */
  #if ATNODE_BOARD == ATNODE_BOARD_ESP32 && \
      LED_BREATH_PIN >= 6 && LED_BREATH_PIN <= 11
  #error "classic ESP32: LED_BREATH_PIN must not be 6..11 (SPI flash lines)"
  #endif
  /* LED pin must not sit on the I2C bus */
  #if FEATURE_I2C && \
      (LED_BREATH_PIN == PIN_I2C_SDA || LED_BREATH_PIN == PIN_I2C_SCL)
  #error "LED breath pin conflicts with I2C — pick one (ATNODE_LED=0 or FEATURE_I2C=0)"
  #endif
  /* ...or on the AP provisioning trigger */
  #if LED_BREATH_PIN == PIN_AP_TRIGGER
  #error "LED breath pin conflicts with the AP trigger button pin"
  #endif
#endif

#if ATNODE_LED == ATNODE_LED_COLOR
  #if LED_COLOR_PIN < 0
  #error "ATNODE_LED=2 requires -DLED_COLOR_PIN=<free gpio>"
  #endif
  #if ATNODE_BOARD == ATNODE_BOARD_ESP32 && \
      LED_COLOR_PIN >= 6 && LED_COLOR_PIN <= 11
  #error "classic ESP32: LED_COLOR_PIN must not be 6..11 (SPI flash lines)"
  #endif
  #if FEATURE_I2C && \
      (LED_COLOR_PIN == PIN_I2C_SDA || LED_COLOR_PIN == PIN_I2C_SCL)
  #error "LED_COLOR_PIN conflicts with I2C — pick one"
  #endif
  #if LED_COLOR_PIN == PIN_AP_TRIGGER
  #error "LED_COLOR_PIN conflicts with the AP trigger button pin"
  #endif
  #error "ATNODE_LED=2 (WS2812) is a reserved model hook on Arduino: driver not implemented yet (use esp32/rust for color)"
#endif
