// boards/waveshare_c6_lcd_1_47/board.c — Waveshare ESP32-C6-Touch-LCD-1.47.
//
// Phase 1: JD9853 LCD + LEDC backlight bring-up.
// Self-test (NIVONA_DISPLAY_SELFTEST=1): fills screen solid RED at boot.

#define NIVONA_DISPLAY_SELFTEST 1

#include "board.h"

#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_touch.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "board_ws";

// ---- Pin definitions (mirror Waveshare reference demo) ----
#define WS_SPI_HOST     SPI2_HOST
#define WS_PIN_SCLK     GPIO_NUM_1
#define WS_PIN_MOSI     GPIO_NUM_2
#define WS_PIN_MISO     GPIO_NUM_3
#define WS_PIN_LCD_CS   GPIO_NUM_14
#define WS_PIN_LCD_DC   GPIO_NUM_15
#define WS_PIN_LCD_RST  GPIO_NUM_22
#define WS_PIN_LCD_BL   GPIO_NUM_23

#define WS_LCD_WIDTH    172
#define WS_LCD_HEIGHT   320

// ---- LEDC backlight config ----
#define WS_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define WS_LEDC_TIMER       LEDC_TIMER_0
#define WS_LEDC_CHANNEL     LEDC_CHANNEL_0
#define WS_LEDC_DUTY_RES    LEDC_TIMER_10_BIT   // 10-bit = 1024 steps
#define WS_LEDC_FREQ_HZ     5000
#define WS_LEDC_DUTY_MAX    1024                // 2^10

// ---- Static handles ----
static esp_lcd_panel_io_handle_t s_io    = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

// ---- Board info ----
static const board_info_t s_info = {
    .name        = "waveshare_c6_lcd_1_47",
    .has_display = true,
};

const board_info_t *board_info(void) { return &s_info; }

// ---- Backlight (LEDC PWM) ----
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = WS_LEDC_MODE,
        .timer_num       = WS_LEDC_TIMER,
        .duty_resolution = WS_LEDC_DUTY_RES,
        .freq_hz         = WS_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = WS_LEDC_MODE,
        .channel    = WS_LEDC_CHANNEL,
        .timer_sel  = WS_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = WS_PIN_LCD_BL,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ESP_LOGI(TAG, "backlight LEDC initialised");
}

void board_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * (WS_LEDC_DUTY_MAX - 1) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(WS_LEDC_MODE, WS_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(WS_LEDC_MODE, WS_LEDC_CHANNEL));
}

// ---- Public accessors ----
esp_lcd_panel_handle_t board_lcd_panel(void) { return s_panel; }
esp_lcd_touch_handle_t board_touch(void)     { return NULL; }  // Phase 2

// ---- Board early init ----
void board_early_init(void)
{
    ESP_LOGI(TAG, "board_early_init: SPI2 + JD9853 LCD");

    // --- SPI bus ---
    spi_bus_config_t buscfg = {
        .sclk_io_num   = WS_PIN_SCLK,
        .mosi_io_num   = WS_PIN_MOSI,
        .miso_io_num   = WS_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(WS_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI2 bus initialised");

    // --- Panel IO (SPI -> LCD) ---
    esp_lcd_panel_io_spi_config_t io_config =
        JD9853_PANEL_IO_SPI_CONFIG(WS_PIN_LCD_CS, WS_PIN_LCD_DC, NULL, NULL);
    io_config.pclk_hz = 80 * 1000 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)WS_SPI_HOST, &io_config, &s_io));
    ESP_LOGI(TAG, "panel IO created");

    // --- JD9853 panel ---
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num  = WS_PIN_LCD_RST,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(s_io, &panel_config, &s_panel));
    ESP_LOGI(TAG, "JD9853 panel created");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    // The 1.47" glass shows 172 of the controller's 240 RAM columns,
    // centred with a 34-px offset. Without this the image is shifted and
    // a black band appears on one side. x_gap=34 maps draw coords 0..171
    // onto visible RAM columns 34..205.
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 34, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "JD9853 panel on");

    // --- Backlight ---
    backlight_init();

#if NIVONA_DISPLAY_SELFTEST
    // Temporary self-test: fill entire screen RED (RGB565 big-endian).
    // RGB565 red = 0xF800. The ESP-IDF SPI driver sends bytes MSB-first,
    // so the host must byte-swap: store 0x00F8 per pixel so the wire carries
    // F8 00 which the panel interprets as red in RGB565.
    board_set_backlight(100);

    uint16_t *linebuf = (uint16_t *)malloc(WS_LCD_WIDTH * sizeof(uint16_t));
    if (linebuf) {
        // 0xF800 byte-swapped → 0x00F8
        uint16_t red_be = ((0xF800u >> 8) | (0xF800u << 8)) & 0xFFFFu;
        for (int x = 0; x < WS_LCD_WIDTH; x++) {
            linebuf[x] = red_be;
        }
        for (int y = 0; y < WS_LCD_HEIGHT; y++) {
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
                s_panel, 0, y, WS_LCD_WIDTH, y + 1, linebuf));
        }
        free(linebuf);
        ESP_LOGI(TAG, "selftest: filled red");
    } else {
        ESP_LOGE(TAG, "selftest: malloc failed");
    }
#endif
}
