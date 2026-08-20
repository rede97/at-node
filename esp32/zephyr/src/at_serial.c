/*
 * AT-Node Zephyr — serial (UART0 console) AT interface.
 *
 * Dedicated RX thread polls zephyr,console (uart0, 115200) with
 * uart_poll_in(); printable chars are echoed and accumulated (300 char
 * line buffer). CR or LF terminates a command (a LF right after CR is
 * swallowed so CRLF terminals don't execute twice); empty lines are
 * ignored; Ctrl-C (0x03) cancels the line being typed; backspace edits.
 * Responses are emitted char-by-char via uart_poll_out() + CR/LF.
 *
 * Mirrors the Arduino variant's non-blocking handle_serial(): CR executes,
 * the following LF is an empty line and is skipped; overflow drops garbage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "at_core.h"
#include "at_serial.h"

#define AT_LINE_MAX   300
#define AT_STACK_SIZE 2048
#define AT_THREAD_PRIO 7

static const struct device *const at_uart =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static struct k_thread at_thread_data;
static K_THREAD_STACK_DEFINE(at_stack, AT_STACK_SIZE);

static void uart_emit(const char *line, void *ctx)
{
	const struct device *dev = ctx;

	while (*line != '\0') {
		uart_poll_out(dev, (unsigned char)*line++);
	}
	uart_poll_out(dev, '\r');
	uart_poll_out(dev, '\n');
}

static void at_rx_thread(void *a, void *b, void *c)
{
	static char line[AT_LINE_MAX + 1];
	int len = 0;
	bool skip_lf = false;
	unsigned char ch;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uart_emit("AT-Node Zephyr ready, AT+HELP for cmds", (void *)at_uart);

	while (1) {
		if (uart_poll_in(at_uart, &ch) != 0) {
			k_sleep(K_MSEC(2));
			continue;
		}

		if (ch == 0x03) { /* Ctrl-C: cancel the line being typed */
			len = 0;
			skip_lf = false;
			uart_emit("^C", (void *)at_uart);
			continue;
		}

		if (ch == '\r' || ch == '\n') {
			if (ch == '\n' && skip_lf) { /* LF of a CRLF pair */
				skip_lf = false;
				continue;
			}
			skip_lf = (ch == '\r');
			uart_poll_out(at_uart, '\r');
			uart_poll_out(at_uart, '\n');
			/* trim trailing spaces/tabs (Arduino line.trim()) */
			while (len > 0 &&
			       (line[len - 1] == ' ' || line[len - 1] == '\t')) {
				len--;
			}
			line[len] = '\0';
			if (len > 0) { /* empty lines ignored */
				at_handle_line(line, uart_emit, (void *)at_uart);
			}
			len = 0;
			continue;
		}
		skip_lf = false;

		if (ch == '\b' || ch == 0x7F) { /* backspace / DEL */
			if (len > 0) {
				len--;
				uart_poll_out(at_uart, '\b');
				uart_poll_out(at_uart, ' ');
				uart_poll_out(at_uart, '\b');
			}
			continue;
		}

		if (ch < 0x20) {
			continue; /* drop other control chars */
		}

		if (len < AT_LINE_MAX) {
			line[len++] = (char)ch;
			uart_poll_out(at_uart, ch); /* echo */
		} else {
			len = 0; /* overflow: drop garbage (Arduino semantics) */
		}
	}
}

void at_serial_init(void)
{
	if (!device_is_ready(at_uart)) {
		printk("AT: console UART not ready\n");
		return;
	}
	k_thread_create(&at_thread_data, at_stack, AT_STACK_SIZE, at_rx_thread,
			NULL, NULL, NULL, AT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&at_thread_data, "at_serial");
	printk("AT: serial console on uart0 (115200)\n");
}
