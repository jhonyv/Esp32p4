/* Menu de configuracion a pantalla completa.
 *
 *   CONFIGURACION
 *     Dashboard y valores    -> el menu de siempre (temas, mover valores, nombres)
 *     Sensores (con clave)   -> de donde lee cada valor, tipo de sensor, calibracion
 *     ECU (con clave)        -> leer la ECU original por OBD-II (CAN)
 *     Clave de instalador    -> cambiarla o desactivarla (de fabrica 0000)
 *     Modo USB               -> la SD como pendrive
 *     Restablecer de fabrica -> borra todo lo guardado y reinicia
 *
 * Se dibuja en una pantalla propia; el dashboard queda en pausa detras. */
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "sensors.h"
#include "usb_mode.h"
#include "ecu.h"

static const char *TAG = "settings";

#define NVS_NS        "ajustes"
#define BG            0x0E0E0E
#define PANEL         0x1C1C1C
#define ACCENT        0xFF8C00
#define TEXT_DIM      0x9A9A9A
#define DEFAULT_PIN   0

/* Hasta que dashboard.c tenga estas funciones */
__attribute__((weak)) void dashboard_set_paused(bool paused) { (void)paused; }
__attribute__((weak)) void dashboard_menu_open(void) { }

typedef enum { PIN_FOR_SENSORS, PIN_FOR_CHANGE, PIN_NEW, PIN_FOR_ECU } pin_purpose_t;

static lv_obj_t *s_scr;            /* pantalla del menu */
static lv_obj_t *s_prev_scr;       /* la del dashboard */
static lv_obj_t *s_page;           /* contenido de la pagina actual */
static lv_obj_t *s_kb;
static lv_timer_t *s_live_timer;
static bool s_unlocked;            /* ya puso la clave en esta apertura */
static bool s_dirty;               /* hay cambios de sensores sin guardar */
static sensor_setup_t s_edit[SENSOR_COUNT];
static ecu_config_t s_ecu_edit;
static bool s_ecu_dirty;
static lv_obj_t *s_ecu_status;
static lv_obj_t *s_ecu_values;
static int s_edit_id;
static char s_pin_buf[5];
static pin_purpose_t s_pin_purpose;
static lv_obj_t *s_pin_label;
static lv_obj_t *s_pin_msg;
static lv_obj_t *s_live_labels[SENSOR_COUNT];
static lv_obj_t *s_editor_live;

static void page_main(void);
static void page_sensors(void);
static void page_sensor_edit(int id);
static void page_pin(pin_purpose_t purpose);
static void page_pin_settings(void);
static void page_ecu(void);

/* ------------------------------------------------------------------------- */
/* Clave                                                                      */
/* ------------------------------------------------------------------------- */

static uint16_t pin_get(bool *enabled)
{
    uint16_t pin = DEFAULT_PIN;
    uint8_t on = 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "pin", &pin);
        nvs_get_u8(h, "pin_on", &on);
        nvs_close(h);
    }
    if (enabled) {
        *enabled = on != 0;
    }
    return pin;
}

static void pin_set(uint16_t pin, bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "pin", pin);
        nvs_set_u8(h, "pin_on", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------------------- */
/* Piezas comunes                                                             */
/* ------------------------------------------------------------------------- */

static void stop_live(void)
{
    if (s_live_timer) {
        lv_timer_delete(s_live_timer);
        s_live_timer = NULL;
    }
    memset(s_live_labels, 0, sizeof(s_live_labels));
    s_editor_live = NULL;
    s_ecu_status = s_ecu_values = NULL;
}

/* Borra la pagina anterior y crea una nueva con titulo y boton para volver */
static lv_obj_t *new_page(const char *title, lv_event_cb_t back_cb)
{
    stop_live();
    lv_obj_clean(s_scr);
    s_kb = NULL;
    s_pin_label = s_pin_msg = NULL;

    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(bar, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(bar, 2, 0);

    if (back_cb) {
        lv_obj_t *back = lv_button_create(bar);
        lv_obj_set_size(back, 120, 48);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(back);
        lv_label_set_text(l, LV_SYMBOL_LEFT " Volver");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }
    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_center(t);

    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), lv_obj_get_height(s_scr) - 64);
    lv_obj_set_y(s_page, 64);
    lv_obj_set_style_pad_all(s_page, 16, 0);
    lv_obj_set_style_pad_row(s_page, 10, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_page, LV_DIR_VER);
    return s_page;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Boton ancho de menu, con un texto chico abajo */
static lv_obj_t *menu_item(lv_obj_t *parent, const char *text, const char *hint, uint32_t color,
                           lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(btn, 14, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    label(btn, text, &lv_font_montserrat_24, 0xFFFFFF);
    if (hint) {
        label(btn, hint, &lv_font_montserrat_16, 0xC8C8C8);
    }
    return btn;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_pad_hor(btn, 22, 0);
    lv_obj_set_style_pad_ver(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = label(btn, text, &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Abrir / cerrar                                                             */
/* ------------------------------------------------------------------------- */

static void close_settings(void)
{
    stop_live();
    lv_obj_t *scr = s_scr;
    s_scr = NULL;
    lv_screen_load(s_prev_scr);
    lv_obj_delete(scr);
    dashboard_set_paused(false);
}

static void exit_cb(lv_event_t *e)
{
    (void)e;
    close_settings();
}

static void display_menu_cb(lv_event_t *e)
{
    (void)e;
    close_settings();
    dashboard_menu_open();
}

void settings_open(void)
{
    if (s_scr) {
        return;
    }
    dashboard_set_paused(true);
    s_prev_scr = lv_screen_active();
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_unlocked = false;
    s_dirty = false;
    s_ecu_dirty = false;
    ecu_get_config(&s_ecu_edit);
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensors_get_setup(i, &s_edit[i]);
    }
    lv_screen_load(s_scr);
    page_main();
}

bool settings_is_open(void)
{
    return s_scr != NULL;
}

/* ------------------------------------------------------------------------- */
/* Pagina principal                                                           */
/* ------------------------------------------------------------------------- */

static void sensors_cb(lv_event_t *e)
{
    (void)e;
    bool on;
    pin_get(&on);
    if (!on || s_unlocked) {
        page_sensors();
    } else {
        page_pin(PIN_FOR_SENSORS);
    }
}

static void ecu_cb(lv_event_t *e)
{
    (void)e;
    bool on;
    pin_get(&on);
    if (!on || s_unlocked) {
        page_ecu();
    } else {
        page_pin(PIN_FOR_ECU);
    }
}

static void pin_settings_cb(lv_event_t *e)
{
    (void)e;
    bool on;
    pin_get(&on);
    if (!on || s_unlocked) {
        page_pin_settings();
    } else {
        page_pin(PIN_FOR_CHANGE);
    }
}

static void usb_cb(lv_event_t *e)
{
    (void)e;
    usb_mode_request();
}

static void factory_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    if (!lv_obj_get_user_data(btn)) {
        /* Primer toque: pide confirmacion */
        lv_obj_set_user_data(btn, (void *)1);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xE02020), 0);
        lv_label_set_text(lv_obj_get_child(btn, 0), "Seguro? Toca de nuevo para borrar todo");
        return;
    }
    ESP_LOGW(TAG, "Restablecer de fabrica: se borra toda la configuracion");
    nvs_flash_deinit();
    nvs_flash_erase();
    esp_restart();
}

static void page_main(void)
{
    lv_obj_t *p = new_page("CONFIGURACION", exit_cb);
    bool pin_on;
    pin_get(&pin_on);
    menu_item(p, "Dashboard y valores", "Elegir dashboard, mover y mostrar valores, nombres",
              0x2A2A2A, display_menu_cb, NULL);
    menu_item(p, pin_on && !s_unlocked ? "Sensores  (con clave)" : "Sensores",
              "De donde lee cada valor, tipo de sensor y calibracion", 0x2A2A2A, sensors_cb, NULL);
    menu_item(p, pin_on && !s_unlocked ? "ECU  (con clave)" : "ECU",
              "Leer la computadora del auto por CAN: OBD-II o MegaSquirt", 0x2A2A2A, ecu_cb, NULL);
    menu_item(p, "Clave de instalador", pin_on ? "Activada" : "Desactivada", 0x2A2A2A, pin_settings_cb, NULL);
    menu_item(p, "Modo USB", "La SD aparece como pendrive en la compu (reinicia)", 0x2A2A2A, usb_cb, NULL);
    lv_obj_t *f = menu_item(p, "Restablecer de fabrica",
                            "Borra dashboards acomodados, sensores y clave, y reinicia",
                            0x6A1414, factory_cb, NULL);
    lv_obj_set_user_data(f, NULL);
}

/* ------------------------------------------------------------------------- */
/* Teclado de la clave                                                        */
/* ------------------------------------------------------------------------- */

static void pin_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_pin_purpose == PIN_NEW) {
        page_pin_settings();
    } else {
        page_main();
    }
}

static void pin_update_label(void)
{
    char shown[12] = "";
    size_t n = strlen(s_pin_buf);
    for (size_t i = 0; i < 4; i++) {
        strcat(shown, i < n ? "* " : "_ ");
    }
    lv_label_set_text(s_pin_label, shown);
}

static void pin_key_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    uint32_t idx = lv_buttonmatrix_get_selected_button(m);
    const char *txt = lv_buttonmatrix_get_button_text(m, idx);
    if (!txt) {
        return;
    }
    size_t n = strlen(s_pin_buf);
    if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) {
        if (n) s_pin_buf[n - 1] = 0;
    } else if (txt[0] >= '0' && txt[0] <= '9' && n < 4) {
        s_pin_buf[n] = txt[0];
        s_pin_buf[n + 1] = 0;
    }
    pin_update_label();
    if (strlen(s_pin_buf) < 4) {
        return;
    }
    uint16_t typed = (uint16_t)atoi(s_pin_buf);
    bool on;
    uint16_t pin = pin_get(&on);
    if (s_pin_purpose == PIN_NEW) {
        pin_set(typed, true);
        page_pin_settings();
        return;
    }
    if (typed == pin) {
        s_unlocked = true;
        if (s_pin_purpose == PIN_FOR_SENSORS) {
            page_sensors();
        } else if (s_pin_purpose == PIN_FOR_ECU) {
            page_ecu();
        } else {
            page_pin_settings();
        }
        return;
    }
    s_pin_buf[0] = 0;
    pin_update_label();
    lv_label_set_text(s_pin_msg, "Clave incorrecta");
}

static void page_pin(pin_purpose_t purpose)
{
    s_pin_purpose = purpose;
    s_pin_buf[0] = 0;
    lv_obj_t *p = new_page(purpose == PIN_NEW ? "CLAVE NUEVA" : "CLAVE DE INSTALADOR", pin_back_cb);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    label(p, purpose == PIN_NEW ? "Escribi la clave nueva (4 numeros)" : "Escribi la clave (4 numeros)",
          &lv_font_montserrat_20, TEXT_DIM);
    s_pin_label = label(p, "", &lv_font_montserrat_48, 0xFFFFFF);
    pin_update_label();
    s_pin_msg = label(p, "", &lv_font_montserrat_20, 0xFF4040);

    static const char *map[] = { "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                 " ", "0", LV_SYMBOL_BACKSPACE, "" };
    lv_obj_t *m = lv_buttonmatrix_create(p);
    lv_buttonmatrix_set_map(m, map);
    lv_buttonmatrix_set_button_ctrl(m, 9, LV_BUTTONMATRIX_CTRL_DISABLED);   /* el " " */
    lv_obj_set_size(m, 420, 340);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_26, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(BG), 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_add_event_cb(m, pin_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ------------------------------------------------------------------------- */
/* Ajustes de la clave                                                        */
/* ------------------------------------------------------------------------- */

static void pin_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on;
    uint16_t pin = pin_get(&on);
    pin_set(pin, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void pin_change_cb(lv_event_t *e)
{
    (void)e;
    page_pin(PIN_NEW);
}

static void back_main_cb(lv_event_t *e)
{
    (void)e;
    page_main();
}

static void page_pin_settings(void)
{
    lv_obj_t *p = new_page("CLAVE DE INSTALADOR", back_main_cb);
    bool on;
    pin_get(&on);
    label(p, "La clave protege la configuracion de sensores, para que el cliente\n"
             "no la cambie por error. De fabrica es 0000.", &lv_font_montserrat_20, TEXT_DIM);
    lv_obj_t *r = row(p);
    label(r, "Pedir clave", &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 80, 40);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, pin_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    small_button(p, "Cambiar clave", 0x0078A0, pin_change_cb, NULL);
}

/* ------------------------------------------------------------------------- */
/* Sensores: lista                                                            */
/* ------------------------------------------------------------------------- */

static bool s_advanced;            /* el editor muestra los campos tecnicos */

/* Texto chico que ocupa el ancho y se corta en renglones */
static lv_obj_t *note(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *l = label(parent, text, &lv_font_montserrat_16, color);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

static bool is_pulse_sensor(int id)
{
    return id == SENSOR_RPM || id == SENSOR_SPEED;
}

/* Motores para la lista de RPM: pulsos por vuelta en un 4 tiempos con una chispa por
 * cilindro (distribuidor, o la salida de tacometro de la ECU / CDI). */
static const struct {
    const char *label;
    float pulses;
} s_engines[] = {
    { "4 cilindros / rotativo 2 rotores", 2 },
    { "6 cilindros / rotativo 3 rotores", 3 },
    { "8 cilindros", 4 },
    { "3 cilindros", 1.5f },
    { "5 cilindros", 2.5f },
    { "2 cilindros", 1 },
    { "1 cilindro", 0.5f },
    { "10 cilindros", 5 },
    { "12 cilindros", 6 },
};
#define ENGINE_COUNT ((int)(sizeof(s_engines) / sizeof(s_engines[0])))

static int engine_index(float pulses)
{
    for (int i = 0; i < ENGINE_COUNT; i++) {
        if (fabsf(s_engines[i].pulses - pulses) < 0.01f) {
            return i;
        }
    }
    return -1;
}

static const char *type_name(int id, int type)
{
    if (type == SENSOR_TYPE_LINEAL && id == SENSOR_TPS) return "Potenciometro (se calibra con el pedal)";
    if (type == SENSOR_TYPE_LINEAL) return "Otro sensor (cargar dos puntos)";
    if (type == SENSOR_TYPE_NTC) return "Otra NTC (R a 25 C y beta)";
    return sensors_type_label(type);
}

static void format_value(char *buf, size_t len, sensor_id_t id, float v)
{
    const sensor_info_t *info = sensors_info(id);
    snprintf(buf, len, "%.*f %s", info->decimals, v, info->unit);
}

/* Lo que esta leyendo ahora (con la configuracion con la que arranco) */
static void live_text(char *buf, size_t len, sensor_id_t id)
{
    sensor_source_t src = sensors_source(id);
    char val[32];
    format_value(val, sizeof(val), id, sensors_get(id));
    float volts = sensors_volts(id);
    if (sensors_problem(id)) {
        snprintf(buf, len, "sin lectura");
    } else if (src == SENSOR_SRC_OFF) {
        snprintf(buf, len, "-");
    } else if (src == SENSOR_SRC_ANALOG && isnan(volts)) {
        snprintf(buf, len, "esperando...");
    } else if (src == SENSOR_SRC_ANALOG) {
        snprintf(buf, len, "%s  (%.2f V)", val, volts);
    } else {
        snprintf(buf, len, "%s", val);
    }
}

static void live_timer_cb(lv_timer_t *t)
{
    (void)t;
    char buf[48];
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (s_live_labels[i]) {
            live_text(buf, sizeof(buf), i);
            lv_label_set_text(s_live_labels[i], buf);
        }
    }
    if (s_editor_live) {
        live_text(buf, sizeof(buf), s_edit_id);
        const char *problem = sensors_problem(s_edit_id);
        if (problem) {
            lv_label_set_text_fmt(s_editor_live, "Ahora: sin lectura, %s", problem);
        } else {
            lv_label_set_text_fmt(s_editor_live, "Ahora: %s", buf);
        }
    }
}

/* Donde esta conectado: el borne de la placa, o el pin si es un cableado a medida */
static void where_text(char *buf, size_t len, const sensor_setup_t *s, sensor_id_t id)
{
    const sensor_input_t *in = sensors_board_input(id);
    if (in && sensors_uses_board_input(id, s)) {
        snprintf(buf, len, "borne %s", in->name);
    } else {
        char pin[24];
        sensors_pin_name(s->gpio, pin, sizeof(pin));
        snprintf(buf, len, "a medida en %s", pin);
    }
}

static void summary(char *buf, size_t len, const sensor_setup_t *s, sensor_id_t id)
{
    char where[40];
    switch (s->src) {
    case SENSOR_SRC_ANALOG:
        where_text(where, sizeof(where), s, id);
        snprintf(buf, len, "Sensor, %s - %s", where, type_name(id, s->type));
        break;
    case SENSOR_SRC_PULSE: {
        where_text(where, sizeof(where), s, id);
        int e = engine_index(s->pulses);
        if (id == SENSOR_RPM && e >= 0) {
            snprintf(buf, len, "Sensor, %s - %s", where, s_engines[e].label);
        } else {
            snprintf(buf, len, "Sensor, %s - %g pulsos por %s", where, s->pulses,
                     id == SENSOR_SPEED ? "km" : "vuelta");
        }
        break;
    }
    case SENSOR_SRC_ECU: {
        ecu_status_t st;
        ecu_get_status(&st);
        snprintf(buf, len, "ECU%s",
                 st.state == ECU_STATE_CONNECTED && !ecu_supports(id) ? " - la ECU no lo informa" : "");
        break;
    }
    case SENSOR_SRC_SIM:
        snprintf(buf, len, "Simulado (demo)");
        break;
    default:
        snprintf(buf, len, "Apagado");
        break;
    }
}

static void sensor_row_cb(lv_event_t *e)
{
    s_advanced = false;
    page_sensor_edit((int)(intptr_t)lv_event_get_user_data(e));
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (s_ecu_dirty) {
        ecu_save_config(&s_ecu_edit);
    }
    if (sensors_save_setup(s_edit) == ESP_OK) {
        esp_restart();   /* la configuracion nueva se aplica al arrancar */
    }
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensors_get_setup(i, &s_edit[i]);
    }
    s_dirty = false;
    page_main();
}

static void page_sensors(void)
{
    lv_obj_t *p = new_page("SENSORES", s_dirty ? cancel_cb : back_main_cb);
    note(p, "Toca un valor para elegir de donde se lee y que sensor tenes. Los cambios se aplican "
            "al guardar (reinicia la pantalla).", TEXT_DIM);
    for (int i = 0; i < SENSOR_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(p);
        lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_add_event_cb(btn, sensor_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *name = label(btn, sensors_info(i)->name, &lv_font_montserrat_22, 0xFFFFFF);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
        char buf[96];
        summary(buf, sizeof(buf), &s_edit[i], i);
        lv_obj_t *sum = label(btn, buf, &lv_font_montserrat_16,
                              s_edit[i].src == SENSOR_SRC_OFF ? TEXT_DIM : ACCENT);
        lv_obj_align(sum, LV_ALIGN_TOP_LEFT, 0, 30);
        s_live_labels[i] = label(btn, "", &lv_font_montserrat_22, 0xFFFFFF);
        lv_obj_align(s_live_labels[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_t *r = row(p);
    small_button(r, LV_SYMBOL_SAVE " Guardar y reiniciar", s_dirty ? 0x1E7A1E : 0x3A3A3A, save_cb, NULL);
    if (s_dirty) {
        small_button(r, "Descartar cambios", 0x6A1414, cancel_cb, NULL);
    }
    s_live_timer = lv_timer_create(live_timer_cb, 250, NULL);
    live_timer_cb(NULL);
}

/* ------------------------------------------------------------------------- */
/* Sensores: editor de uno                                                    */
/* ------------------------------------------------------------------------- */

/* Lo que ofrece cada lista (el indice del dropdown -> el valor de verdad) */
static uint8_t s_src_options[4];
static int s_pin_options[40];
static int s_pin_option_count;
static uint8_t s_type_options[32];

static void editor_back_cb(lv_event_t *e)
{
    (void)e;
    page_sensors();
}

static void rebuild_async(void *arg)
{
    (void)arg;
    if (s_scr) {
        page_sensor_edit(s_edit_id);
    }
}

static void rebuild(void)
{
    lv_async_call(rebuild_async, NULL);   /* cambian los campos que se muestran */
}

static void src_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    sensor_setup_t *s = &s_edit[s_edit_id];
    s->src = s_src_options[lv_dropdown_get_selected(dd)];
    /* Sensor en la placa: si el pin que tenia no sirve, va a su entrada fija */
    if ((s->src == SENSOR_SRC_ANALOG || s->src == SENSOR_SRC_PULSE) &&
        !sensors_gpio_ok(s->gpio, s->src == SENSOR_SRC_ANALOG)) {
        sensors_apply_board_input(s_edit_id, s);
    }
    s_dirty = true;
    rebuild();
}

static void pin_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_edit[s_edit_id].gpio = (int8_t)s_pin_options[lv_dropdown_get_selected(dd)];
    s_dirty = true;
    rebuild();
}

static void board_input_cb(lv_event_t *e)
{
    (void)e;
    sensors_apply_board_input(s_edit_id, &s_edit[s_edit_id]);
    s_dirty = true;
    rebuild();
}

static void advanced_cb(lv_event_t *e)
{
    (void)e;
    s_advanced = !s_advanced;
    rebuild();
}

static void type_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_edit[s_edit_id].type = s_type_options[lv_dropdown_get_selected(dd)];
    s_dirty = true;
    rebuild();
}

static void engine_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel < ENGINE_COUNT) {
        s_edit[s_edit_id].pulses = s_engines[sel].pulses;
        s_dirty = true;
    }
}

static void edge_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_edit[s_edit_id].edge_rising = lv_dropdown_get_selected(dd) == 1;
    s_dirty = true;
}

/* Acelerador: toma el voltaje de ahora como "suelto" (0 %) o "a fondo" (100 %) */
static void tps_capture_cb(lv_event_t *e)
{
    bool full = (intptr_t)lv_event_get_user_data(e) != 0;
    float v = sensors_volts(SENSOR_TPS);
    if (isnan(v)) {
        return;
    }
    sensor_setup_t *s = &s_edit[SENSOR_TPS];
    if (full) {
        s->v2 = roundf(v * 100) / 100;
        s->val2 = 100;
    } else {
        s->v1 = roundf(v * 100) / 100;
        s->val1 = 0;
    }
    s_dirty = true;
    rebuild();
}

static void number_changed_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    float *ptr = lv_event_get_user_data(e);
    const char *txt = lv_textarea_get_text(ta);
    if (txt && txt[0]) {
        *ptr = strtof(txt, NULL);
        s_dirty = true;
    }
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_page, lv_obj_get_height(s_scr) - 64);
    }
}

static void number_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!s_kb) {
        return;
    }
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    /* La pagina se achica para que el campo quede a la vista arriba del teclado */
    lv_obj_set_height(s_page, lv_obj_get_height(s_scr) - 64 - lv_obj_get_height(s_kb));
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static lv_obj_t *add_number(lv_obj_t *parent, const char *name, float *ptr)
{
    lv_obj_t *r = row(parent);
    lv_obj_t *l = label(r, name, &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_set_width(l, 340);
    lv_obj_t *ta = lv_textarea_create(r);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789.-");
    lv_textarea_set_max_length(ta, 10);
    char buf[24];
    snprintf(buf, sizeof(buf), "%g", *ptr);
    lv_textarea_set_text(ta, buf);
    lv_obj_set_width(ta, 200);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_22, 0);
    lv_obj_add_event_cb(ta, number_changed_cb, LV_EVENT_VALUE_CHANGED, ptr);
    lv_obj_add_event_cb(ta, number_focus_cb, LV_EVENT_FOCUSED, NULL);
    return r;
}

static lv_obj_t *add_dropdown(lv_obj_t *parent, const char *name, const char *options, uint32_t sel,
                              lv_event_cb_t cb)
{
    lv_obj_t *r = row(parent);
    lv_obj_t *l = label(r, name, &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_set_width(l, 340);
    lv_obj_t *dd = lv_dropdown_create(r);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_width(dd, 480);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_20, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_20, 0);
    lv_obj_set_style_max_height(list, 360, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

/* Fuente: Apagado / Sensor en la placa / ECU / Simulado */
static void add_source(lv_obj_t *p, int id, sensor_setup_t *s)
{
    uint8_t sensor_src = is_pulse_sensor(id) ? SENSOR_SRC_PULSE : SENSOR_SRC_ANALOG;
    const uint8_t order[4] = { SENSOR_SRC_OFF, sensor_src, SENSOR_SRC_ECU, SENSOR_SRC_SIM };
    if (s->src != SENSOR_SRC_OFF && s->src != sensor_src && s->src != SENSOR_SRC_ECU &&
        s->src != SENSOR_SRC_SIM) {
        s->src = SENSOR_SRC_OFF;       /* por ejemplo "pulsos" en un sensor analogico */
    }
    uint32_t sel = 0;
    for (int i = 0; i < 4; i++) {
        s_src_options[i] = order[i];
        if (order[i] == s->src) {
            sel = i;
        }
    }
    add_dropdown(p, "Se lee de", "Apagado\nUn sensor en la placa\nLa ECU (CAN / OBD-II)\nSimulado (demo)",
                 sel, src_cb);
}

/* Donde va conectado. En modo simple solo lo dice; en Avanzado se puede cambiar. */
static void add_wiring(lv_obj_t *p, int id, sensor_setup_t *s)
{
    bool analog = s->src == SENSOR_SRC_ANALOG;
    const sensor_input_t *in = sensors_board_input(id);
    bool board = in && sensors_uses_board_input(id, s);
    char buf[160], pin[24];
    sensors_pin_name(s->gpio, pin, sizeof(pin));
    if (board) {
        snprintf(buf, sizeof(buf), "Se conecta en el borne %s de la placa (%s).", in->name, pin);
        note(p, buf, 0xFFFFFF);
    } else {
        bool ntc = s->type == SENSOR_TYPE_NTC || strstr(sensors_type_label(s->type), "NTC") != NULL;
        if (s->src == SENSOR_SRC_PULSE) {
            snprintf(buf, sizeof(buf), "Cableado a medida: %s, flanco de %s.", pin,
                     s->edge_rising ? "subida" : "bajada");
        } else if (ntc) {
            snprintf(buf, sizeof(buf), "Cableado a medida: %s, divisor %g, pull-up de %g ohms a %g V.", pin,
                     s->divisor, s->pullup, s->vpullup);
        } else {
            snprintf(buf, sizeof(buf), "Cableado a medida: %s, divisor %g.", pin, s->divisor);
        }
        note(p, buf, ACCENT);
        if (in) {
            lv_obj_t *r = row(p);
            snprintf(buf, sizeof(buf), "Usar el borne %s de la placa", in->name);
            small_button(r, buf, 0x0078A0, board_input_cb, NULL);
        }
    }
    if (!s_advanced) {
        return;
    }
    /* Todas las entradas que sirven, con el nombre del borne si es de la placa */
    char *opts = malloc(40 * 32);
    opts[0] = 0;
    s_pin_option_count = 0;
    uint32_t sel = 0;
    for (int g = 0; g < 108 && s_pin_option_count < 40; g++) {
        if (!sensors_gpio_ok(g, analog)) {
            continue;
        }
        const char *board_name = NULL;
        for (int k = 0; k < SENSOR_COUNT; k++) {
            const sensor_input_t *bi = sensors_board_input(k);
            if (bi && bi->pin == g) {
                board_name = bi->name;
            }
        }
        if (!board_name && g == SENSOR_PIN_ADS(1, 3)) {
            board_name = "EXTRA";
        }
        if (g == s->gpio) {
            sel = s_pin_option_count;
        }
        s_pin_options[s_pin_option_count++] = g;
        sensors_pin_name(g, pin, sizeof(pin));
        sprintf(opts + strlen(opts), "%s%s%s%s%s", opts[0] ? "\n" : "", pin,
                board_name ? " (" : "", board_name ? board_name : "", board_name ? ")" : "");
    }
    if (s_pin_option_count) {
        add_dropdown(p, "Entrada", opts, sel, pin_cb);
    }
    free(opts);
}

static void add_type_list(lv_obj_t *p, int id, sensor_setup_t *s)
{
    char *opts = malloc(1600);
    opts[0] = 0;
    int n = 0;
    uint32_t sel = 0;
    for (int t = 0; t < sensors_type_count() && n < (int)sizeof(s_type_options); t++) {
        /* La lista corta solo tiene los que sirven para este valor (y el que ya estaba) */
        if (!s_advanced && !sensors_type_fits(id, t) && t != s->type) {
            continue;
        }
        if (t == s->type) {
            sel = n;
        }
        s_type_options[n++] = t;
        if (opts[0]) strcat(opts, "\n");
        strcat(opts, type_name(id, t));
    }
    add_dropdown(p, "Que sensor tenes", opts, sel, type_cb);
    free(opts);
}

static void add_analog(lv_obj_t *p, int id, sensor_setup_t *s)
{
    const sensor_info_t *info = sensors_info(id);
    add_type_list(p, id, s);
    bool ntc = s->type == SENSOR_TYPE_NTC || strstr(sensors_type_label(s->type), "NTC") != NULL;

    if (s->type == SENSOR_TYPE_LINEAL && id == SENSOR_TPS) {
        add_number(p, "Voltaje sin acelerar (V)", &s->v1);
        add_number(p, "Voltaje a fondo (V)", &s->v2);
        s->val1 = 0;
        s->val2 = 100;
        if (sensors_source(SENSOR_TPS) == SENSOR_SRC_ANALOG) {
            lv_obj_t *r = row(p);
            small_button(r, "Tomar: pie suelto", 0x0078A0, tps_capture_cb, (void *)0);
            small_button(r, "Tomar: a fondo", 0x0078A0, tps_capture_cb, (void *)1);
            note(p, "Con el motor apagado y el contacto puesto: pie suelto, tocar el primero; "
                    "pisar a fondo, tocar el segundo.", TEXT_DIM);
        } else {
            note(p, "Para calibrarlo con el pedal: guardar, volver a entrar y usar los botones \"Tomar\".",
                 TEXT_DIM);
        }
    } else if (s->type == SENSOR_TYPE_LINEAL) {
        char name[48];
        note(p, "Sacalo de la hoja del sensor: dos voltajes y lo que marca con cada uno.", TEXT_DIM);
        add_number(p, "Voltaje 1 (V)", &s->v1);
        snprintf(name, sizeof(name), "Valor con voltaje 1 (%s)", info->unit);
        add_number(p, name, &s->val1);
        add_number(p, "Voltaje 2 (V)", &s->v2);
        snprintf(name, sizeof(name), "Valor con voltaje 2 (%s)", info->unit);
        add_number(p, name, &s->val2);
    }
    if (s->type == SENSOR_TYPE_NTC) {
        add_number(p, "R a 25 C (ohms)", &s->r25);
        add_number(p, "Beta", &s->beta);
    }
    if (s_advanced) {
        add_number(p, "Divisor de la entrada", &s->divisor);
        if (ntc) {
            add_number(p, "Resistencia pull-up (ohms)", &s->pullup);
            add_number(p, "Pull-up conectada a (V)", &s->vpullup);
        }
        add_number(p, "Filtro (1 = sin filtro)", &s->filter);
        note(p, "Divisor: voltaje en el sensor / voltaje en el pin. Bornes de 5 V de la placa: 1.5 "
                "(10k / 20k). Bateria: 11 (100k / 10k). NTC: 1, con pull-up de 1k a 3,3 V.", TEXT_DIM);
    }
}

static void add_pulses(lv_obj_t *p, int id, sensor_setup_t *s)
{
    if (id == SENSOR_RPM) {
        int e = engine_index(s->pulses);
        char *opts = malloc(512);
        opts[0] = 0;
        for (int i = 0; i < ENGINE_COUNT; i++) {
            if (i) strcat(opts, "\n");
            strcat(opts, s_engines[i].label);
        }
        if (e < 0) {
            sprintf(opts + strlen(opts), "\nOtro: %g pulsos por vuelta", s->pulses);
            e = ENGINE_COUNT;
        }
        add_dropdown(p, "Motor", opts, e, engine_cb);
        free(opts);
        note(p, "Motor de 4 tiempos con distribuidor, o la salida de tacometro de la ECU / CDI. "
                "Si marca el doble o la mitad, en Avanzado cambia los pulsos por vuelta. "
                "Nunca conectar la bobina directo: siempre por el borne RPM de la placa.", TEXT_DIM);
    } else {
        add_number(p, "Pulsos por km", &s->pulses);
        note(p, "Si marca de mas o de menos: pulsos nuevos = pulsos x velocidad que marca / velocidad real "
                "(con GPS o el velocimetro).", TEXT_DIM);
    }
    if (s_advanced) {
        if (id == SENSOR_RPM) {
            add_number(p, "Pulsos por vuelta", &s->pulses);
        }
        add_dropdown(p, "Flanco", "Bajada (optoacoplador de la placa)\nSubida", s->edge_rising ? 1 : 0, edge_cb);
    }
}

static void page_sensor_edit(int id)
{
    s_edit_id = id;
    sensor_setup_t *s = &s_edit[id];
    const sensor_info_t *info = sensors_info(id);
    char title[48];
    snprintf(title, sizeof(title), "SENSOR: %s", info->name);
    lv_obj_t *p = new_page(title, editor_back_cb);

    s_editor_live = label(p, "", &lv_font_montserrat_20, ACCENT);
    lv_obj_set_width(s_editor_live, LV_PCT(100));
    note(p, "(\"Ahora\" es con la configuracion guardada; lo nuevo se ve al guardar)", TEXT_DIM);

    add_source(p, id, s);

    if (s->src == SENSOR_SRC_ANALOG || s->src == SENSOR_SRC_PULSE) {
        add_wiring(p, id, s);
        if (s->src == SENSOR_SRC_ANALOG) {
            add_analog(p, id, s);
        } else {
            add_pulses(p, id, s);
        }
    } else if (s->src == SENSOR_SRC_ECU) {
        note(p, "Se configura en Configuracion > ECU. Si la ECU no informa este valor, conviene "
                "leerlo de un sensor en la placa.", TEXT_DIM);
    } else if (s->src == SENSOR_SRC_SIM) {
        note(p, "Valores inventados que se mueven solos, para mostrar la pantalla sin auto.", TEXT_DIM);
    }

    lv_obj_t *r = row(p);
    small_button(r, "Listo", 0x0078A0, editor_back_cb, NULL);
    if (s->src == SENSOR_SRC_ANALOG || s->src == SENSOR_SRC_PULSE) {
        small_button(r, s_advanced ? "Ocultar avanzado" : "Avanzado (instaladores)", 0x3A3A3A, advanced_cb, NULL);
    }

    s_kb = lv_keyboard_create(s_scr);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_height(s_kb, 260);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);

    s_live_timer = lv_timer_create(live_timer_cb, 250, NULL);
    live_timer_cb(NULL);
}

/* ------------------------------------------------------------------------- */
/* ECU original (OBD-II por CAN)                                              */
/* ------------------------------------------------------------------------- */

static int s_can_pins[56];
static int s_can_pin_count;

static void ecu_rebuild_async(void *arg)
{
    (void)arg;
    if (s_scr) {
        page_ecu();
    }
}

static void ecu_live_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ecu_status) {
        return;
    }
    ecu_status_t st;
    ecu_get_status(&st);
    switch (st.state) {
    case ECU_STATE_OFF:
        lv_label_set_text(s_ecu_status, "Apagada (se activa al guardar y reiniciar)");
        break;
    case ECU_STATE_SEARCHING:
        lv_label_set_text(s_ecu_status, s_ecu_edit.mode == ECU_MODE_MEGASQUIRT
                          ? "Esperando datos de la MegaSquirt...  (broadcast activado? CAN-H / CAN-L?)"
                          : "Buscando la ECU...  (contacto puesto? cables CAN-H / CAN-L?)");
        break;
    case ECU_STATE_ERROR:
        lv_label_set_text(s_ecu_status, "No se pudo iniciar el CAN: revisa los pines TX / RX");
        break;
    case ECU_STATE_CONNECTED:
        if (st.ms_base) {
            lv_label_set_text_fmt(s_ecu_status, "MegaSquirt conectada (%s, ID base %u): %d mensajes/s",
                                  st.ms_format == MS_FORMAT_DASH ? "dash simplificado" : "broadcast avanzado",
                                  st.ms_base, st.responses_per_s);
        } else {
            lv_label_set_text_fmt(s_ecu_status, "Conectada: %lu kbit/s, IDs de %d bits, informa %d datos, %d respuestas/s",
                                  (unsigned long)(st.bitrate / 1000), st.ext_id ? 29 : 11, st.pid_count,
                                  st.responses_per_s);
        }
        break;
    }
    /* Lo que va mandando, sensor por sensor */
    char buf[512] = "";
    for (int i = 0; i < SENSOR_COUNT; i++) {
        float v;
        if (!ecu_get_value(i, &v)) {
            continue;
        }
        char val[32], line[80];
        format_value(val, sizeof(val), i, v);
        snprintf(line, sizeof(line), "%s: %s%s\n", sensors_info(i)->name, val,
                 s_edit[i].src == SENSOR_SRC_ECU ? "" : "  (no se usa)");
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }
    lv_label_set_text(s_ecu_values, buf[0] ? buf : "Todavia no llegaron datos de la ECU.");
}

static void ecu_mode_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_ecu_edit.mode = (uint8_t)lv_dropdown_get_selected(dd);
    s_ecu_dirty = true;
    lv_async_call(ecu_rebuild_async, NULL);   /* cambian las instrucciones */
}

static void ecu_pin_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    bool is_tx = (intptr_t)lv_event_get_user_data(e) == 0;
    uint32_t sel = lv_dropdown_get_selected(dd);
    int8_t g = sel == 0 ? -1 : (int8_t)s_can_pins[sel - 1];
    if (is_tx) {
        s_ecu_edit.tx_gpio = g;
    } else {
        s_ecu_edit.rx_gpio = g;
    }
    s_ecu_dirty = true;
}

/* Todo lo que la ECU informa y esta apagado o simulado pasa a leerse de la ECU */
static void ecu_auto_cb(lv_event_t *e)
{
    int n = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (ecu_supports(i) && (s_edit[i].src == SENSOR_SRC_OFF || s_edit[i].src == SENSOR_SRC_SIM)) {
            s_edit[i].src = SENSOR_SRC_ECU;
            n++;
        }
    }
    lv_obj_t *btn = lv_event_get_current_target(e);
    lv_label_set_text_fmt(lv_obj_get_child(btn, 0), n ? "%d valores pasan a la ECU (falta guardar)"
                                                      : "No hay valores nuevos para pasar", n);
    if (n) {
        s_dirty = true;
    }
}

static void ecu_save_cb(lv_event_t *e)
{
    (void)e;
    if (s_ecu_edit.mode != ECU_MODE_OFF &&
        (s_ecu_edit.tx_gpio < 0 || s_ecu_edit.rx_gpio < 0 || s_ecu_edit.tx_gpio == s_ecu_edit.rx_gpio)) {
        lv_label_set_text(s_ecu_status, "Elegi dos pines distintos para TX y RX");
        return;
    }
    bool ok = ecu_save_config(&s_ecu_edit) == ESP_OK;
    if (ok && s_dirty) {
        ok = sensors_save_setup(s_edit) == ESP_OK;
    }
    if (ok) {
        esp_restart();
    }
}

static void ecu_back_cb(lv_event_t *e)
{
    (void)e;
    page_main();   /* lo que no se guardo queda pendiente hasta salir */
}

static lv_obj_t *ecu_pin_dropdown(lv_obj_t *parent, const char *name, int current, intptr_t which)
{
    char *opts = malloc(56 * 32 + 16);
    strcpy(opts, "(elegir)");
    uint32_t sel = 0;
    for (int i = 0; i < s_can_pin_count; i++) {
        if (s_can_pins[i] == current) {
            sel = i + 1;
        }
        int g = s_can_pins[i];
        sprintf(opts + strlen(opts), "\nIO%d%s", g,
                g == ECU_CAN_TX_DEFAULT ? " (CAN TX de la placa)" :
                g == ECU_CAN_RX_DEFAULT ? " (CAN RX de la placa)" : "");
    }
    lv_obj_t *dd = add_dropdown(parent, name, opts, sel, ecu_pin_cb);
    free(opts);
    /* add_dropdown registra el callback sin dato: se reemplaza para saber si es TX o RX */
    lv_obj_remove_event_cb(dd, ecu_pin_cb);
    lv_obj_add_event_cb(dd, ecu_pin_cb, LV_EVENT_VALUE_CHANGED, (void *)which);
    return dd;
}

static void page_ecu(void)
{
    lv_obj_t *p = new_page("ECU", ecu_back_cb);
    s_can_pin_count = 0;
    for (int g = 0; g <= 54; g++) {
        /* Para el CAN mejor no gastar pines con ADC (quedan para sensores) */
        if (sensors_gpio_ok(g, false) && !sensors_gpio_ok(g, true)) {
            s_can_pins[s_can_pin_count++] = g;
        }
    }

    s_ecu_status = label(p, "", &lv_font_montserrat_20, ACCENT);

    add_dropdown(p, "Leer la ECU", "Apagado\nOBD-II (autos de fabrica)\nMegaSquirt (MS2 / MS3 / MicroSquirt)",
                 s_ecu_edit.mode, ecu_mode_cb);

    ecu_pin_dropdown(p, "Pin TX (al transceptor)", s_ecu_edit.tx_gpio, 0);
    ecu_pin_dropdown(p, "Pin RX (del transceptor)", s_ecu_edit.rx_gpio, 1);

    if (s_ecu_edit.mode == ECU_MODE_MEGASQUIRT) {
        label(p, "Transceptor CAN de 3,3 V (SN65HVD230): CTX a IO46 y CRX a IO47 (ficha CAN de la placa),\n"
                 "CANH / CANL a los cables CAN-H / CAN-L de la MegaSquirt.\n"
                 "En TunerStudio (firmware MS2/Extra 3.4 o mas nuevo), CAN-bus / Testmodes:\n"
                 " - CAN Parameters: velocidad 500K.\n"
                 " - Dash broadcasting = ON (ID base 1512), o bien Realtime data broadcasting = ON\n"
                 "   con los grupos 0, 2, 3 (y 22 EGT, 31 AFR, 42 velocidad, 53 presion de nafta).\n"
                 "Grabar (Burn) y reiniciar la ECU. El formato y el ID base se detectan solos.",
              &lv_font_montserrat_14, TEXT_DIM);
    } else {
        label(p, "Conexion: transceptor CAN de 3,3 V (SN65HVD230), CTX a IO46 y CRX a IO47 (ficha CAN de la placa).\n"
                 "Al conector OBD: pin 6 = CAN-H, pin 14 = CAN-L, pin 4/5 = masa. La velocidad del bus se detecta sola.",
              &lv_font_montserrat_14, TEXT_DIM);
    }

    label(p, "Datos que manda la ECU:", &lv_font_montserrat_20, 0xFFFFFF);
    s_ecu_values = label(p, "", &lv_font_montserrat_20, 0xFFFFFF);

    lv_obj_t *r2 = row(p);
    small_button(r2, "Usar la ECU para todo lo que informa", 0x0078A0, ecu_auto_cb, NULL);
    small_button(r2, LV_SYMBOL_SAVE " Guardar y reiniciar", 0x1E7A1E, ecu_save_cb, NULL);
    label(p, "Lo que la ECU no informa (por ejemplo presion de aceite) se sigue leyendo de los sensores agregados.",
          &lv_font_montserrat_14, TEXT_DIM);

    s_live_timer = lv_timer_create(ecu_live_cb, 500, NULL);
    ecu_live_cb(NULL);
}
