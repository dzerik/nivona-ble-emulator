# nivona_ui is only compiled on boards that have a display.
# On headless boards the component still registers (so other components can
# list it as a dependency) but contributes no sources, so the linker sees an
# empty archive and the build stays green.
if(CONFIG_NIVONA_BOARD_HAS_DISPLAY)
    set(_srcs "nivona_ui.c")
    set(_requires esp_lcd espressif__esp_lcd_touch)
    set(_priv_requires esp_lvgl_port)
else()
    set(_srcs "")
    set(_requires "")
    set(_priv_requires "")
endif()

idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/boards"
    REQUIRES ${_requires}
    PRIV_REQUIRES ${_priv_requires})

# board.h (in PRIV_INCLUDE_DIRS) includes esp_lcd_touch.h and esp_lcd headers.
# The managed component's include path may not be transitively propagated in
# all ESP-IDF versions, so pin it explicitly.
if(CONFIG_NIVONA_BOARD_HAS_DISPLAY)
    target_include_directories(__idf_nivona_ui PRIVATE
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_lcd_touch/include"
        "${CMAKE_SOURCE_DIR}/components/esp_lcd_jd9853/include"
        "${CMAKE_SOURCE_DIR}/components/esp_lcd_touch_axs5106/include"
    )
endif()
