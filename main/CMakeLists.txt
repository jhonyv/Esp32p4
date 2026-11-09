file(GLOB ASSET_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.c")

idf_component_register(
    SRCS main.c dashboard.c sensors.c theme_initial_d.c theme_barras.c sd_themes.c usb_mode.c ${ASSET_SRCS}
    INCLUDE_DIRS .
    REQUIRES esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl esp_timer nvs_flash espressif__esp_lv_decoder
             espressif__esp_tinyusb sdmmc esp_driver_sdmmc soc)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)
