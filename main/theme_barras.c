/* Tema "barras": dashboard simple sin relojes. Barra de RPM arriba (con imagenes),
 * valores con su nombre y barras dibujadas por el ESP. Pantalla de 1024x600. */
#include "dashboard.h"
#include "assets/assets.h"

#if LV_FONT_MONTSERRAT_48
#define BIG_FONT (&lv_font_montserrat_48)
#else
#define BIG_FONT (&lv_font_montserrat_26)
#endif

#define NAME_COLOR  0xFF4040
#define VALUE_COLOR 0xFF70B0
#define BAR_BG      0x4A4A4A
#define BAR_COLOR   0xF0C020
#define WARN_COLOR  0xFF3020

/* Fila de la izquierda: nombre + valor y una barra horizontal debajo (mismo grupo) */
#define ROW(grp, sens, name, yy, lo, hi, warn_at)                                         \
    { .type = DASH_EL_VALUE, .sensor = sens, .movable = true, .group = grp, .x = 30, .y = yy, \
      .font = BIG_FONT, .color = VALUE_COLOR, .caption = name, .caption_color = NAME_COLOR }, \
    { .type = DASH_EL_BAR, .sensor = sens, .group = grp, .x = 30, .y = yy + 82, .w = 560, .h = 20, \
      .min = lo, .max = hi, .bar_bg = BAR_BG, .bar_color = BAR_COLOR,                        \
      .warn = warn_at, .warn_color = WARN_COLOR }

/* Columna derecha: barra vertical chica + nombre + valor (mismo grupo) */
#define SIDE(grp, sens, name, yy, lo, hi)                                                  \
    { .type = DASH_EL_VALUE, .sensor = sens, .movable = true, .group = grp, .x = 690, .y = yy, \
      .font = BIG_FONT, .color = VALUE_COLOR, .caption = name, .caption_color = NAME_COLOR }, \
    { .type = DASH_EL_BAR, .sensor = sens, .group = grp, .x = 640, .y = yy + 10, .w = 26, .h = 80, \
      .vertical = true, .min = lo, .max = hi, .bar_bg = BAR_BG, .bar_color = BAR_COLOR }

static const dash_element_t s_elements[] = {
    /* Barra de RPM de izquierda a derecha: imagen apagada + imagen encendida que crece */
    {
        .type = DASH_EL_BAR, .sensor = SENSOR_RPM, .movable = true, .x = 12, .y = 10,
        .caption = "Barra RPM",
        .img_off = &img_rpm_bar_off, .img_on = &img_rpm_bar_on, .min = 0, .max = 8000,
        .smoothing = 0.5f,
    },

    ROW(1, SENSOR_OIL_PRESS,  "Pres aceite", 84,  0, 7, 6.5f),
    ROW(2, SENSOR_OIL_TEMP,   "Temp aceite", 212, 40, 150, 125),
    ROW(3, SENSOR_WATER_TEMP, "Temp agua",   340, 40, 120, 100),
    ROW(4, SENSOR_SPEED,      "Velocidad",   468, 0, 240, 1000),

    SIDE(5, SENSOR_BOOST,   "Turbo",   84,  -20, 30),
    SIDE(6, SENSOR_AFR,     "AFR",     212, 10, 20),
    SIDE(7, SENSOR_BATTERY, "Bateria", 340, 10, 16),
    SIDE(8, SENSOR_RPM,     "RPM",     468, 0, 8000),
};

const dash_theme_t theme_barras = {
    .name = "barras",
    .bg_color = 0x0A0A0A,
    .show_names = true,
    .elements = s_elements,
    .element_count = sizeof(s_elements) / sizeof(s_elements[0]),
};
