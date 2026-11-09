/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_lv_decoder.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lv_demos.h"
#include "dashboard.h"
#include "sensors.h"
#include "sd_themes.h"
#include "usb_mode.h"

#define MAX_SD_THEMES 16

void app_main(void)
{
    /* NVS guarda la configuracion del dashboard (valores visibles, posiciones) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* El puerto USB vuelve a ser el de programar, por si venimos del modo USB */
    usb_mode_restore_phy();
    bool usb_mode = usb_mode_requested();

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_180,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    ESP_ERROR_CHECK(display != NULL ? ESP_OK : ESP_FAIL);

    bsp_display_backlight_on();

    ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);

    /* Modo USB (pedido desde AJUSTES): la SD queda como pendrive para la compu */
    if (usb_mode) {
        usb_mode_run();
        bsp_display_unlock();
        return;
    }

    /* true = valores simulados, hasta que conectemos los sensores reales */
    sensors_start(true);

    // lv_demo_music();
    // lv_demo_benchmark();
    // lv_demo_widgets();
    /* Decodificadores de PNG/JPG (el JPG usa el decodificador por hardware del P4) */
    esp_lv_decoder_handle_t decoder = NULL;
    ESP_ERROR_CHECK(esp_lv_decoder_init(&decoder));

    /* Dashboards: los que vienen en el firmware + los de la SD (/sdcard/dashboards) */
    static const dash_theme_t *themes[2 + MAX_SD_THEMES] = { &theme_initial_d, &theme_barras };
    size_t theme_count = 2;
    if (bsp_sdcard_mount() == ESP_OK) {
        theme_count += sd_themes_load(&themes[2], MAX_SD_THEMES);
    } else {
        ESP_LOGW("main", "No hay SD (o no se pudo leer): solo dashboards de fabrica");
    }
    dashboard_start(themes, theme_count);

    bsp_display_unlock();
}
