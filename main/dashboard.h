#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "sensors.h"

#define DASH_MAX_CAL_POINTS 8
#define DASH_MAX_DIGITS     5
#define DASH_MAX_RANGES     8

typedef enum {
    DASH_EL_IMAGE,   /* imagen fija: fondo, reloj, logo */
    DASH_EL_NEEDLE,  /* aguja que gira segun un sensor */
    DASH_EL_DIGITS,  /* numero dibujado con imagenes 0-9 */
    DASH_EL_VALUE,   /* numero con fuente de texto */
    DASH_EL_BAR,     /* barra que se llena segun un sensor (dibujada o con imagenes) */
    DASH_EL_FRAMES,  /* secuencia de imagenes: se elige el cuadro segun el valor */
} dash_el_type_t;

/* Punto de calibracion de una aguja: con este valor, la aguja va a este angulo.
 * Angulo en grados, 0 = apuntando hacia arriba, positivo = sentido horario.
 * Con varios puntos se pueden seguir escalas no lineales. */
typedef struct {
    float value;
    float angle;
} dash_cal_pt_t;

/* Tramo de una animacion (DASH_EL_FRAMES): con valores entre from y to se muestran
 * los cuadros first..last (contando desde 0), repartidos parejo en el tramo.
 * fps > 0: en ese tramo los cuadros pasan solos, a fps por segundo. */
typedef struct {
    float from, to;
    uint8_t first, last;
    uint8_t fps;
} dash_frame_range_t;

typedef struct {
    dash_el_type_t type;
    sensor_id_t sensor;
    int16_t x, y;   /* esquina superior izquierda; en DASH_EL_NEEDLE es el centro de giro */
    bool movable;   /* se puede mover desde el modo edicion y ocultar desde el menu */
    bool hidden;    /* arranca oculto (se puede activar desde el menu) */
    uint8_t group;  /* != 0: los elementos con el mismo grupo se mueven y ocultan juntos.
                       El primero del grupo tiene que ser el movable (es el que sale en el menu) */
    uint16_t scale; /* tamano inicial: 256 = 100% (0 = 100%). El usuario lo cambia con -/+ */

    /* DASH_EL_IMAGE y DASH_EL_NEEDLE */
    const lv_image_dsc_t *img;

    /* DASH_EL_NEEDLE */
    int16_t pivot_x, pivot_y;   /* punto de giro dentro de la imagen de la aguja */
    dash_cal_pt_t cal[DASH_MAX_CAL_POINTS];
    uint8_t cal_count;
    float smoothing;            /* 0..1, mas bajo = mas suave/lento. 0 = 0.35. Tambien BAR */

    /* DASH_EL_BAR y DASH_EL_FRAMES: rango del sensor */
    float min, max;

    /* DASH_EL_BAR */
    int16_t w, h;               /* tamano si es dibujada (con imagenes se usa el de img_on) */
    bool vertical;              /* llena de abajo hacia arriba en vez de izquierda a derecha */
    const lv_image_dsc_t *img_on;   /* barra encendida; NULL = barra dibujada */
    const lv_image_dsc_t *img_off;  /* barra apagada de fondo (opcional) */
    uint32_t bar_bg;            /* dibujada: color de fondo */
    uint32_t bar_color;         /* dibujada: color de relleno */
    float warn;                 /* dibujada: desde este valor usa warn_color (si warn_color != 0) */
    uint32_t warn_color;

    /* DASH_EL_FRAMES: imagenes en memoria (frames) o archivos que se leen
     * cuando hacen falta (frame_path_fmt, ej. "S:/dashboards/x/fondo_%02u.jpg") */
    const lv_image_dsc_t *const *frames;
    const char *frame_path_fmt;
    uint8_t frame_count;
    dash_frame_range_t ranges[DASH_MAX_RANGES];  /* que cuadros van con que valores */
    uint8_t range_count;                         /* 0 = todos parejo de min a max */
    bool preload;               /* desde archivo: leer todos los cuadros al abrir el tema,
                                   si entran en memoria (cambian al instante) */
    bool shift_light;           /* imagen del shift light: no sigue al sensor, aparece con las
                                   RPM del shift light del tema (ver dash_shift_t) */

    /* DASH_EL_DIGITS */
    const lv_image_dsc_t *const *digit_imgs;   /* 10 imagenes: 0..9 */
    uint8_t digit_count;
    int16_t digit_spacing;

    /* DASH_EL_VALUE */
    const lv_font_t *font;
    uint32_t color;           /* 0xRRGGBB */
    const char *caption;      /* nombre arriba del valor; NULL = nombre del sensor, "" = sin nombre.
                                 Se muestra solo si "Mostrar nombres" esta activo */
    uint32_t caption_color;   /* 0 = mismo color que el valor */
    const lv_font_t *caption_font;  /* NULL = montserrat 16 */
    bool hide_unit;
} dash_element_t;

/* Alarma de un sensor: fuera de [low, high] su valor se pone rojo y titila */
typedef struct {
    bool use_low, use_high;
    float low, high;
    float rpm_min;            /* solo avisa con las RPM por encima de esto (0 = siempre) */
} dash_alarm_t;

#define DASH_SHIFT_BORDER 0x01   /* shift light en el borde de la pantalla */
#define DASH_SHIFT_BAR    0x02   /* la barra de RPM titila */
#define DASH_SHIFT_IMAGE  0x04   /* una imagen o GIF (elemento FRAMES con shift_light) */

/* Shift light: desde warn_rpm prende fijo (borde, o la imagen quieta en su primer cuadro),
 * desde flash_rpm titila (borde, barra, imagen) o se anima (GIF, a fps cuadros por segundo) */
typedef struct {
    float warn_rpm;           /* 0 = sin aviso fijo */
    float flash_rpm;          /* 0 = sin shift light */
    uint32_t color;
    uint8_t mode;             /* DASH_SHIFT_BORDER | DASH_SHIFT_BAR | DASH_SHIFT_IMAGE */
    uint8_t fps;              /* GIF del shift light: cuadros por segundo (0 = 12) */
} dash_shift_t;

typedef struct {
    const char *name;
    uint32_t bg_color;
    bool show_names;          /* valor inicial de "Mostrar nombres" */
    uint32_t layout_sig;      /* cambia si cambia la ubicacion inicial (dashboard.txt de la SD):
                                 lo acomodado en la pantalla se descarta y se usa lo nuevo */
    const dash_element_t *elements;
    size_t element_count;
    dash_alarm_t alarms[SENSOR_COUNT];
    bool alarm_border;        /* con una alarma, el borde de la pantalla titila en rojo */
    dash_shift_t shift;
} dash_theme_t;

/* Arranca el dashboard con la lista de temas disponibles. Llamar con el lock de
 * LVGL tomado. Usa el ultimo tema elegido y la configuracion guardada en NVS
 * (valores visibles, posiciones, nombres). Mantener presionado el fondo abre el
 * menu de ajustes, desde donde tambien se cambia de tema. */
void dashboard_start(const dash_theme_t *const *themes, size_t theme_count);

/* Para el menu de ajustes a pantalla completa: con paused = true deja de refrescar
 * valores, agujas, animaciones, alarmas y shift light (no gasta tiempo detras del menu).
 * Llamar con el lock de LVGL tomado. */
void dashboard_set_paused(bool paused);

/* Abre el menu del dashboard (valores visibles, nombres, mover valores, temas) */
void dashboard_menu_open(void);

/* Lee una imagen (PNG/JPG) con los decodificadores de LVGL y devuelve una copia
 * propia en memoria, o NULL si no se pudo. Se libera con lv_draw_buf_destroy(). */
lv_draw_buf_t *dash_image_load(const char *path);

extern const dash_theme_t theme_initial_d;
extern const dash_theme_t theme_barras;
