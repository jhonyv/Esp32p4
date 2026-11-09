#include "dashboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "usb_mode.h"
#include "settings.h"
#include "src/draw/lv_image_decoder_private.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static const char *TAG = "dashboard";

#define DASH_REFRESH_MS 30
#define EDIT_COLOR      0x00C8FF
#define MENU_BG         0x161616
#define NVS_NAMESPACE   "dash"
#define LAYOUT_VERSION  5
#define SCALE_100       256     /* escala 1.0 (como lv_image_set_scale) */
#define SCALE_MIN       128
#define SCALE_MAX       640
#define SCALE_STEP      26      /* ~10% */
#define SELECTED_COLOR  0xFF8C00
#define FRAME_MIN_MS    50      /* fondo animado: como maximo ~20 cambios por segundo */
#define FRAME_CACHE_MAX (12u * 1024 * 1024)   /* animaciones en memoria, entre todas */
#define FRAME_FREE_KEEP (4u * 1024 * 1024)    /* memoria que se deja libre para lo demas */
#define NVS_THEME_KEY   "theme_idx"
#define ALARM_COLOR     0xFF2020
#define ALARM_BLINK_MS  300     /* alarmas: medio ciclo del titileo */
#define ALARM_DELAY_MS  300     /* se tiene que pasar del limite este tiempo seguido para avisar */
#define SHIFT_BLINK_MS  80      /* shift light: titila rapido */
#define BORDER_W        14      /* ancho del borde de la pantalla (alarmas y shift light) */

typedef struct dash_item {
    dash_element_t el;      /* copia en RAM: el menu y el modo edicion cambian x/y/hidden */
    lv_obj_t *obj;
    lv_obj_t *digits[DASH_MAX_DIGITS];
    lv_obj_t *caption_label;
    lv_obj_t *value_label;
    lv_obj_t *fill;         /* BAR: parte llena (o recorte de la imagen encendida) */
    lv_obj_t *fill_img;     /* BAR con imagenes: imagen encendida dentro del recorte */
    float shown;            /* valor mostrado por agujas y barras (suavizado) */
    int32_t last_rotation;
    int32_t last_number;    /* DIGITS: numero, FRAMES: cuadro, BAR: pixeles llenos */
    int8_t last_warn;
    lv_draw_buf_t *frame_buf;   /* FRAMES desde archivo: cuadro que se esta mostrando */
    lv_draw_buf_t **frame_cache;    /* FRAMES con preload: los cuadros en memoria (o NULL) */
    uint8_t frame_step;             /* 1 = todos; N = solo 1 de cada N (no entraban todos) */
    uint32_t frame_tick;
    char last_text[24];
    struct dash_item *leader;   /* primero del grupo (o el mismo item) */
    int16_t dx0, dy0;           /* distancia al lider en el tema, sin escalar */
    uint16_t scale;             /* SCALE_100 = tamano original */
    int8_t alarm_state;         /* VALUE: 0 normal, 1 alarma (prendido), 2 alarma (tenue), -1 revisar */
    bool shift_off;             /* BAR de RPM: relleno apagado por el shift light */
} dash_item_t;

/* Lo que se guarda en NVS por cada tema */
typedef struct {
    int16_t x, y;
    uint16_t scale;
    uint8_t hidden;
    uint8_t reserved;
    uint32_t color;     /* color del texto (VALUE) o de la barra dibujada (BAR) */
} saved_item_t;

typedef struct {
    uint8_t version;
    uint8_t show_names;
    uint8_t count;
    uint8_t reserved;
    uint32_t sig;       /* layout_sig del tema cuando se guardo */
    saved_item_t items[];
} saved_layout_t;

static const dash_theme_t *const *s_themes;
static size_t s_theme_count;
static size_t s_theme_idx;
static const dash_theme_t *s_theme;
static lv_timer_t *s_timer;
static dash_item_t *s_items;
static size_t s_item_count;
static bool s_show_names;
static bool s_edit;
static lv_obj_t *s_menu;
static lv_obj_t *s_edit_bar;
static dash_item_t *s_selected;     /* elemento elegido en modo edicion */
static bool s_paused;               /* menu de ajustes abierto: no se refresca nada */

/* Alarmas y shift light */
static bool s_alarm[SENSOR_COUNT];
static uint32_t s_alarm_since[SENSOR_COUNT];    /* desde cuando esta pasado del limite (0 = no) */
static lv_obj_t *s_border[4];                   /* arriba, abajo, izquierda, derecha */
static int32_t s_border_state;                  /* color que muestra, -1 = apagado */
static lv_obj_t *s_banner;                      /* aviso de alarmas de valores que no se ven */
static lv_obj_t *s_banner_label;
static bool s_shift_bar_off;                    /* la barra de RPM titila: ahora apagada */

static float needle_angle(const dash_element_t *el, float v)
{
    const dash_cal_pt_t *c = el->cal;
    int n = el->cal_count;
    if (n < 2) {
        return 0;
    }
    if (v <= c[0].value) {
        return c[0].angle;
    }
    for (int i = 1; i < n; i++) {
        if (v <= c[i].value) {
            float t = (v - c[i - 1].value) / (c[i].value - c[i - 1].value);
            return c[i - 1].angle + t * (c[i].angle - c[i - 1].angle);
        }
    }
    return c[n - 1].angle;
}

static float value_fraction(const dash_element_t *el, float v)
{
    if (el->max <= el->min) {
        return 0;
    }
    float f = (v - el->min) / (el->max - el->min);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

/* Cuadro de una animacion segun el valor (y el tiempo, en los tramos que se mueven solos).
 * Antes del primer tramo queda en su primer cuadro; entre tramos, en el ultimo del anterior. */
static int32_t frame_index(const dash_element_t *el, float v)
{
    int32_t n = el->frame_count;
    int32_t idx;
    if (!el->range_count) {
        idx = (int32_t)lroundf(value_fraction(el, v) * (n - 1));
    } else {
        idx = el->ranges[0].first;
        for (int i = 0; i < el->range_count; i++) {
            const dash_frame_range_t *r = &el->ranges[i];
            if (v < r->from) {
                break;
            }
            int32_t span = r->last >= r->first ? r->last - r->first : r->first - r->last;
            int32_t dir = r->last >= r->first ? 1 : -1;
            if (v <= r->to) {
                int32_t step;
                if (r->fps) {
                    step = (int32_t)(((uint64_t)lv_tick_get() * r->fps / 1000) % (uint64_t)(span + 1));
                } else {
                    float t = r->to > r->from ? (v - r->from) / (r->to - r->from) : 1;
                    step = (int32_t)floorf(t * (span + 1));
                    step = step > span ? span : step;
                }
                idx = r->first + dir * step;
                break;
            }
            idx = r->last;
        }
    }
    return idx < 0 ? 0 : (idx >= n ? n - 1 : idx);
}

/* Los elementos del mismo grupo se mueven y ocultan juntos */
static bool same_group(const dash_item_t *a, const dash_item_t *b)
{
    return a == b || (a->el.group != 0 && a->el.group == b->el.group);
}

/* Texto arriba del valor ("" = sin nombre) */
static const char *caption_text(const dash_item_t *it)
{
    return it->el.caption ? it->el.caption : sensors_info(it->el.sensor)->name;
}

/* Nombre en el menu: si el valor no tiene nombre, el del sensor */
static const char *item_name(const dash_item_t *it)
{
    const char *c = caption_text(it);
    return c[0] ? c : sensors_info(it->el.sensor)->name;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- Guardar / cargar configuracion (NVS) ---------- */

static void nvs_key(char *key, size_t len)
{
    /* las claves de NVS son de 15 caracteres como maximo */
    snprintf(key, len, "%.15s", s_theme->name);
}

static void layout_save(void)
{
    size_t size = sizeof(saved_layout_t) + s_item_count * sizeof(saved_item_t);
    saved_layout_t *l = lv_malloc_zeroed(size);
    if (!l) {
        return;
    }
    l->version = LAYOUT_VERSION;
    l->show_names = s_show_names;
    l->count = s_item_count;
    l->sig = s_theme->layout_sig;
    for (size_t i = 0; i < s_item_count; i++) {
        l->items[i].x = s_items[i].el.x;
        l->items[i].y = s_items[i].el.y;
        l->items[i].hidden = s_items[i].el.hidden;
        l->items[i].scale = s_items[i].scale;
        l->items[i].color = s_items[i].el.type == DASH_EL_BAR ? s_items[i].el.bar_color : s_items[i].el.color;
    }

    char key[16];
    nvs_key(key, sizeof(key));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, l, size);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuracion guardada");
    } else {
        ESP_LOGW(TAG, "No se pudo guardar la configuracion (%d)", err);
    }
    lv_free(l);
}

/* Aplica sobre s_items lo guardado. Si el tema cambio de cantidad de elementos
 * se descarta lo guardado y se usan los valores del tema. */
static void layout_load(void)
{
    s_show_names = s_theme->show_names;

    char key[16];
    nvs_key(key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t size = 0;
    saved_layout_t *l = NULL;
    if (nvs_get_blob(h, key, NULL, &size) == ESP_OK &&
            size == sizeof(saved_layout_t) + s_item_count * sizeof(saved_item_t)) {
        l = lv_malloc(size);
        if (l && nvs_get_blob(h, key, l, &size) != ESP_OK) {
            lv_free(l);
            l = NULL;
        }
    }
    nvs_close(h);

    if (l && l->sig != s_theme->layout_sig) {
        /* se cambio la ubicacion en dashboard.txt (ej. desde el armador): manda lo nuevo */
        ESP_LOGI(TAG, "'%s' cambio en la SD: se usa la ubicacion nueva", s_theme->name);
        lv_free(l);
        l = NULL;
    }
    if (l && l->version == LAYOUT_VERSION && l->count == s_item_count) {
        s_show_names = l->show_names;
        for (size_t i = 0; i < s_item_count; i++) {
            s_items[i].el.x = l->items[i].x;
            s_items[i].el.y = l->items[i].y;
            s_items[i].el.hidden = l->items[i].hidden;
            if (l->items[i].scale >= SCALE_MIN && l->items[i].scale <= SCALE_MAX) {
                s_items[i].scale = l->items[i].scale;
            }
            if (s_items[i].el.type == DASH_EL_BAR) {
                s_items[i].el.bar_color = l->items[i].color;
            } else {
                s_items[i].el.color = l->items[i].color;
            }
        }
        ESP_LOGI(TAG, "Configuracion cargada");
    }
    lv_free(l);
}

/* ---------- Creacion y actualizacion de elementos ---------- */

static lv_obj_t *create_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return box;
}

lv_draw_buf_t *dash_image_load(const char *path)
{
    lv_image_decoder_dsc_t dsc;
    lv_image_decoder_args_t args = { .no_cache = true };
    if (lv_image_decoder_open(&dsc, path, &args) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "No se pudo leer %s", path);
        return NULL;
    }
    lv_draw_buf_t *copy = dsc.decoded ? lv_draw_buf_dup(dsc.decoded) : NULL;
    lv_image_decoder_close(&dsc);
    return copy;
}

/* Lee del archivo el cuadro idx y lo pone en la imagen; libera el anterior */
static bool show_file_frame(dash_item_t *it, int32_t idx)
{
    char path[128];
    snprintf(path, sizeof(path), it->el.frame_path_fmt, (unsigned)idx);
    lv_draw_buf_t *buf = dash_image_load(path);
    if (!buf) {
        return false;
    }
    lv_image_set_src(it->obj, buf);
    if (it->frame_buf) {
        lv_draw_buf_destroy(it->frame_buf);
    }
    it->frame_buf = buf;
    it->frame_tick = lv_tick_get();
    return true;
}

/* ARGB8888 -> RGB565A8 (o RGB565 si no tiene transparencia): ocupa 3/4 o la mitad */
static lv_draw_buf_t *compact_frame(lv_draw_buf_t *src)
{
    if (src->header.cf != LV_COLOR_FORMAT_ARGB8888) {
        return src;
    }
    uint32_t w = src->header.w, h = src->header.h;
    bool opaque = true;
    for (uint32_t y = 0; y < h && opaque; y++) {
        const uint8_t *p = src->data + y * src->header.stride;
        for (uint32_t x = 0; x < w; x++) {
            if (p[x * 4 + 3] != 0xFF) {
                opaque = false;
                break;
            }
        }
    }
    lv_draw_buf_t *dst = lv_draw_buf_create(w, h, opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8,
                                            LV_STRIDE_AUTO);
    if (!dst) {
        return src;
    }
    uint32_t stride = dst->header.stride;
    uint8_t *alpha = dst->data + stride * h;         /* RGB565A8: el alfa va despues de los colores */
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *p = src->data + y * src->header.stride;
        uint16_t *c = (uint16_t *)(dst->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t b = p[x * 4], g = p[x * 4 + 1], r = p[x * 4 + 2];
            c[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            if (!opaque) {
                alpha[y * (stride / 2) + x] = p[x * 4 + 3];
            }
        }
    }
    lv_draw_buf_destroy(src);
    return dst;
}

static size_t s_frame_cache_bytes;      /* lo que ocupan las animaciones del tema abierto */

static bool frame_memory_ok(size_t need)
{
#ifdef ESP_PLATFORM
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > need * 2 + FRAME_FREE_KEEP;
#else
    (void)need;
    return true;
#endif
}

static void free_frame_cache(dash_item_t *it)
{
    if (!it->frame_cache) {
        return;
    }
    for (int i = 0; i < it->el.frame_count; i++) {
        if (it->frame_cache[i]) {
            s_frame_cache_bytes -= it->frame_cache[i]->data_size;
            lv_draw_buf_destroy(it->frame_cache[i]);
        }
    }
    lv_free(it->frame_cache);
    it->frame_cache = NULL;
    it->frame_step = 0;
}

/* El cuadro que hay en memoria para idx: si se guardo 1 de cada N, el anterior que haya */
static lv_draw_buf_t *cached_frame(const dash_item_t *it, int32_t idx)
{
    return it->frame_cache[it->frame_step > 1 ? idx - idx % it->frame_step : idx];
}

static bool is_jpg_path(const char *fmt)
{
    size_t n = strlen(fmt);
    return n >= 3 && strcmp(fmt + n - 3, "jpg") == 0;
}

/* Lee los cuadros a memoria (FRAME_CACHE_MAX entre todas las animaciones). Si no entran todos,
 * guarda 1 de cada N y los del medio muestran el anterior: se ve con menos cuadros pero va fluida.
 * Si ni asi entran (o son JPG, que se leen rapido de la SD), quedan para leer de a uno desde la SD.
 * El armador de dashboards hace la misma cuenta para avisar (animPlans). */
static void preload_frames(dash_item_t *it)
{
    const dash_element_t *el = &it->el;
    const int n = el->frame_count;
    char path[128];
    snprintf(path, sizeof(path), el->frame_path_fmt, 0u);
    lv_draw_buf_t *first = frame_memory_ok(0) ? dash_image_load(path) : NULL;
    if (!first) {
        ESP_LOGW(TAG, "La animacion no entra en memoria (%d cuadros): se lee desde la SD", n);
        return;
    }
    first = compact_frame(first);
    const size_t size = first->data_size;
    const size_t room = s_frame_cache_bytes < FRAME_CACHE_MAX ? FRAME_CACHE_MAX - s_frame_cache_bytes : 0;
    const int fit = (int)(room / size);
    int step = 1;
    if (fit < n) {
        if (fit < 2 || is_jpg_path(el->frame_path_fmt)) {
            lv_draw_buf_destroy(first);
            ESP_LOGW(TAG, "La animacion no entra en memoria (%d cuadros de %u KB): se lee desde la SD", n,
                     (unsigned)(size / 1024));
            return;
        }
        step = (n + fit - 1) / fit;
    }
    it->frame_cache = lv_malloc_zeroed(n * sizeof(lv_draw_buf_t *));
    if (!it->frame_cache) {
        lv_draw_buf_destroy(first);
        return;
    }
    it->frame_step = (uint8_t)step;
    it->frame_cache[0] = first;
    s_frame_cache_bytes += size;
    int kept = 1;
    for (int i = step; i < n; i += step) {
        snprintf(path, sizeof(path), el->frame_path_fmt, (unsigned)i);
        lv_draw_buf_t *buf = frame_memory_ok(size * 2) ? dash_image_load(path) : NULL;
        if (buf) {
            buf = compact_frame(buf);
        }
        if (!buf || s_frame_cache_bytes + buf->data_size > FRAME_CACHE_MAX) {
            if (buf) {
                lv_draw_buf_destroy(buf);
            }
            ESP_LOGW(TAG, "La animacion no entra en memoria (%d cuadros): se lee desde la SD", n);
            free_frame_cache(it);
            return;
        }
        it->frame_cache[i] = buf;
        s_frame_cache_bytes += buf->data_size;
        kept++;
    }
    if (step > 1) {
        ESP_LOGW(TAG, "Animacion en memoria: %d de %d cuadros (1 de cada %d, no entraban todos), %u KB en total",
                 kept, n, step, (unsigned)(s_frame_cache_bytes / 1024));
    } else {
        ESP_LOGI(TAG, "Animacion en memoria: %d cuadros, %u KB en total", n, (unsigned)(s_frame_cache_bytes / 1024));
    }
}

static void create_item(lv_obj_t *scr, dash_item_t *it)
{
    const dash_element_t *el = &it->el;
    it->last_rotation = INT32_MIN;
    it->last_number = INT32_MIN;

    switch (el->type) {
    case DASH_EL_IMAGE:
        it->obj = lv_image_create(scr);
        lv_image_set_src(it->obj, el->img);
        lv_obj_set_pos(it->obj, el->x, el->y);
        break;

    case DASH_EL_NEEDLE:
        it->obj = lv_image_create(scr);
        lv_image_set_src(it->obj, el->img);
        lv_image_set_pivot(it->obj, el->pivot_x, el->pivot_y);
        lv_image_set_antialias(it->obj, true);
        /* se ubica la imagen para que su punto de giro caiga en (x, y) */
        lv_obj_set_pos(it->obj, el->x - el->pivot_x, el->y - el->pivot_y);
        it->shown = el->cal_count ? el->cal[0].value : 0;
        break;

    case DASH_EL_DIGITS: {
        uint8_t n = el->digit_count > DASH_MAX_DIGITS ? DASH_MAX_DIGITS : el->digit_count;
        it->el.digit_count = n;
        it->obj = create_box(scr);
        lv_obj_set_size(it->obj, n * el->digit_spacing, el->digit_imgs[0]->header.h);
        lv_obj_set_pos(it->obj, el->x, el->y);
        for (int i = 0; i < n; i++) {
            it->digits[i] = lv_image_create(it->obj);
            lv_image_set_src(it->digits[i], el->digit_imgs[0]);
            lv_obj_set_pos(it->digits[i], i * el->digit_spacing, 0);
        }
        break;
    }

    case DASH_EL_VALUE: {
        it->obj = create_box(scr);
        lv_obj_set_size(it->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(it->obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_pos(it->obj, el->x, el->y);
        it->caption_label = lv_label_create(it->obj);
        lv_label_set_text(it->caption_label, caption_text(it));
        lv_obj_set_style_text_font(it->caption_label, el->caption_font ? el->caption_font : &lv_font_montserrat_16, 0);
        if (el->caption_color) {
            lv_obj_set_style_text_color(it->caption_label, lv_color_hex(el->caption_color), 0);
        } else {
            lv_obj_set_style_text_color(it->caption_label, lv_color_hex(el->color), 0);
            lv_obj_set_style_text_opa(it->caption_label, LV_OPA_70, 0);
        }
        it->value_label = lv_label_create(it->obj);
        lv_label_set_text(it->value_label, "--");
        lv_obj_set_style_text_font(it->value_label, el->font ? el->font : LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(it->value_label, lv_color_hex(el->color), 0);
        break;
    }

    case DASH_EL_BAR:
        it->obj = create_box(scr);
        lv_obj_set_pos(it->obj, el->x, el->y);
        it->shown = el->min;
        if (el->img_on) {
            /* imagen apagada de fondo + recorte de la imagen encendida que crece */
            lv_obj_set_size(it->obj, el->img_on->header.w, el->img_on->header.h);
            if (el->img_off) {
                lv_obj_t *off = lv_image_create(it->obj);
                lv_image_set_src(off, el->img_off);
            }
            it->fill = create_box(it->obj);
            it->fill_img = lv_image_create(it->fill);
            lv_image_set_src(it->fill_img, el->img_on);
        } else {
            lv_obj_set_size(it->obj, el->w, el->h);
            lv_obj_set_style_bg_color(it->obj, lv_color_hex(el->bar_bg), 0);
            lv_obj_set_style_bg_opa(it->obj, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(it->obj, 3, 0);
            it->fill = create_box(it->obj);
            lv_obj_set_style_bg_color(it->fill, lv_color_hex(el->bar_color), 0);
            lv_obj_set_style_bg_opa(it->fill, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(it->fill, 3, 0);
        }
        it->last_warn = -1;
        lv_obj_set_size(it->fill, 0, 0);
        break;

    case DASH_EL_FRAMES: {
        it->obj = lv_image_create(scr);
        lv_obj_set_pos(it->obj, el->x, el->y);
        if (el->frame_path_fmt && el->preload) {
            preload_frames(it);
        }
        it->shown = sensors_get(el->sensor);
        int32_t idx = frame_index(el, it->shown);
        if (it->frame_cache) {
            idx = it->frame_step > 1 ? idx - idx % it->frame_step : idx;
            lv_image_set_src(it->obj, cached_frame(it, idx));
        } else if (el->frame_path_fmt) {
            show_file_frame(it, idx);
        } else {
            lv_image_set_src(it->obj, el->frames[idx]);
        }
        it->last_number = idx;
        break;
    }
    }
}

/* Ubica el item y le aplica la escala. Los miembros de un grupo se ubican
 * relativos al lider, asi al agrandar/achicar no se pisan entre ellos. */
static void place_item(dash_item_t *it)
{
    dash_item_t *ld = it->leader;
    if (ld != it) {
        it->scale = ld->scale;
        it->el.x = ld->el.x + it->dx0 * it->scale / SCALE_100;
        it->el.y = ld->el.y + it->dy0 * it->scale / SCALE_100;
    }
    if (it->el.type == DASH_EL_NEEDLE) {
        lv_obj_set_pos(it->obj, it->el.x - it->el.pivot_x, it->el.y - it->el.pivot_y);
        lv_image_set_scale(it->obj, it->scale);
        return;
    }
    lv_obj_set_pos(it->obj, it->el.x, it->el.y);
    if (it->scale != SCALE_100) {
        lv_obj_set_style_transform_pivot_x(it->obj, 0, 0);
        lv_obj_set_style_transform_pivot_y(it->obj, 0, 0);
    }
    lv_obj_set_style_transform_scale(it->obj, it->scale, 0);
}

static void place_group(dash_item_t *ld)
{
    for (size_t i = 0; i < s_item_count; i++) {
        if (s_items[i].leader == ld) {
            place_item(&s_items[i]);
        }
    }
}

static void apply_settings(void)
{
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        set_hidden(it->obj, it->el.hidden);
        if (it->caption_label) {
            set_hidden(it->caption_label, !s_show_names || !caption_text(it)[0]);
        }
    }
}

/* ---------- Alarmas y shift light ---------- */

static bool alarm_blink_on(void)
{
    return (lv_tick_get() / ALARM_BLINK_MS) % 2 == 0;
}

/* Margen para salir de la alarma, asi no parpadea cuando el valor anda justo en el limite */
static float alarm_margin(float limit)
{
    float m = fabsf(limit) * 0.015f;
    return m < 0.1f ? 0.1f : m;
}

static void alarms_update(void)
{
    float rpm = sensors_get(SENSOR_RPM);
    for (int i = 0; i < SENSOR_COUNT; i++) {
        const dash_alarm_t *a = &s_theme->alarms[i];
        if ((!a->use_low && !a->use_high) || !sensors_enabled((sensor_id_t)i)) {
            s_alarm[i] = false;
            s_alarm_since[i] = 0;
            continue;
        }
        float v = sensors_get((sensor_id_t)i);
        bool on = s_alarm[i];
        bool rpm_ok = a->rpm_min <= 0 || rpm >= a->rpm_min - (on ? 100 : 0);
        bool out = rpm_ok &&
                   ((a->use_high && v > a->high - (on ? alarm_margin(a->high) : 0)) ||
                    (a->use_low && v < a->low + (on ? alarm_margin(a->low) : 0)));
        if (!out) {
            s_alarm[i] = false;
            s_alarm_since[i] = 0;
        } else if (!on) {
            if (!s_alarm_since[i]) {
                s_alarm_since[i] = lv_tick_get() | 1;
            } else if (lv_tick_elaps(s_alarm_since[i]) >= ALARM_DELAY_MS) {
                s_alarm[i] = true;
            }
        }
    }
}

/* Valor rojo que titila (st 1/2) o sus colores de siempre (st 0) */
static void set_value_alarm(dash_item_t *it, int8_t st)
{
    const dash_element_t *el = &it->el;
    it->alarm_state = st;
    if (st) {
        lv_opa_t opa = st == 1 ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_text_color(it->value_label, lv_color_hex(ALARM_COLOR), 0);
        lv_obj_set_style_text_opa(it->value_label, opa, 0);
        lv_obj_set_style_text_color(it->caption_label, lv_color_hex(ALARM_COLOR), 0);
        lv_obj_set_style_text_opa(it->caption_label, opa, 0);
        return;
    }
    lv_obj_set_style_text_color(it->value_label, lv_color_hex(el->color), 0);
    lv_obj_set_style_text_opa(it->value_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(it->caption_label, lv_color_hex(el->caption_color ? el->caption_color : el->color), 0);
    lv_obj_set_style_text_opa(it->caption_label, el->caption_color ? LV_OPA_COVER : LV_OPA_70, 0);
}

/* El valor (con texto) de un sensor que se ve en la pantalla, o NULL */
static dash_item_t *value_item(sensor_id_t id, bool visible_only)
{
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        if (it->el.type == DASH_EL_VALUE && it->el.sensor == id && (!visible_only || !it->el.hidden)) {
            return it;
        }
    }
    return NULL;
}

static void border_set(bool show, uint32_t color)
{
    int32_t st = show ? (int32_t)(color & 0xFFFFFF) : -1;
    if (st == s_border_state || !s_border[0]) {
        return;
    }
    s_border_state = st;
    for (int i = 0; i < 4; i++) {
        if (show) {
            lv_obj_set_style_bg_color(s_border[i], lv_color_hex(color), 0);
        }
        set_hidden(s_border[i], !show);
    }
}

/* Cartel abajo con las alarmas de los valores que no estan en la pantalla */
static void banner_update(void)
{
    char text[128] = "";
    size_t len = 0;
    for (int i = 0; i < SENSOR_COUNT && len < sizeof(text) - 1; i++) {
        if (!s_alarm[i] || value_item((sensor_id_t)i, true)) {
            continue;
        }
        const sensor_info_t *info = sensors_info((sensor_id_t)i);
        dash_item_t *vi = value_item((sensor_id_t)i, false);
        const char *name = vi ? item_name(vi) : info->name;
        len += snprintf(text + len, sizeof(text) - len, "%s%s %.*f %s", len ? "    " : "", name,
                        info->decimals, sensors_get((sensor_id_t)i), info->unit);
    }
    set_hidden(s_banner, !text[0]);
    if (text[0] && strcmp(lv_label_get_text(s_banner_label), text) != 0) {
        lv_label_set_text(s_banner_label, text);
    }
}

static void overlay_update(void)
{
    const dash_shift_t *sh = &s_theme->shift;
    float rpm = sensors_get(SENSOR_RPM);
    bool flash = sh->flash_rpm > 0 && rpm >= sh->flash_rpm;
    bool warn = sh->warn_rpm > 0 && rpm >= sh->warn_rpm;
    bool fast_on = (lv_tick_get() / SHIFT_BLINK_MS) % 2 == 0;
    bool any_alarm = false;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        any_alarm |= s_alarm[i];
    }
    if (any_alarm && s_theme->alarm_border) {
        border_set(alarm_blink_on(), ALARM_COLOR);
    } else if ((sh->mode & DASH_SHIFT_BORDER) && flash) {
        border_set(fast_on, sh->color);
    } else if ((sh->mode & DASH_SHIFT_BORDER) && warn) {
        border_set(true, sh->color);
    } else {
        border_set(false, 0);
    }
    s_shift_bar_off = (sh->mode & DASH_SHIFT_BAR) && flash && !fast_on;
    if (s_banner) {
        banner_update();
    }
}

/* Borde y cartel: arriba de todo el dashboard (el menu va por encima, en la capa superior) */
static void create_overlay(lv_obj_t *scr)
{
    s_border_state = -1;
    s_shift_bar_off = false;
    memset(s_alarm, 0, sizeof(s_alarm));
    memset(s_alarm_since, 0, sizeof(s_alarm_since));
    int32_t w = lv_obj_get_width(scr), h = lv_obj_get_height(scr);
    const int32_t pos[4][4] = {
        { 0, 0, w, BORDER_W }, { 0, h - BORDER_W, w, BORDER_W },
        { 0, BORDER_W, BORDER_W, h - 2 * BORDER_W }, { w - BORDER_W, BORDER_W, BORDER_W, h - 2 * BORDER_W },
    };
    for (int i = 0; i < 4; i++) {
        s_border[i] = create_box(scr);
        lv_obj_set_pos(s_border[i], pos[i][0], pos[i][1]);
        lv_obj_set_size(s_border[i], pos[i][2], pos[i][3]);
        lv_obj_set_style_bg_opa(s_border[i], LV_OPA_COVER, 0);
        set_hidden(s_border[i], true);
    }
    s_banner = create_box(scr);
    lv_obj_set_size(s_banner, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(ALARM_COLOR), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_banner, 10, 0);
    lv_obj_set_style_pad_hor(s_banner, 18, 0);
    lv_obj_set_style_pad_ver(s_banner, 8, 0);
    lv_obj_align(s_banner, LV_ALIGN_BOTTOM_MID, 0, -(BORDER_W + 10));
    s_banner_label = lv_label_create(s_banner);
    lv_label_set_text(s_banner_label, "");
    lv_obj_set_style_text_font(s_banner_label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(s_banner_label, lv_color_white(), 0);
    set_hidden(s_banner, true);
}

static void update_item(dash_item_t *it)
{
    const dash_element_t *el = &it->el;
    if (el->hidden) {
        return;
    }
    float v = sensors_get(el->sensor);

    switch (el->type) {
    case DASH_EL_IMAGE:
        break;

    case DASH_EL_NEEDLE: {
        float k = el->smoothing > 0 ? el->smoothing : 0.35f;
        it->shown += (v - it->shown) * k;
        int32_t rot = (int32_t)lroundf(needle_angle(el, it->shown) * 10.0f);   /* 0.1 grados */
        if (rot != it->last_rotation) {
            lv_image_set_rotation(it->obj, rot);
            it->last_rotation = rot;
        }
        break;
    }

    case DASH_EL_DIGITS: {
        int32_t max = 1;
        for (int i = 0; i < el->digit_count; i++) {
            max *= 10;
        }
        int32_t num = (int32_t)lroundf(v);
        num = num < 0 ? 0 : (num >= max ? max - 1 : num);
        if (num == it->last_number) {
            break;
        }
        it->last_number = num;
        /* de derecha a izquierda; los ceros a la izquierda se ocultan */
        for (int i = el->digit_count - 1; i >= 0; i--) {
            bool leading = (num == 0 && i != el->digit_count - 1);
            set_hidden(it->digits[i], leading);
            if (!leading) {
                lv_image_set_src(it->digits[i], el->digit_imgs[num % 10]);
            }
            num /= 10;
        }
        break;
    }

    case DASH_EL_VALUE: {
        const sensor_info_t *info = sensors_info(el->sensor);
        char buf[sizeof(it->last_text)];
        snprintf(buf, sizeof(buf), "%.*f %s", info->decimals, v, el->hide_unit ? "" : info->unit);
        if (strcmp(buf, it->last_text) != 0) {
            strcpy(it->last_text, buf);
            lv_label_set_text(it->value_label, buf);
        }
        int8_t st = s_alarm[el->sensor] ? (alarm_blink_on() ? 1 : 2) : 0;
        if (st != it->alarm_state) {
            set_value_alarm(it, st);
        }
        break;
    }

    case DASH_EL_BAR: {
        float k = el->smoothing > 0 ? el->smoothing : 0.35f;
        it->shown += (v - it->shown) * k;
        int32_t w = lv_obj_get_width(it->obj);
        int32_t h = lv_obj_get_height(it->obj);
        int32_t len = el->vertical ? h : w;
        int32_t px = (int32_t)lroundf(value_fraction(el, it->shown) * len);
        if (px != it->last_number) {
            it->last_number = px;
            if (el->vertical) {
                /* crece hacia arriba: el recorte se pega abajo y la imagen se corre */
                lv_obj_set_size(it->fill, w, px);
                lv_obj_set_pos(it->fill, 0, h - px);
                if (it->fill_img) {
                    lv_obj_set_pos(it->fill_img, 0, px - h);
                }
            } else {
                lv_obj_set_size(it->fill, px, h);
            }
        }
        if (el->sensor == SENSOR_RPM && s_shift_bar_off != it->shift_off) {
            it->shift_off = s_shift_bar_off;    /* shift light: la barra titila */
            set_hidden(it->fill, it->shift_off);
        }
        if (!el->img_on && el->warn_color) {
            int8_t warn = v >= el->warn;
            if (warn != it->last_warn) {
                it->last_warn = warn;
                lv_obj_set_style_bg_color(it->fill, lv_color_hex(warn ? el->warn_color : el->bar_color), 0);
            }
        }
        break;
    }

    case DASH_EL_FRAMES: {
        if (el->smoothing > 0) {
            it->shown += (v - it->shown) * el->smoothing;
            v = it->shown;
        }
        int32_t idx = frame_index(el, v);
        if (it->frame_cache && it->frame_step > 1) {
            idx -= idx % it->frame_step;
        }
        if (idx == it->last_number) {
            break;
        }
        if (it->frame_cache) {
            it->last_number = idx;
            lv_image_set_src(it->obj, cached_frame(it, idx));
        } else if (el->frame_path_fmt) {
            /* cada cuadro se lee de la SD: se limita la cantidad de cambios por segundo */
            if (lv_tick_elaps(it->frame_tick) >= FRAME_MIN_MS && show_file_frame(it, idx)) {
                it->last_number = idx;
            }
        } else {
            it->last_number = idx;
            lv_image_set_src(it->obj, el->frames[idx]);
        }
        break;
    }
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    alarms_update();
    overlay_update();
    for (size_t i = 0; i < s_item_count; i++) {
        update_item(&s_items[i]);
    }
}

/* ---------- Modo edicion: arrastrar los valores con el dedo ---------- */

static void drag_event_cb(lv_event_t *e)
{
    if (!s_edit) {
        return;
    }
    dash_item_t *it = lv_event_get_user_data(e);
    lv_point_t vect;
    lv_indev_get_vect(lv_indev_active(), &vect);

    /* tamano visible (con la escala) para no salirse de la pantalla */
    int32_t w = lv_obj_get_width(it->obj) * it->scale / SCALE_100;
    int32_t h = lv_obj_get_height(it->obj) * it->scale / SCALE_100;
    int32_t max_x = LV_MAX(0, lv_obj_get_width(lv_obj_get_parent(it->obj)) - w);
    int32_t max_y = LV_MAX(0, lv_obj_get_height(lv_obj_get_parent(it->obj)) - h);
    it->el.x = LV_CLAMP(0, it->el.x + vect.x, max_x);
    it->el.y = LV_CLAMP(0, it->el.y + vect.y, max_y);
    place_group(it);
}

static void mark_selection(void)
{
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        if (it->el.movable) {
            bool sel = (it == s_selected);
            lv_obj_set_style_outline_color(it->obj, lv_color_hex(sel ? SELECTED_COLOR : EDIT_COLOR), 0);
            lv_obj_set_style_outline_width(it->obj, sel ? 4 : 2, 0);
        }
    }
}

static void select_event_cb(lv_event_t *e)
{
    if (s_edit) {
        s_selected = lv_event_get_user_data(e);
        mark_selection();
    }
}

static void resize_selected(int32_t step)
{
    if (!s_selected) {
        return;
    }
    s_selected->scale = LV_CLAMP(SCALE_MIN, s_selected->scale + step, SCALE_MAX);
    place_group(s_selected);
}

/* Colores que se pueden elegir en modo edicion */
static const uint32_t s_palette[] = {
    0xFFFFFF, 0xFFB020, 0xF0C020, 0xFF3030, 0xFF70B0, 0x30E070, 0x00C8FF, 0x9A9A9A,
};

static void apply_color(dash_item_t *it)
{
    if (it->el.type == DASH_EL_VALUE) {
        lv_obj_set_style_text_color(it->value_label, lv_color_hex(it->el.color), 0);
        if (!it->el.caption_color) {
            lv_obj_set_style_text_color(it->caption_label, lv_color_hex(it->el.color), 0);
        }
        it->alarm_state = -1;   /* si esta en alarma, el proximo refresco lo vuelve a poner rojo */
    } else if (it->el.type == DASH_EL_BAR && !it->el.img_on) {
        lv_obj_set_style_bg_color(it->fill, lv_color_hex(it->el.bar_color), 0);
        it->last_warn = -1;     /* que el proximo refresco revise el color de alarma */
    }
}

/* Cambia el color del elemento elegido: el texto de los valores y, si se
 * toca una barra dibujada, el relleno de la barra */
static void color_cb(lv_event_t *e)
{
    if (!s_selected) {
        return;
    }
    uint32_t c = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        if (it->leader != s_selected) {
            continue;
        }
        if (it->el.type == DASH_EL_VALUE) {
            it->el.color = c;
            apply_color(it);
        } else if (it->el.type == DASH_EL_BAR && !it->el.img_on && it == s_selected) {
            it->el.bar_color = c;
            apply_color(it);
        }
    }
}

static void bigger_cb(lv_event_t *e)
{
    resize_selected(SCALE_STEP);
}

static void smaller_cb(lv_event_t *e)
{
    resize_selected(-SCALE_STEP);
}

static void set_edit_mode(bool on)
{
    s_edit = on;
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        if (!it->el.movable) {
            continue;
        }
        if (on) {
            lv_obj_add_flag(it->obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_outline_pad(it->obj, 6, 0);
            if (!s_selected && !it->el.hidden) {
                s_selected = it;
            }
        } else {
            lv_obj_remove_flag(it->obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_outline_width(it->obj, 0, 0);
        }
    }
    if (on) {
        mark_selection();
    } else {
        s_selected = NULL;
    }
    set_hidden(s_edit_bar, !on);
    if (!on) {
        layout_save();
    }
}

static void edit_done_cb(lv_event_t *e)
{
    set_edit_mode(false);
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_pad_hor(btn, 24, 0);
    lv_obj_set_style_pad_ver(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    return btn;
}

static void create_edit_bar(void)
{
    s_edit_bar = create_box(lv_layer_top());
    lv_obj_set_size(s_edit_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(s_edit_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(s_edit_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_edit_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_edit_bar, 14, 0);
    lv_obj_set_style_pad_ver(s_edit_bar, 8, 0);
    lv_obj_set_style_bg_color(s_edit_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_edit_bar, LV_OPA_80, 0);

    for (size_t i = 0; i < sizeof(s_palette) / sizeof(s_palette[0]); i++) {
        lv_obj_t *sw = lv_button_create(s_edit_bar);
        lv_obj_set_size(sw, 40, 40);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(s_palette[i]), 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(0x666666), 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_add_event_cb(sw, color_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)s_palette[i]);
    }
    create_button(s_edit_bar, LV_SYMBOL_MINUS, 0x444444, smaller_cb);
    create_button(s_edit_bar, LV_SYMBOL_PLUS, 0x444444, bigger_cb);
    create_button(s_edit_bar, "LISTO", 0x0078A0, edit_done_cb);
    lv_obj_add_flag(s_edit_bar, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- Menu de ajustes ---------- */

static void menu_theme_cb(lv_event_t *e);
static void menu_reset_cb(lv_event_t *e);

static void menu_usb_cb(lv_event_t *e)
{
    (void)e;
    usb_mode_request();
}

static void menu_close(void)
{
    if (s_menu) {
        lv_obj_delete(s_menu);
        s_menu = NULL;
        lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
        layout_save();
    }
}

static void menu_close_cb(lv_event_t *e)
{
    menu_close();
}

static void menu_move_cb(lv_event_t *e)
{
    menu_close();
    set_edit_mode(true);
}

static void item_switch_cb(lv_event_t *e)
{
    dash_item_t *it = lv_event_get_user_data(e);
    bool hidden = !lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    for (size_t i = 0; i < s_item_count; i++) {
        if (same_group(it, &s_items[i])) {
            s_items[i].el.hidden = hidden;
        }
    }
    apply_settings();
}

static void names_switch_cb(lv_event_t *e)
{
    s_show_names = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    apply_settings();
}

static lv_obj_t *menu_row(lv_obj_t *parent, const char *text, bool checked, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = create_box(parent);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x333333), 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 70, 36);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFF8C00), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return row;
}

static void menu_open(void)
{
    if (s_menu || s_edit) {
        return;
    }
    /* la capa superior bloquea los toques al dashboard mientras el menu esta abierto */
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(lv_layer_top(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_60, 0);

    s_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_menu, 560, 540);
    lv_obj_center(s_menu);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(MENU_BG), 0);
    lv_obj_set_style_border_color(s_menu, lv_color_hex(0xFF8C00), 0);
    lv_obj_set_style_border_width(s_menu, 2, 0);
    lv_obj_set_style_radius(s_menu, 12, 0);
    lv_obj_set_style_pad_all(s_menu, 16, 0);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_menu, 8, 0);
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_menu);
    lv_label_set_text(title, "AJUSTES");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF8C00), 0);

    /* Lista con un interruptor por cada valor que se puede mostrar/ocultar */
    lv_obj_t *list = create_box(s_menu);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_theme_count > 1) {
        lv_obj_t *sub0 = lv_label_create(list);
        lv_label_set_text(sub0, "Dashboard");
        lv_obj_set_style_text_color(sub0, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(sub0, &lv_font_montserrat_16, 0);
        lv_obj_t *themes = create_box(list);
        lv_obj_set_size(themes, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(themes, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_all(themes, 6, 0);
        lv_obj_set_style_pad_gap(themes, 10, 0);
        for (size_t i = 0; i < s_theme_count; i++) {
            bool cur = (i == s_theme_idx);
            lv_obj_t *btn = create_button(themes, s_themes[i]->name, cur ? 0xFF8C00 : 0x333333, menu_theme_cb);
            lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        }
    }

    lv_obj_t *sub = lv_label_create(list);
    lv_label_set_text(sub, "Valores en pantalla");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        if (it->el.movable) {
            menu_row(list, item_name(it), !it->el.hidden, item_switch_cb, it);
        }
    }

    lv_obj_t *sub2 = lv_label_create(list);
    lv_label_set_text(sub2, "Opciones");
    lv_obj_set_style_text_color(sub2, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(sub2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(sub2, 12, 0);
    menu_row(list, "Mostrar nombres", s_show_names, names_switch_cb, NULL);

    /* Modo USB: reinicia y deja la SD como pendrive para la compu */
    lv_obj_t *usb_btn = create_button(list, "Modo USB (cargar dashboards)", 0x2A5DB0, menu_usb_cb);
    lv_obj_set_width(usb_btn, LV_PCT(100));
    lv_obj_set_style_margin_top(usb_btn, 8, 0);

    lv_obj_t *buttons = create_box(s_menu);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    create_button(buttons, "Mover valores", 0x0078A0, menu_move_cb);
    create_button(buttons, "Restablecer", 0x8A1C1C, menu_reset_cb);
    create_button(buttons, "Cerrar", 0x444444, menu_close_cb);
}

/* El menu se abre al soltar despues de mantener presionado; si se abriera en
 * el LONG_PRESSED, el dedo quedaria encima de un boton del menu y lo tocaria. */
static void screen_long_press_cb(lv_event_t *e)
{
    static bool long_pressed;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        long_pressed = true;
    } else if (code == LV_EVENT_RELEASED && long_pressed) {
        long_pressed = false;
        settings_open();   /* menu de configuracion a pantalla completa (settings.c) */
    } else if (code == LV_EVENT_PRESSED) {
        long_pressed = false;
    }
}

/* ---------- Armado del dashboard y cambio de tema ---------- */

static void build(const dash_theme_t *theme)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme->bg_color), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_theme = theme;
    s_item_count = theme->element_count;
    s_items = lv_malloc_zeroed(sizeof(dash_item_t) * s_item_count);
    LV_ASSERT_MALLOC(s_items);
    for (size_t i = 0; i < s_item_count; i++) {
        dash_item_t *it = &s_items[i];
        it->el = theme->elements[i];
        it->scale = it->el.scale ? LV_CLAMP(SCALE_MIN, it->el.scale, SCALE_MAX) : SCALE_100;
        it->leader = it;
        for (size_t j = 0; j < i && it->el.group; j++) {
            if (s_items[j].el.group == it->el.group) {
                it->leader = &s_items[j];
                it->dx0 = it->el.x - s_items[j].el.x;
                it->dy0 = it->el.y - s_items[j].el.y;
                break;
            }
        }
    }
    layout_load();

    for (size_t i = 0; i < s_item_count; i++) {
        create_item(scr, &s_items[i]);
        if (s_items[i].el.movable) {
            lv_obj_add_event_cb(s_items[i].obj, select_event_cb, LV_EVENT_PRESSED, &s_items[i]);
            lv_obj_add_event_cb(s_items[i].obj, drag_event_cb, LV_EVENT_PRESSING, &s_items[i]);
        }
    }
    for (size_t i = 0; i < s_item_count; i++) {
        place_item(&s_items[i]);
    }
    apply_settings();
    create_edit_bar();
    create_overlay(scr);

    lv_obj_add_event_cb(scr, screen_long_press_cb, LV_EVENT_ALL, NULL);

    refresh_timer_cb(NULL);
    s_timer = lv_timer_create(refresh_timer_cb, DASH_REFRESH_MS, NULL);
    if (s_paused) {
        lv_timer_pause(s_timer);
    }
    ESP_LOGI(TAG, "Dashboard '%s' creado con %u elementos", theme->name, (unsigned)s_item_count);
}

static void destroy(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_timer_delete(s_timer);
    s_timer = NULL;
    lv_obj_remove_event_cb(scr, screen_long_press_cb);
    lv_obj_clean(lv_layer_top());
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    lv_obj_clean(scr);
    for (size_t i = 0; i < s_item_count; i++) {
        if (s_items[i].frame_buf) {
            lv_draw_buf_destroy(s_items[i].frame_buf);
        }
        free_frame_cache(&s_items[i]);
    }
    lv_free(s_items);
    s_items = NULL;
    s_item_count = 0;
    s_menu = NULL;
    s_edit_bar = NULL;
    s_selected = NULL;
    s_edit = false;
    memset(s_border, 0, sizeof(s_border));
    s_banner = NULL;
    s_banner_label = NULL;
}

static void save_theme_idx(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_THEME_KEY, (uint8_t)s_theme_idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void switch_theme_async(void *arg)
{
    destroy();
    build(s_themes[s_theme_idx]);
}

/* Restablecer: el primer toque pide confirmacion, el segundo borra lo guardado
 * de este dashboard (posiciones, tamanos, valores visibles) y lo vuelve a armar. */
static void menu_reset_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    if (!lv_obj_get_user_data(btn)) {
        lv_obj_set_user_data(btn, (void *)1);
        lv_label_set_text(lv_obj_get_child(btn, 0), "Seguro?");
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xE02020), 0);
        return;
    }
    char key[16];
    nvs_key(key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Dashboard '%s' restablecido", s_theme->name);
    lv_async_call(switch_theme_async, NULL);
}

static void menu_theme_cb(lv_event_t *e)
{
    size_t idx = (uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (idx == s_theme_idx || idx >= s_theme_count) {
        return;
    }
    layout_save();
    s_theme_idx = idx;
    save_theme_idx();
    /* el boton que llamo esto se borra al reconstruir: se hace despues del evento */
    lv_async_call(switch_theme_async, NULL);
}

void dashboard_menu_open(void)
{
    menu_open();
}

void dashboard_set_paused(bool paused)
{
    s_paused = paused;
    if (!s_timer) {
        return;
    }
    if (paused) {
        lv_timer_pause(s_timer);
    } else {
        lv_timer_resume(s_timer);
        lv_timer_ready(s_timer);    /* refresca ya, sin esperar el proximo ciclo */
    }
}

void dashboard_start(const dash_theme_t *const *themes, size_t theme_count)
{
    s_themes = themes;
    s_theme_count = theme_count;
    s_theme_idx = 0;

    nvs_handle_t h;
    uint8_t idx;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_THEME_KEY, &idx) == ESP_OK && idx < theme_count) {
            s_theme_idx = idx;
        }
        nvs_close(h);
    }

    lv_obj_clean(lv_screen_active());
    build(s_themes[s_theme_idx]);
}
