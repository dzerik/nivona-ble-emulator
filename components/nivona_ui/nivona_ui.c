// components/nivona_ui/nivona_ui.c — LVGL v9 bring-up via esp_lvgl_port.
//
// Phase 3: Stand up LVGL on the board display + touch handles and render
// a "hello world" UI with a touchable tap counter to confirm:
//   (a) LVGL renders correctly on the JD9853 panel
//   (b) AXS5106L touch drives LVGL input
//   (c) Touch coordinates land on the on-screen button (orientation correct)

#include "nivona_ui.h"

#include "board.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "nivona_ui";

// Screen dimensions (portrait mode, matching WS_LCD_WIDTH/WS_LCD_HEIGHT)
#define UI_HRES  172
#define UI_VRES  320

// Draw buffer: 20 lines worth of pixels, double-buffered.
// 172 * 20 * 2 bytes = 6 880 bytes per buffer — conservative for no-PSRAM C6.
#define UI_BUF_LINES 20
#define UI_BUF_SIZE  (UI_HRES * UI_BUF_LINES)

// ---------------------------------------------------------------------------
// Button click callback — increments counter and updates label
// ---------------------------------------------------------------------------
static void btn_click_cb(lv_event_t *e)
{
    static int count = 0;
    count++;

    lv_obj_t *btn = lv_event_get_target(e);
    // The button's first child is the label
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_label_set_text_fmt(lbl, "Tapped: %d", count);
    }
}

// ---------------------------------------------------------------------------
// nivona_ui_start — public entry point
// ---------------------------------------------------------------------------
void nivona_ui_start(void)
{
    // ---- 1. Init LVGL port ------------------------------------------------
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority   = 4,     // below BLE host (5), above idle
        .task_stack      = 6144,  // 6 kB — enough for v9 + our simple UI
        .task_affinity   = -1,    // no core affinity (single-core C6 ignores)
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,     // 5 ms tick
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // ---- 2. Add display ---------------------------------------------------
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = board_lcd_panel_io(),
        .panel_handle = board_lcd_panel(),
        .buffer_size  = UI_BUF_SIZE,
        .double_buffer = true,
        .hres = UI_HRES,
        .vres = UI_VRES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        // Portrait, no rotation/mirror — the panel was initialised with
        // mirror_x=false, mirror_y=false, swap_xy=false in board_early_init().
        // Touch uses mirror_x=1 to correct the raw AXS5106L axis, so we
        // do NOT add any additional display-side mirror here.
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = true,
            // Phase 1 self-test wrote red as 0x00F8 (byte-swapped RGB565).
            // The SPI driver transmits MSB-first, so LVGL must also
            // byte-swap its RGB565 output before handing it to the DMA.
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    // ---- 3. Add touch input -----------------------------------------------
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = board_touch(),
    };
    lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (!touch_indev) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed — touch disabled");
    }

    // ---- 4. Build UI (must hold LVGL lock) ---------------------------------
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_screen_active();

        // Dark background
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        // Title label "Nivona UI"
        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "Nivona UI");
        lv_obj_set_style_text_color(title, lv_color_hex(0xe0e0ff), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

        // Tap button
        lv_obj_t *btn = lv_button_create(scr);
        lv_obj_set_size(btn, 140, 60);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
        lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Tap me");
        lv_obj_center(btn_lbl);

        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "LVGL UI started");
}
