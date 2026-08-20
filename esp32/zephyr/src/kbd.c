/*
 * AT-Node Zephyr — keyboard routing layer (CH582 kb_* equivalent).
 *
 * kbd_send_report() fans a boot-keyboard report out to the enabled targets
 * (AT+DEV bitmask). kbd_tap()/kbd_type_text() run on a dedicated sequence
 * thread so press+release are always paired atomically (FIELD-NOTES F18)
 * and AT/HTTP/MQTT callers never block on key timing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "kbd.h"

static uint8_t kb_targets = KB_TGT_ALL;

/* ------------------------------------------------------------------ */
/* Sequence engine: one job queue, serialized tap/text execution        */
/* ------------------------------------------------------------------ */

enum job_type { JOB_TAP, JOB_TEXT };

struct kbd_job {
	enum job_type type;
	uint16_t ms;
	uint16_t gap;
	uint8_t mods;
	uint8_t key;
	char text[257]; /* AT+KEY_STR payload limit */
};

K_MSGQ_DEFINE(kbd_jobs, sizeof(struct kbd_job), 4, 4);

/* ASCII -> HID usage (boot keyboard, US layout). Returns keycode, sets
 * *shift when Shift modifier is required. 0 = untypable.
 */
static uint8_t ascii_to_hid(char c, bool *shift)
{
	*shift = false;

	if (c >= 'a' && c <= 'z') {
		return 0x04 + (c - 'a');
	}
	if (c >= 'A' && c <= 'Z') {
		*shift = true;
		return 0x04 + (c - 'A');
	}
	if (c >= '1' && c <= '9') {
		return 0x1E + (c - '1');
	}
	switch (c) {
	case '0': return 0x27;
	case '\n': return 0x28; /* Enter */
	case '\t': return 0x2B;
	case ' ': return 0x2C;
	case '-': return 0x2D;
	case '=': return 0x2E;
	case '[': return 0x2F;
	case ']': return 0x30;
	case '\\': return 0x31;
	case ';': return 0x33;
	case '\'': return 0x34;
	case '`': return 0x35;
	case ',': return 0x36;
	case '.': return 0x37;
	case '/': return 0x38;
	}
	/* shifted digits/punct */
	static const char sym[] = "!@#$%^&*";
	static const uint8_t sym_kc[] = { 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };
	const char *p = strchr(sym, c);

	if (p != NULL) {
		*shift = true;
		return sym_kc[p - sym];
	}
	switch (c) {
	case '_': *shift = true; return 0x2D;
	case '+': *shift = true; return 0x2E;
	case '{': *shift = true; return 0x2F;
	case '}': *shift = true; return 0x30;
	case '|': *shift = true; return 0x31;
	case ':': *shift = true; return 0x33;
	case '"': *shift = true; return 0x34;
	case '~': *shift = true; return 0x35;
	case '<': *shift = true; return 0x36;
	case '>': *shift = true; return 0x37;
	case '?': *shift = true; return 0x38;
	}
	return 0;
}

static void press_release(uint8_t mods, uint8_t key, uint16_t ms)
{
	uint8_t keys[6] = { key, 0, 0, 0, 0, 0 };
	uint8_t none[6] = { 0 };

	kbd_send_report(mods, keys);
	k_sleep(K_MSEC(ms));
	kbd_send_report(0, none);
}

static void seq_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct kbd_job job;

	while (k_msgq_get(&kbd_jobs, &job, K_FOREVER) == 0) {
		if (job.type == JOB_TAP) {
			press_release(job.mods, job.key, job.ms);
		} else {
			for (const char *p = job.text; *p != '\0'; p++) {
				bool shift;
				uint8_t kc = ascii_to_hid(*p, &shift);

				if (kc == 0) {
					continue;
				}
				press_release(shift ? 0x02 : job.mods, kc, job.ms);
				if (job.gap != 0) {
					k_sleep(K_MSEC(job.gap));
				}
			}
		}
	}
}

K_THREAD_DEFINE(kbd_seq, 2048, seq_thread, NULL, NULL, NULL, 7, 0, 0);

void kbd_init(void)
{
	/* msgq + thread are static; nothing else to do */
}

/* ------------------------------------------------------------------ */
/* Routing                                                              */
/* ------------------------------------------------------------------ */

void kbd_set_targets(uint8_t mask)
{
	kb_targets = mask & KB_TGT_ALL;
}

bool kbd_typing(void)
{
	return k_msgq_num_used_get(&kbd_jobs) > 0;
}

uint8_t kbd_get_targets(void)
{
	return kb_targets;
}

int kbd_send_report(uint8_t mods, const uint8_t keys[6])
{
	int rc = -ENOTCONN;
	bool any = false;

	if ((kb_targets & KB_TGT_BLE) != 0 && kbd_ble_connected()) {
		if (kbd_ble_send(mods, keys) == 0) {
			any = true;
		}
	}
	if ((kb_targets & KB_TGT_USB) != 0 && kbd_usb_ready()) {
		if (kbd_usb_send(mods, keys) == 0) {
			any = true;
		}
	}
	if (any) {
		rc = 0;
	}
	return rc;
}

int kbd_tap(uint16_t ms, uint8_t mods, uint8_t key)
{
	struct kbd_job job = { .type = JOB_TAP, .ms = ms, .mods = mods, .key = key };

	if (ms == 0 || ms > 10000 || key == 0) {
		return -EINVAL;
	}
	return k_msgq_put(&kbd_jobs, &job, K_NO_WAIT);
}

int kbd_type_text(const char *s, uint16_t ms, uint16_t gap)
{
	struct kbd_job job = { .type = JOB_TEXT, .ms = ms, .gap = gap };
	size_t n = strlen(s);

	if (n == 0 || n >= sizeof(job.text) || ms == 0) {
		return -EINVAL;
	}
	memcpy(job.text, s, n + 1);
	return k_msgq_put(&kbd_jobs, &job, K_NO_WAIT);
}
