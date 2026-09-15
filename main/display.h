#pragma once

/* Initialize the ST7789 LCD (LilyGo T-Display C5), bring up LVGL, build the
 * status UI, and start a task that refreshes it from g_status. Call once,
 * after the status subsystem exists. */
void display_init(void);
