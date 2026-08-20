/*
 * AT-Node Zephyr — transport-agnostic AT command core.
 *
 * One parser/dispatcher shared by serial UART, HTTP /at-node/at and the
 * MQTT cmd topic, mirroring the Arduino variant's handle_at_command().
 *
 * Protocol: caller feeds one full command line (without trailing CR/LF).
 * Responses are delivered line-by-line through emit() WITHOUT CR/LF;
 * the transport adds framing. Every command ends with exactly one final
 * "OK" or "ERROR <reason>" line; data lines precede it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

typedef void (*at_emit_fn)(const char *line, void *ctx);

/* Dispatch one command line. Thread-safe (serializes internally). */
void at_handle_line(const char *line, at_emit_fn emit, void *ctx);

/* Synchronous helper: run one command, collect full response into buf
 * (lines separated by \n, always ends with "OK\n" or "ERROR ...\n").
 * Returns response length, truncated to len-1. For HTTP/MQTT transports.
 */
int at_handle_collect(const char *line, char *buf, int len);
