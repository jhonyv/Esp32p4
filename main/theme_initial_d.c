/* Tema "initial d" (imagenes de un mod de Assetto Corsa).
 * Pantalla de 1024x600. Las imagenes ya vienen escaladas desde png2lvgl.py:
 * reloj RPM x2, reloj turbo x1.5, digitos x1.3. */
#include "dashboard.h"
#include "assets/assets.h"

static const lv_image_dsc_t *const s_digits[10] = {
    &img_digit_0, &img_digit_1, &img_digit_2, &img_digit_3, &img_digit_4,
    &img_digit_5, &img_digit_6, &img_digit_7, &img_digit_8, &img_digit_9,
};

/* Luces de cambio: 11 cuadros (0 = apagadas ... 10 = todas en rojo) */
static const lv_image_dsc_t *const s_shift[11] = {
    &img_shift_00, &img_shift_01, &img_shift_02, &img_shift_03, &img_shift_04, &img_shift_05,
    &img_shift_06, &img_shift_07, &img_shift_08, &img_shift_09, &img_shift_10,
};

#define RPM_X   272
#define RPM_Y   20
#define TURBO_X 700
#define TURBO_Y 40

static const dash_element_t s_elements[] = {
    /* Reloj de RPM al centro */
    { .type = DASH_EL_IMAGE, .x = RPM_X, .y = RPM_Y, .img = &img_rpm_gauge },
    {
        .type = DASH_EL_NEEDLE, .sensor = SENSOR_RPM,
        .x = RPM_X + 241, .y = RPM_Y + 229,
        .img = &img_rpm_needle, .pivot_x = 15, .pivot_y = 200,
        /* 0 rpm a -128 grados, 8000 rpm a +128 grados */
        .cal = { { 0, -128 }, { 8000, 128 } }, .cal_count = 2,
        .smoothing = 0.45f,
    },

    /* Reloj de turbo a la derecha. La parte de vacio (-20..0) esta comprimida */
    { .type = DASH_EL_IMAGE, .x = TURBO_X, .y = TURBO_Y, .img = &img_turbo_gauge },
    {
        .type = DASH_EL_NEEDLE, .sensor = SENSOR_BOOST,
        .x = TURBO_X + 154, .y = TURBO_Y + 154,
        .img = &img_turbo_needle, .pivot_x = 16, .pivot_y = 118,
        .cal = { { -20, -135 }, { 0, -90 }, { 40, 90 } }, .cal_count = 3,
    },

    /* Velocidad con los digitos del tema, debajo del reloj de RPM */
    {
        .type = DASH_EL_DIGITS, .sensor = SENSOR_SPEED, .movable = true,
        .x = 416, .y = 400, .digit_imgs = s_digits, .digit_count = 3, .digit_spacing = 64,
    },

    /* Luces de cambio: secuencia de imagenes elegida segun las RPM (4500 a 7500) */
    {
        .type = DASH_EL_FRAMES, .sensor = SENSOR_RPM, .movable = true,
        .caption = "Luces de cambio",
        .x = 312, .y = 520, .frames = s_shift, .frame_count = 11, .min = 4500, .max = 7500,
    },

    /* Valores con texto, se pueden acomodar desde el modo edicion */
    { .type = DASH_EL_VALUE, .sensor = SENSOR_WATER_TEMP, .movable = true, .x = 40,  .y = 60,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "AGUA" },
    { .type = DASH_EL_VALUE, .sensor = SENSOR_OIL_TEMP,   .movable = true, .x = 40,  .y = 160,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "TEMP ACEITE" },
    { .type = DASH_EL_VALUE, .sensor = SENSOR_OIL_PRESS,  .movable = true, .x = 40,  .y = 260,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "PRES ACEITE" },
    { .type = DASH_EL_VALUE, .sensor = SENSOR_BATTERY,    .movable = true, .x = 40,  .y = 360,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "BATERIA" },
    { .type = DASH_EL_VALUE, .sensor = SENSOR_AFR,        .movable = true, .x = 760, .y = 380,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "AFR" },

    /* Disponibles en el menu, ocultos de entrada */
    { .type = DASH_EL_VALUE, .sensor = SENSOR_RPM,   .movable = true, .hidden = true, .x = 760, .y = 460,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "RPM" },
    { .type = DASH_EL_VALUE, .sensor = SENSOR_BOOST, .movable = true, .hidden = true, .x = 880, .y = 380,
      .font = &lv_font_montserrat_26, .color = 0xFFB020, .caption = "TURBO" },
};

const dash_theme_t theme_initial_d = {
    .name = "initial d",
    .bg_color = 0x000000,
    .elements = s_elements,
    .element_count = sizeof(s_elements) / sizeof(s_elements[0]),
};
