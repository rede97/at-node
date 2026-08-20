/*
 * AT-Node Zephyr — hardware services (GPIO / ADC / I2C).
 *
 * Pin map (nanoESP32-S3): I2C0 SDA=GPIO8 SCL=GPIO9 (app.overlay);
 * ADC0 channels per esp32s3 pinout (ch0=GPIO1 .. ch9=GPIO10);
 * GPIO 6-11 are flash lines on classic ESP32 only, but S3 strapping/USB
 * pins (0,3,19,20,45,46) are rejected by hws_gpio_*.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

int hws_init(void);

int hws_gpio_write(uint8_t pin, int level);     /* -EINVAL unsafe pin */
int hws_gpio_read(uint8_t pin, int *level);
int hws_adc_read_mv(uint8_t ch, int *mv);       /* -EINVAL bad channel */
int hws_i2c_scan(char *buf, size_t len);        /* "+I2C: 0x3C 0x50" / "+I2C: none" */
int hws_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
int hws_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);
