# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Board definition for MuseLab nanoESP32-S3 lives in ./boards
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(atnode_zephyr)

# Per-service HTTP resource iterable section (required by HTTP_SERVICE_DEFINE)
zephyr_linker_sources(SECTIONS sections-rom.ld)

# Shim so the shared Arduino-generated web_page.h (<pgmspace.h>/PROGMEM)
# compiles unmodified under Zephyr.
target_include_directories(app PRIVATE src src/compat)

target_sources(app PRIVATE
	src/main.c
	src/cfg.c
	src/at_core.c
	src/at_serial.c
	src/kbd.c
	src/kbd_ble.c
	src/kbd_usb.c
	src/led.c
	src/wifi_sta.c
	src/httpd.c
	src/mqttc.c
	src/hws.c
)
