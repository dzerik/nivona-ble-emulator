// boards/board.h — common board hardware-abstraction interface.
//
// Every board profile (boards/<name>/board.c) implements this. main.c
// talks only to this interface, never to board-specific GPIO/peripherals.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;        // board key, e.g. "xiao_c6"
    bool        has_display; // runtime mirror of CONFIG_NIVONA_BOARD_HAS_DISPLAY
} board_info_t;

// Static description of the compiled-in board. Never NULL.
const board_info_t *board_info(void);

// Earliest board bring-up — called first thing in app_main(), before NVS
// and the BLE host. Does RF-switch / power / (later) display panel setup.
void board_early_init(void);

#if CONFIG_NIVONA_BOARD_HAS_DISPLAY
// Display accessors — only declared on boards that have a panel. Defined
// by that board's board.c and consumed by the nivona_ui component (added
// in the follow-up UI plan).
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
esp_lcd_panel_handle_t board_lcd_panel(void);
esp_lcd_touch_handle_t board_touch(void);
void                   board_set_backlight(uint8_t pct);
#endif

#ifdef __cplusplus
}
#endif
