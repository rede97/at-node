/*
 * AT-Node Zephyr — serial (UART0 console) AT interface.
 * Line-oriented: chars until CR/LF, echo on, then at_handle_line().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

void at_serial_init(void); /* spawns RX thread on zephyr,console UART */
