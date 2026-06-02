set(_req
    nvs_flash
    bt
    esp_hw_support
    console
    esp_wifi
    esp_netif
    esp_event
    esp_http_server
    app_update
    mdns
    lwip
    driver
)
if(CONFIG_NIVONA_BOARD_HAS_DISPLAY)
    list(APPEND _req
        esp_lcd
        esp_lcd_jd9853
        esp_lcd_touch_axs5106
        espressif__esp_lcd_touch
        esp_driver_spi
        esp_driver_ledc
        esp_driver_gpio
        esp_driver_i2c
        nivona_ui
    )
endif()

idf_component_register(
    SRCS
        "main.c"
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/board.c"
        "nivona_ble.c"
        "nivona_gatt.c"
        "nivona_dis.c"
        "nivona_frame.c"
        "nivona_crypto.c"
        "nivona_dispatch.c"
        "nivona_families.c"
        "nivona_stats.c"
        "nivona_consumables.c"
        "nivona_maint.c"
        "nivona_maint_cycle.c"
        "nivona_fsm.c"
        "nivona_store.c"
        "nivona_brew.c"
        "nivona_cli.c"
        "nivona_wifi.c"
        "nivona_ota.c"
        "nivona_telnet.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/boards"
    REQUIRES ${_req}
)

if(CONFIG_NIVONA_BOARD_HAS_DISPLAY)
    target_include_directories(__idf_main PRIVATE
        "${CMAKE_SOURCE_DIR}/components/esp_lcd_jd9853/include"
        "${CMAKE_SOURCE_DIR}/components/esp_lcd_touch_axs5106/include"
        "${CMAKE_SOURCE_DIR}/components/nivona_ui/include"
    )
endif()
