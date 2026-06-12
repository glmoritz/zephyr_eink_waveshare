/*
 * Input semantic conversions — the single translation point between Zephyr
 * keycodes, logical buttons (enum), and LLSS wire strings.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/input/input.h>

#include "input_events.h"

/* Logical button → Zephyr keycode.  Indexed by enum ui_btn. */
static const int32_t btn_keycode[UI_BTN_COUNT] = {
	[UI_BTN_1]        = INPUT_KEY_1,
	[UI_BTN_2]        = INPUT_KEY_2,
	[UI_BTN_3]        = INPUT_KEY_3,
	[UI_BTN_4]        = INPUT_KEY_4,
	[UI_BTN_5]        = INPUT_KEY_5,
	[UI_BTN_6]        = INPUT_KEY_6,
	[UI_BTN_7]        = INPUT_KEY_7,
	[UI_BTN_8]        = INPUT_KEY_8,
	[UI_BTN_ENTER]    = INPUT_KEY_ENTER,
	[UI_BTN_ESC]      = INPUT_KEY_ESC,
	[UI_BTN_HL_LEFT]  = INPUT_KEY_LEFT,
	[UI_BTN_HL_RIGHT] = INPUT_KEY_RIGHT,
};

/* Logical button → LLSS wire name.  Indexed by enum ui_btn. */
static const char *const btn_name[UI_BTN_COUNT] = {
	[UI_BTN_1]        = "BTN_1",
	[UI_BTN_2]        = "BTN_2",
	[UI_BTN_3]        = "BTN_3",
	[UI_BTN_4]        = "BTN_4",
	[UI_BTN_5]        = "BTN_5",
	[UI_BTN_6]        = "BTN_6",
	[UI_BTN_7]        = "BTN_7",
	[UI_BTN_8]        = "BTN_8",
	[UI_BTN_ENTER]    = "ENTER",
	[UI_BTN_ESC]      = "ESC",
	[UI_BTN_HL_LEFT]  = "HL_LEFT",
	[UI_BTN_HL_RIGHT] = "HL_RIGHT",
};

enum ui_btn ui_btn_from_keycode(int keycode)
{
	if (keycode == INPUT_KEY_KPENTER) {
		return UI_BTN_ENTER;
	}

	for (int32_t i = 0; i < UI_BTN_COUNT; i++) {
		if (btn_keycode[i] == keycode) {
			return (enum ui_btn)i;
		}
	}
	return UI_BTN_NONE;
}

int ui_btn_to_keycode(enum ui_btn btn)
{
	if (btn < 0 || btn >= UI_BTN_COUNT) {
		return -1;
	}
	return btn_keycode[btn];
}

enum ui_btn ui_btn_from_name(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return UI_BTN_NONE;
	}

	if (strcmp(name, "1") == 0) {
		return UI_BTN_1;
	}
	if (strcmp(name, "2") == 0) {
		return UI_BTN_2;
	}
	if (strcmp(name, "3") == 0) {
		return UI_BTN_3;
	}
	if (strcmp(name, "4") == 0) {
		return UI_BTN_4;
	}
	if (strcmp(name, "5") == 0) {
		return UI_BTN_5;
	}
	if (strcmp(name, "6") == 0) {
		return UI_BTN_6;
	}
	if (strcmp(name, "7") == 0) {
		return UI_BTN_7;
	}
	if (strcmp(name, "8") == 0) {
		return UI_BTN_8;
	}
	if ((strcmp(name, "en") == 0) || (strcmp(name, "enter") == 0)) {
		return UI_BTN_ENTER;
	}
	if ((strcmp(name, "esc") == 0) || (strcmp(name, "escape") == 0)) {
		return UI_BTN_ESC;
	}
	if ((strcmp(name, "hl") == 0) || (strcmp(name, "left") == 0)) {
		return UI_BTN_HL_LEFT;
	}
	if ((strcmp(name, "hr") == 0) || (strcmp(name, "right") == 0)) {
		return UI_BTN_HL_RIGHT;
	}

	for (int32_t i = 0; i < UI_BTN_COUNT; i++) {
		if (btn_name[i] && strcmp(btn_name[i], name) == 0) {
			return (enum ui_btn)i;
		}
	}
	return UI_BTN_NONE;
}

const char *ui_btn_llss_name(enum ui_btn btn)
{
	if (btn < 0 || btn >= UI_BTN_COUNT) {
		return NULL;
	}
	return btn_name[btn];
}

const char *ui_evt_llss_name(enum ui_evt evt)
{
	return (evt == UI_EVT_LONG_PRESS) ? "LONG_PRESS" : "PRESS";
}
