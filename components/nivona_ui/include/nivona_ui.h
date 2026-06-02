#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Initializes LVGL (via esp_lvgl_port) on the board display + touch and
// shows the UI. Call once after board_early_init() and the rest of app
// init. Safe no-op contract: only call on boards with a display.
void nivona_ui_start(void);

#ifdef __cplusplus
}
#endif
