#include "usb_mode.h"

#include <stdlib.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "lvgl.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "soc/lp_system_struct.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "usb_mode";

#define NVS_NAMESPACE "dash"
#define NVS_KEY_USB   "usb_req"

static sdmmc_card_t *s_card;
static volatile bool s_attached;      /* lo escribe la tarea de USB */
static lv_obj_t *s_status;

/*
 * El P4 tiene dos PHY USB de 12 Mbit. De fabrica el PHY 0 (GPIO24/25, el
 * Type-C "USB 1.1 FS" de la placa) es del USB Serial/JTAG que se usa para
 * programar. Para el modo pendrive se lo pasamos al USB OTG; este registro
 * no se borra con un reinicio por software, por eso se restaura al arrancar.
 */
static void phy_give_to_otg(void)
{
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 1;
}

void usb_mode_restore_phy(void)
{
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;
}

bool usb_mode_requested(void)
{
    nvs_handle_t h;
    uint8_t req = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    if (nvs_get_u8(h, NVS_KEY_USB, &req) == ESP_OK) {
        /* el pedido vale para un solo arranque: si algo falla, el proximo vuelve al dashboard */
        nvs_erase_key(h, NVS_KEY_USB);
        nvs_commit(h);
    }
    nvs_close(h);
    return req != 0;
}

void usb_mode_request(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_USB, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Reiniciando en modo USB");
    esp_restart();
}

/* Misma configuracion que bsp_sdcard_mount(), pero sin montar FAT: la SD queda
 * para la compu y el firmware no la toca mientras dura el modo USB. */
static esp_err_t sd_card_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle), TAG, "LDO de la SD");
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    const sdmmc_slot_config_t slot_config = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };
    ESP_RETURN_ON_ERROR(host.init(), TAG, "host SDMMC");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot_config), TAG, "slot SDMMC");

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) {
        free(s_card);
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_attached = true;
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_attached = false;
    }
}

static esp_err_t usb_msc_start(void)
{
    const tinyusb_msc_driver_config_t driver_cfg = {
        /* la SD queda siempre del lado de la compu; no se monta en el firmware */
        .user_flags.auto_mount_off = 1,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_cfg), TAG, "driver MSC");

    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = s_card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 2,
            .do_not_format = true,  /* nunca formatear la tarjeta del usuario */
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&storage_cfg, NULL), TAG, "SD por USB");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    phy_give_to_otg();
    return tinyusb_driver_install(&tusb_cfg);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int last = -1;
    int now = s_attached ? 1 : 0;
    if (now == last) {
        return;
    }
    last = now;
    lv_label_set_text(s_status, now ? "Conectado a la compu" : "Esperando la compu...");
    lv_obj_set_style_text_color(s_status, lv_color_hex(now ? 0x30E070 : 0xFFB020), 0);
}

static void exit_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Saliendo del modo USB");
    tinyusb_driver_uninstall();
    usb_mode_restore_phy();
    esp_restart();
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}

void usb_mode_run(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(scr, 18, 0);

    add_label(scr, "MODO USB", &lv_font_montserrat_48, 0xFF8C00);

    esp_err_t err = sd_card_init();
    if (err == ESP_OK) {
        err = usb_msc_start();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el modo USB: %s", esp_err_to_name(err));
        usb_mode_restore_phy();
        add_label(scr, s_card ? "No se pudo iniciar el USB" : "No se encontro la tarjeta SD",
                  &lv_font_montserrat_26, 0xFF3030);
    } else {
        lv_obj_t *steps = add_label(scr,
            "1. Conecta un cable USB-C al puerto \"USB 1.1\" de la pantalla\n"
            "    (no al de UART) y a la compu.\n"
            "2. Aparece un disco con la SD: copia tus carpetas dentro de \"dashboards\".\n"
            "3. Expulsa el disco en la compu y despues toca SALIR.",
            &lv_font_montserrat_20, 0xFFFFFF);
        lv_obj_set_style_text_line_space(steps, 8, 0);
        s_status = add_label(scr, "", &lv_font_montserrat_26, 0xFFB020);
        lv_timer_create(status_timer_cb, 250, NULL);
        status_timer_cb(NULL);
    }

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x8A1C1C), 0);
    lv_obj_set_style_pad_hor(btn, 60, 0);
    lv_obj_set_style_pad_ver(btn, 18, 0);
    lv_obj_add_event_cb(btn, exit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = add_label(btn, "SALIR", &lv_font_montserrat_26, 0xFFFFFF);
    lv_obj_center(lbl);
}
