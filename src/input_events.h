#ifndef INPUT_EVENTS_H_
#define INPUT_EVENTS_H_

/*
 * System-wide input semantics.
 *
 * Buttons travel through the firmware as enums, never strings.  The ONLY
 * place an enum becomes a string is the LLSS API boundary (llss_send_input),
 * via ui_btn_llss_name() / ui_evt_llss_name().  Everything else — routing,
 * the device-UI menu, the long-press state machine — switches on the enum.
 * This keeps comparisons to integer equality instead of strcmp().
 *
 * (Prefix is UI_BTN_ rather than INPUT_BTN_ to avoid colliding with Zephyr's
 *  input-event-codes.h, which already defines INPUT_BTN_* gamepad macros.)
 */

enum ui_btn {
	UI_BTN_NONE = -1,
	UI_BTN_1 = 0,
	UI_BTN_2,
	UI_BTN_3,
	UI_BTN_4,
	UI_BTN_5,
	UI_BTN_6,
	UI_BTN_7,
	UI_BTN_8,
	UI_BTN_ENTER,
	UI_BTN_ESC,
	UI_BTN_HL_LEFT,
	UI_BTN_HL_RIGHT,
	/* btn10 in the top-strip contract: device-local menu trigger. Never
	 * forwarded to LLSS (ui_btn_llss_name returns NULL); device_ui owns
	 * the press and toggles the local menu overlay. */
	UI_BTN_MENU,
	UI_BTN_COUNT,
};

enum ui_evt {
	UI_EVT_PRESS = 0,
	UI_EVT_LONG_PRESS,
};

/** Map a Zephyr INPUT_KEY_* code to a logical button, or UI_BTN_NONE. */
enum ui_btn ui_btn_from_keycode(int keycode);

/** Map a logical button to its Zephyr INPUT_KEY_* code, or -1. */
int ui_btn_to_keycode(enum ui_btn btn);

/** Map an LLSS button name ("BTN_1", "ENTER", …) to a logical button. */
enum ui_btn ui_btn_from_name(const char *name);

/** LLSS wire name for a button ("BTN_1".."BTN_8","ENTER","ESC",
 *  "HL_LEFT","HL_RIGHT"), or NULL if out of range. */
const char *ui_btn_llss_name(enum ui_btn btn);

/** LLSS wire name for an event type ("PRESS" / "LONG_PRESS"). */
const char *ui_evt_llss_name(enum ui_evt evt);

#endif /* INPUT_EVENTS_H_ */
