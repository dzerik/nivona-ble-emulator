// boards/waveshare_c6_lcd_1_47/board.c — Waveshare ESP32-C6-Touch-LCD-1.47.
#include "board.h"

// has_display stays false until the UI plan adds the JD9853/CST816 bring-up
// and flips CONFIG_NIVONA_BOARD_HAS_DISPLAY=y.
static const board_info_t s_info = {
    .name = "waveshare_c6_lcd_1_47", .has_display = false,
};

const board_info_t *board_info(void) { return &s_info; }

// PCB antenna, no software RF switch. Display bring-up is added by the UI
// plan; nothing to do at early init while headless.
void board_early_init(void) { }
