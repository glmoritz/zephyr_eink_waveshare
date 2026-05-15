#ifndef DISPLAY_THREAD_H_
#define DISPLAY_THREAD_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Initialise the LVGL screen objects and mark the display worker ready.
 *        Must be called once from main() after the kernel is up.
 */
void ui_init(void);

/**
 * @brief Update the two-line status overlay on the display.
 *        Safe to call from any thread.
 */
void ui_set_status(const char *line1, const char *line2);

/**
 * @brief Hand a PNG buffer to the display thread for rendering.
 *
 * The buffer is copied internally; the caller's buffer may be freed after
 * this returns.
 *
 * @return 0 on success, -ENODEV if display not ready, -EMSGSIZE if too large.
 */
int queue_display_frame(const uint8_t *png_buf, size_t png_len);

#endif /* DISPLAY_THREAD_H_ */
