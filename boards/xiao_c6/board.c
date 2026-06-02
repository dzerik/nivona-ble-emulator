// boards/xiao_c6/board.c — Seeed XIAO ESP32-C6 profile.
#include "board.h"

#include "driver/gpio.h"
#include "esp_err.h"

static const board_info_t s_info = { .name = "xiao_c6", .has_display = false };

const board_info_t *board_info(void) { return &s_info; }

// XIAO ESP32-C6 RF front-end (FM8625H):
//   GPIO3  = RF_SWITCH_EN  -> HIGH enables the RF switch (required)
//   GPIO14 = RF_ANT_SELECT -> LOW = onboard PCB antenna, HIGH = external U.FL
void board_early_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << 3) | (1ULL << 14),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(3, 1));   // enable RF switch
    ESP_ERROR_CHECK(gpio_set_level(14, 1));  // external U.FL antenna
}
