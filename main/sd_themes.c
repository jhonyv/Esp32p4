#include "sd_themes.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "sd_themes";

#define MAX_FRAMES 100
#define MAX_GAUGES 6        /* relojes con aguja por dashboard: reloj1 ... reloj6 */
#define MAX_ANIMS  4        /* animaciones por dashboard: animacion1 ... animacion4 */

#if LV_FONT_MONTSERRAT_48
#define VALUE_FONT (&lv_font_montserrat_48)
#else
#define VALUE_FONT (&lv_font_montserrat_26)
#endif

/* Ubicacion inicial de un elemento: "x,y,tamano%,visible[,color]" en dashboard.txt */
typedef struct {
    bool set;
    int x, y;
    int size_pct;       /* 100 = tamano normal */
    int visible;
    bool has_color;
    uint32_t color;
} sd_place_t;

/* Nombres de los valores en dashboard.txt: valor_velocidad=30,100,100,1 */
static const struct {
    const char *key;
    sensor_id_t id;
} s_value_keys[] = {
    { "valor_rpm", SENSOR_RPM },
    { "valor_velocidad", SENSOR_SPEED },
    { "valor_turbo", SENSOR_BOOST },
    { "valor_agua", SENSOR_WATER_TEMP },
    { "valor_aceite", SENSOR_OIL_TEMP },
    { "valor_pres_aceite", SENSOR_OIL_PRESS },
    { "valor_bateria", SENSOR_BATTERY },
    { "valor_afr", SENSOR_AFR },
    { "valor_pres_nafta", SENSOR_FUEL_PRESS },
    { "valor_admision", SENSOR_IAT },
    { "valor_tps", SENSOR_TPS },
    { "valor_egt", SENSOR_EGT },
};

/* Reloj con aguja (lo arma el armador):
 *   reloj1=rpm,x,y,tamano%,visible   funcion y lugar (esquina de reloj1.png)
 *   reloj1_aguja=cx,cy,px,py         donde gira la aguja sobre el reloj (cx,cy) y
 *                                    el punto de giro dentro de aguja1.png (px,py)
 *   reloj1_escala=0:-135,8000:135    valor:angulo (grados, 0 = como esta la imagen,
 *                                    positivo = sentido horario), de 2 a 8 puntos
 * Archivos: reloj1.png (opcional, sin el la aguja va sola) y aguja1.png */
typedef struct {
    bool set;
    sensor_id_t sensor;
    sd_place_t place;
    bool has_pivot;
    int cx, cy, px, py;
    dash_cal_pt_t cal[DASH_MAX_CAL_POINTS];
    int cal_count;
} sd_gauge_t;

/* Animacion (GIF o fotos) que cambia de foto segun un sensor (la arma el armador):
 *   animacion1=rpm,x,y,tamano%,visible
 *   animacion1_tramos=0:2000:1:2,2000:4000:3:4,4000:8000:5:20
 *       desde:hasta:foto_desde:foto_hasta, contando las fotos desde 1. Un quinto
 *       numero (ej. 0:2000:1:2:6) hace que en ese tramo las fotos pasen solas, 6 por segundo.
 * Archivos: anim1_00.png, anim1_01.png ... (o .jpg). anim1_00 es la foto 1. */
typedef struct {
    bool set;
    sensor_id_t sensor;
    sd_place_t place;
    dash_frame_range_t ranges[DASH_MAX_RANGES];
    int range_count;
} sd_anim_t;

/* Ajustes que se pueden poner en dashboard.txt (una linea "clave=valor" por ajuste) */
typedef struct {
    float rpm_min, rpm_max;         /* rango de la barra de RPM */
    float fondo_min, fondo_max;     /* RPM del primer y del ultimo cuadro del fondo */
    uint32_t color;                 /* color de los valores */
    uint32_t name_color;            /* color de los nombres */
    int nombres;                    /* 1 = mostrar nombres de entrada */
    int font_size;                  /* tamano de los valores con fuente propia (px) */
    int name_font_size;             /* tamano de los nombres con fuente propia (px) */
    sd_place_t bar;                 /* barra=x,y,tamano,visible */
    sd_place_t values[SENSOR_COUNT];
    char names[SENSOR_COUNT][48];   /* nombre_xxx=texto: nombre propio del valor ("" = sin nombre) */
    bool has_name[SENSOR_COUNT];    /* false = el nombre de fabrica */
    dash_alarm_t alarms[SENSOR_COUNT];  /* alarma_xxx=baja_de,sube_de[,rpm] */
    int alarm_border;               /* alarma_borde=1: el borde titila en rojo */
    dash_shift_t shift;             /* shift_aviso / shift_rpm / shift_color / shift_modo / shift_fps */
    sd_place_t shift_img;           /* shift_imagen=x,y,tamano,visible (shift_00.png, ...) */
    sd_gauge_t gauges[MAX_GAUGES];
    sd_anim_t anims[MAX_ANIMS];
    uint32_t sig;                   /* firma del archivo: si cambia, manda la ubicacion nueva */
} sd_config_t;

static bool parse_place(const char *v, sd_place_t *p)
{
    unsigned color;
    int n = sscanf(v, " %d , %d , %d , %d , %x", &p->x, &p->y, &p->size_pct, &p->visible, &color);
    if (n < 2) {
        return false;
    }
    if (n < 3) {
        p->size_pct = 100;
    }
    if (n < 4) {
        p->visible = 1;
    }
    p->has_color = (n == 5);
    p->color = color;
    p->set = true;
    return true;
}

/* "rpm", "velocidad", ... (los mismos nombres que valor_xxx) */
static bool sensor_from_name(const char *name, size_t len, sensor_id_t *out)
{
    for (size_t i = 0; i < sizeof(s_value_keys) / sizeof(s_value_keys[0]); i++) {
        const char *k = s_value_keys[i].key + 6;    /* sin "valor_" */
        if (strlen(k) == len && !strncmp(k, name, len)) {
            *out = s_value_keys[i].id;
            return true;
        }
    }
    return false;
}

/* Numero de un campo; false si esta vacio */
static bool parse_num(const char *s, float *out)
{
    while (*s == ' ') {
        s++;
    }
    char *end;
    float f = strtof(s, &end);
    if (end == s) {
        return false;
    }
    *out = f;
    return true;
}

/* alarma_agua=,105 / alarma_pres_aceite=1,,1500: baja_de, sube_de y desde que RPM avisa.
 * Un campo vacio es sin ese limite */
static void parse_alarm(const char *v, dash_alarm_t *a)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", v);
    char *field[3] = { NULL, NULL, NULL };
    char *p = buf;
    for (int i = 0; i < 3 && p; i++) {
        field[i] = p;
        p = strchr(p, ',');
        if (p) {
            *p++ = 0;
        }
    }
    memset(a, 0, sizeof(*a));
    a->use_low = field[0] && parse_num(field[0], &a->low);
    a->use_high = field[1] && parse_num(field[1], &a->high);
    if (field[2]) {
        parse_num(field[2], &a->rpm_min);
    }
}

/* reloj1=rpm,x,y,tamano,visible / reloj1_aguja=cx,cy,px,py / reloj1_escala=v:a,v:a,... */
static bool parse_gauge_key(const char *k, const char *v, sd_gauge_t *gauges)
{
    int idx = 0, used = 0;
    if (sscanf(k, "reloj%d%n", &idx, &used) != 1 || idx < 1 || idx > MAX_GAUGES) {
        return false;
    }
    sd_gauge_t *g = &gauges[idx - 1];
    const char *suffix = k + used;
    if (!*suffix) {
        const char *comma = strchr(v, ',');
        size_t len = comma ? (size_t)(comma - v) : strlen(v);
        while (len && isspace((unsigned char)v[len - 1])) {
            len--;
        }
        if (comma && sensor_from_name(v, len, &g->sensor) && parse_place(comma + 1, &g->place)) {
            g->set = true;
        }
    } else if (!strcmp(suffix, "_aguja")) {
        g->has_pivot = sscanf(v, " %d , %d , %d , %d", &g->cx, &g->cy, &g->px, &g->py) == 4;
    } else if (!strcmp(suffix, "_escala")) {
        g->cal_count = 0;
        const char *c = v;
        while (*c && g->cal_count < DASH_MAX_CAL_POINTS) {
            float val, ang;
            if (sscanf(c, " %f : %f", &val, &ang) == 2) {
                g->cal[g->cal_count++] = (dash_cal_pt_t) { val, ang };
            }
            c = strchr(c, ',');
            if (!c) {
                break;
            }
            c++;
        }
    }
    return true;
}

/* animacion1=rpm,x,y,tamano,visible / animacion1_tramos=desde:hasta:foto:foto[:fps],... */
static bool parse_anim_key(const char *k, const char *v, sd_anim_t *anims)
{
    int idx = 0, used = 0;
    if (sscanf(k, "animacion%d%n", &idx, &used) != 1 || idx < 1 || idx > MAX_ANIMS) {
        return false;
    }
    sd_anim_t *a = &anims[idx - 1];
    const char *suffix = k + used;
    if (!*suffix) {
        const char *comma = strchr(v, ',');
        size_t len = comma ? (size_t)(comma - v) : strlen(v);
        while (len && isspace((unsigned char)v[len - 1])) {
            len--;
        }
        if (comma && sensor_from_name(v, len, &a->sensor) && parse_place(comma + 1, &a->place)) {
            a->set = true;
        }
    } else if (!strcmp(suffix, "_tramos")) {
        a->range_count = 0;
        const char *c = v;
        while (*c && a->range_count < DASH_MAX_RANGES) {
            float from, to;
            int first, last, fps = 0;
            int got = sscanf(c, " %f : %f : %d : %d : %d", &from, &to, &first, &last, &fps);
            if (got >= 4 && first >= 1 && last >= 1 && to >= from) {
                a->ranges[a->range_count++] = (dash_frame_range_t) {
                    .from = from, .to = to,
                    .first = (uint8_t)(first > 255 ? 255 : first) - 1,
                    .last = (uint8_t)(last > 255 ? 255 : last) - 1,
                    .fps = (uint8_t)(got == 5 && fps > 0 ? (fps > 60 ? 60 : fps) : 0),
                };
            }
            c = strchr(c, ',');
            if (!c) {
                break;
            }
            c++;
        }
        /* en orden por "desde" (el armador ya los escribe asi) */
        for (int i = 1; i < a->range_count; i++) {
            for (int j = i; j > 0 && a->ranges[j].from < a->ranges[j - 1].from; j--) {
                dash_frame_range_t t = a->ranges[j];
                a->ranges[j] = a->ranges[j - 1];
                a->ranges[j - 1] = t;
            }
        }
    }
    return true;
}

/* tamano en % -> escala de LVGL (256 = 100%) */
static uint16_t pct_to_scale(int pct)
{
    if (pct <= 0) {
        return 0;
    }
    return (uint16_t)((pct * 256 + 50) / 100);
}

static bool file_exists(const char *dir, const char *name)
{
    char path[600];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Cuantas fotos seguidas hay con este formato (ej. "anim1_%02d.%s"), y con que extension */
static int count_frames(const char *dir, const char *fmt, const char **ext_out)
{
    char fname[40];
    for (int e = 0; e < 2; e++) {
        const char *ext = e == 0 ? "png" : "jpg";
        int n = 0;
        while (n < MAX_FRAMES) {
            snprintf(fname, sizeof(fname), fmt, n, ext);
            if (!file_exists(dir, fname)) {
                break;
            }
            n++;
        }
        if (n) {
            *ext_out = ext;
            return n;
        }
    }
    return 0;
}

static void read_config(const char *dir, sd_config_t *cfg)
{
    char path[600], line[320];
    snprintf(path, sizeof(path), "%s/dashboard.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    uint32_t sig = 2166136261u;     /* FNV-1a de todo el archivo */
    while (fgets(line, sizeof(line), f)) {
        for (const char *c = line; *c; c++) {
            if (*c != '\r' && *c != '\n') {
                sig = (sig ^ (uint8_t)*c) * 16777619u;
            }
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char *k = line, *v = eq + 1;
        bool is_value = false;
        for (size_t i = 0; i < sizeof(s_value_keys) / sizeof(s_value_keys[0]); i++) {
            if (!strcmp(k, s_value_keys[i].key)) {
                parse_place(v, &cfg->values[s_value_keys[i].id]);
                is_value = true;
            }
        }
        if (is_value) {
            continue;
        }
        /* fin de linea fuera (si no, el \r\n queda pegado al ultimo numero de la escala) */
        char *end = (char *)v + strlen(v);
        while (end > v && (end[-1] == '\r' || end[-1] == '\n')) {
            *--end = 0;
        }
        if (!strncmp(k, "reloj", 5) && parse_gauge_key(k, v, cfg->gauges)) {
            continue;
        }
        if (!strncmp(k, "animacion", 9) && parse_anim_key(k, v, cfg->anims)) {
            continue;
        }
        sensor_id_t alarm_id;
        if (!strcmp(k, "alarma_borde")) {
            cfg->alarm_border = atoi(v);
            continue;
        }
        if (!strncmp(k, "alarma_", 7) && sensor_from_name(k + 7, strlen(k + 7), &alarm_id)) {
            parse_alarm(v, &cfg->alarms[alarm_id]);
            continue;
        }
        if (!strcmp(k, "shift_aviso")) {
            cfg->shift.warn_rpm = strtof(v, NULL);
            continue;
        }
        if (!strcmp(k, "shift_rpm")) {
            cfg->shift.flash_rpm = strtof(v, NULL);
            continue;
        }
        if (!strcmp(k, "shift_color")) {
            cfg->shift.color = strtoul(v, NULL, 16);
            continue;
        }
        if (!strcmp(k, "shift_modo")) {
            /* borde, barra, ambos (borde y barra) o imagen; se pueden juntar: "imagen,borde" */
            uint8_t m = 0;
            if (strstr(v, "borde") || strstr(v, "ambos")) {
                m |= DASH_SHIFT_BORDER;
            }
            if (strstr(v, "barra") || strstr(v, "ambos")) {
                m |= DASH_SHIFT_BAR;
            }
            if (strstr(v, "imagen")) {
                m |= DASH_SHIFT_IMAGE;
            }
            cfg->shift.mode = m ? m : DASH_SHIFT_BORDER;
            continue;
        }
        if (!strcmp(k, "shift_fps")) {
            int fps = atoi(v);
            cfg->shift.fps = (uint8_t)(fps < 1 ? 1 : fps > 60 ? 60 : fps);
            continue;
        }
        if (!strcmp(k, "shift_imagen")) {
            parse_place(v, &cfg->shift_img);
            continue;
        }
        sensor_id_t name_id;
        if (!strncmp(k, "nombre_", 7) && sensor_from_name(k + 7, strlen(k + 7), &name_id)) {
            snprintf(cfg->names[name_id], sizeof(cfg->names[name_id]), "%s", v);
            cfg->has_name[name_id] = true;
            continue;
        }
        if (!strcmp(k, "barra")) {
            parse_place(v, &cfg->bar);
        } else if (!strcmp(k, "rpm_min")) {
            cfg->rpm_min = strtof(v, NULL);
        } else if (!strcmp(k, "rpm_max")) {
            cfg->rpm_max = strtof(v, NULL);
        } else if (!strcmp(k, "fondo_rpm_min")) {
            cfg->fondo_min = strtof(v, NULL);
        } else if (!strcmp(k, "fondo_rpm_max")) {
            cfg->fondo_max = strtof(v, NULL);
        } else if (!strcmp(k, "color")) {
            cfg->color = strtoul(v, NULL, 16);
        } else if (!strcmp(k, "color_nombres")) {
            cfg->name_color = strtoul(v, NULL, 16);
        } else if (!strcmp(k, "nombres")) {
            cfg->nombres = atoi(v);
        } else if (!strcmp(k, "fuente_tamano")) {
            cfg->font_size = atoi(v);
        } else if (!strcmp(k, "fuente_nombres_tamano")) {
            cfg->name_font_size = atoi(v);
        }
    }
    fclose(f);
    cfg->sig = sig;
}

static const lv_image_dsc_t *load(const char *name, const char *file)
{
    char path[600];
    snprintf(path, sizeof(path), SD_THEMES_LV_DIR "/%s/%s", name, file);
    return (const lv_image_dsc_t *)dash_image_load(path);
}

/* Fuente propia: fuente.ttf (o el primer .ttf / .otf de la carpeta).
 * Devuelve el nombre del archivo en out, o false si no hay. */
static bool find_font(const char *dir, char *out, size_t len)
{
    static const char *const preferred[] = { "fuente.ttf", "fuente.otf" };
    for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
        if (file_exists(dir, preferred[i])) {
            snprintf(out, len, "%s", preferred[i]);
            return true;
        }
    }
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || ent->d_name[0] == '.' || strlen(dot) != 4) {
            continue;
        }
        char ext[4];
        for (int i = 0; i < 3; i++) {
            ext[i] = (char)tolower((unsigned char)dot[1 + i]);
        }
        ext[3] = 0;
        if (!strcmp(ext, "ttf") || !strcmp(ext, "otf")) {
            snprintf(out, len, "%s", ent->d_name);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* Lee el archivo entero a memoria (PSRAM). La fuente lo usa mientras exista, no se libera. */
static void *read_file(const char *dir, const char *name, size_t *size)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = len > 0 ? malloc(len) : NULL;
    if (data && fread(data, 1, len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)len : 0;
    return data;
}

typedef struct {
    const lv_font_t *value;     /* NULL = fuente de fabrica */
    const lv_font_t *caption;
} sd_fonts_t;

static sd_fonts_t load_fonts(const char *name, const char *dir, const sd_config_t *cfg)
{
    sd_fonts_t fonts = { 0 };
    char file[260];
    if (!find_font(dir, file, sizeof(file))) {
        return fonts;
    }
#if LV_USE_TINY_TTF
    size_t size;
    void *data = read_file(dir, file, &size);
    if (!data) {
        ESP_LOGW(TAG, "'%s': no se pudo leer %s", name, file);
        return fonts;
    }
    lv_font_t *value = lv_tiny_ttf_create_data(data, size, cfg->font_size);
    if (!value) {
        ESP_LOGW(TAG, "'%s': %s no es una fuente valida", name, file);
        free(data);
        return fonts;
    }
    /* si a la fuente le falta un caracter (ej. el simbolo de grado) se usa la de fabrica */
    value->fallback = VALUE_FONT;
    fonts.value = value;
    lv_font_t *caption = lv_tiny_ttf_create_data(data, size, cfg->name_font_size);
    if (caption) {
        caption->fallback = &lv_font_montserrat_16;
        fonts.caption = caption;
    }
    ESP_LOGI(TAG, "'%s': fuente %s (%d px, nombres %d px)", name, file, cfg->font_size, cfg->name_font_size);
#else
    ESP_LOGW(TAG, "'%s': tiene %s pero falta CONFIG_LV_USE_TINY_TTF", name, file);
#endif
    return fonts;
}

/* Arma un tema a partir de una carpeta; NULL si no tiene nada reconocible */
static dash_theme_t *build_theme(const char *name)
{
    char dir[300];
    snprintf(dir, sizeof(dir), SD_THEMES_DIR "/%s", name);

    sd_config_t cfg = {
        .rpm_min = 0, .rpm_max = 8000, .fondo_min = 800, .fondo_max = 7500,
        .color = 0xFFFFFF, .name_color = 0, .nombres = 1,
        .font_size = 56, .name_font_size = 22, .alarm_border = 1,
        .shift = { .color = 0x2080FF, .mode = DASH_SHIFT_BORDER },
    };
    read_config(dir, &cfg);
    if (cfg.font_size < 8 || cfg.font_size > 300) {
        cfg.font_size = 56;
    }
    if (cfg.name_font_size < 8 || cfg.name_font_size > 150) {
        cfg.name_font_size = 22;
    }

    /* fondo animado: fondo_00.jpg, fondo_01.jpg, ... (o .png) */
    const char *frame_ext = NULL;
    int frames = 0;
    char fname[32];
    for (int e = 0; e < 2 && !frames; e++) {
        const char *ext = e == 0 ? "jpg" : "png";
        while (frames < MAX_FRAMES) {
            snprintf(fname, sizeof(fname), "fondo_%02d.%s", frames, ext);
            if (!file_exists(dir, fname)) {
                break;
            }
            frames++;
        }
        if (frames) {
            frame_ext = ext;
        }
    }
    const char *bg_file = file_exists(dir, "fondo.jpg") ? "fondo.jpg" :
                          file_exists(dir, "fondo.png") ? "fondo.png" : NULL;
    bool has_bar = file_exists(dir, "bar.png");
    bool has_needle = false;
    for (int g = 1; g <= MAX_GAUGES && !has_needle; g++) {
        snprintf(fname, sizeof(fname), "aguja%d.png", g);
        has_needle = file_exists(dir, fname);
    }
    int anim_frames[MAX_ANIMS] = { 0 };
    const char *anim_ext[MAX_ANIMS] = { 0 };
    bool has_anim = false;
    for (int a = 0; a < MAX_ANIMS; a++) {
        char fmt[24];
        snprintf(fmt, sizeof(fmt), "anim%d_%%02d.%%s", a + 1);
        anim_frames[a] = count_frames(dir, fmt, &anim_ext[a]);
        has_anim |= anim_frames[a] > 0;
    }
    const char *shift_ext = NULL;
    int shift_frames = count_frames(dir, "shift_%02d.%s", &shift_ext);

    if (!frames && !bg_file && !has_bar && !has_needle && !has_anim) {
        ESP_LOGW(TAG, "'%s': no tiene fondo, bar.png, relojes ni animaciones, se ignora", name);
        return NULL;
    }

    dash_element_t *els = calloc(3 + MAX_ANIMS + 2 * MAX_GAUGES + SENSOR_COUNT, sizeof(dash_element_t));
    dash_theme_t *th = calloc(1, sizeof(dash_theme_t));
    if (!els || !th) {
        free(els);
        free(th);
        return NULL;
    }
    size_t n = 0;

    /* 1. Fondo (siempre primero, queda detras de todo) */
    if (frames) {
        char *fmt = malloc(320);
        snprintf(fmt, 320, SD_THEMES_LV_DIR "/%s/fondo_%%02u.%s", name, frame_ext);
        els[n++] = (dash_element_t) {
            .type = DASH_EL_FRAMES, .sensor = SENSOR_RPM, .frame_path_fmt = fmt,
            .frame_count = frames, .min = cfg.fondo_min, .max = cfg.fondo_max,
        };
    } else if (bg_file) {
        const lv_image_dsc_t *img = load(name, bg_file);
        if (img) {
            els[n++] = (dash_element_t) { .type = DASH_EL_IMAGE, .img = img };
        }
    }

    /* 2. Animaciones: fotos que cambian segun un sensor (quedan detras de la barra y los relojes) */
    int anims = 0;
    for (int a = 0; a < MAX_ANIMS; a++) {
        const sd_anim_t *ac = &cfg.anims[a];
        int count = anim_frames[a];
        if (!ac->set || !count) {
            if (ac->set || count) {
                ESP_LOGW(TAG, "'%s': animacion%d incompleta (faltan anim%d_00.png o la linea animacion%d=)",
                         name, a + 1, a + 1, a + 1);
            }
            continue;
        }
        char *fmt = malloc(320);
        if (!fmt) {
            continue;
        }
        snprintf(fmt, 320, SD_THEMES_LV_DIR "/%s/anim%d_%%02u.%s", name, a + 1, anim_ext[a]);
        char caption[40];
        snprintf(caption, sizeof(caption), "Animacion %s", sensors_info(ac->sensor)->name);
        dash_element_t *el = &els[n++];
        *el = (dash_element_t) {
            .type = DASH_EL_FRAMES, .sensor = ac->sensor, .movable = true, .caption = strdup(caption),
            .x = ac->place.x, .y = ac->place.y, .scale = pct_to_scale(ac->place.size_pct),
            .hidden = !ac->place.visible, .frame_path_fmt = fmt, .frame_count = (uint8_t)count,
            .preload = true, .smoothing = 0.5f, .min = cfg.rpm_min, .max = cfg.rpm_max,
        };
        for (int r = 0; r < ac->range_count; r++) {
            dash_frame_range_t rg = ac->ranges[r];
            rg.first = rg.first >= count ? count - 1 : rg.first;
            rg.last = rg.last >= count ? count - 1 : rg.last;
            el->ranges[el->range_count++] = rg;
        }
        anims++;
    }

    /* 3. Barra de RPM: bar.png encendida, bar_black.png apagada (opcional) */
    if (has_bar) {
        const lv_image_dsc_t *on = load(name, "bar.png");
        const lv_image_dsc_t *off = file_exists(dir, "bar_black.png") ? load(name, "bar_black.png") : NULL;
        if (on) {
            int16_t x = on->header.w < 1024 ? (1024 - on->header.w) / 2 : 0;
            els[n] = (dash_element_t) {
                .type = DASH_EL_BAR, .sensor = SENSOR_RPM, .movable = true, .caption = "Barra RPM",
                .x = x, .y = 10, .img_on = on, .img_off = off,
                .min = cfg.rpm_min, .max = cfg.rpm_max, .smoothing = 0.5f,
            };
            if (cfg.bar.set) {
                els[n].x = cfg.bar.x;
                els[n].y = cfg.bar.y;
                els[n].scale = pct_to_scale(cfg.bar.size_pct);
                els[n].hidden = !cfg.bar.visible;
            }
            n++;
        }
    }

    /* 4. Relojes con aguja: reloj y aguja forman un grupo (se mueven, agrandan y ocultan juntos) */
    int gauges = 0;
    for (int g = 0; g < MAX_GAUGES; g++) {
        const sd_gauge_t *gc = &cfg.gauges[g];
        char dial_file[16], needle_file[16];
        snprintf(dial_file, sizeof(dial_file), "reloj%d.png", g + 1);
        snprintf(needle_file, sizeof(needle_file), "aguja%d.png", g + 1);
        if (!gc->set || !gc->has_pivot || gc->cal_count < 2 || !file_exists(dir, needle_file)) {
            if (gc->set || file_exists(dir, needle_file)) {
                ESP_LOGW(TAG, "'%s': reloj%d incompleto (faltan aguja%d.png, reloj%d_aguja o reloj%d_escala)",
                         name, g + 1, g + 1, g + 1, g + 1);
            }
            continue;
        }
        const lv_image_dsc_t *needle = load(name, needle_file);
        const lv_image_dsc_t *dial = file_exists(dir, dial_file) ? load(name, dial_file) : NULL;
        if (!needle) {
            continue;
        }
        char caption[40];
        snprintf(caption, sizeof(caption), "Reloj %s", sensors_info(gc->sensor)->name);
        uint16_t scale = pct_to_scale(gc->place.size_pct);
        bool hidden = !gc->place.visible;
        uint8_t group = dial ? (uint8_t)(g + 1) : 0;
        if (dial) {
            els[n++] = (dash_element_t) {
                .type = DASH_EL_IMAGE, .movable = true, .group = group, .caption = strdup(caption),
                .x = gc->place.x, .y = gc->place.y, .scale = scale, .hidden = hidden, .img = dial,
            };
        }
        dash_element_t nd = {
            .type = DASH_EL_NEEDLE, .sensor = gc->sensor, .group = group, .hidden = hidden,
            .movable = !dial, .caption = dial ? NULL : strdup(caption), .scale = scale,
            .x = gc->place.x + gc->cx, .y = gc->place.y + gc->cy,
            .img = needle, .pivot_x = gc->px, .pivot_y = gc->py, .cal_count = gc->cal_count,
            .smoothing = gc->sensor == SENSOR_RPM ? 0.45f : 0.35f,
        };
        memcpy(nd.cal, gc->cal, sizeof(nd.cal));
        els[n++] = nd;
        gauges++;
    }

    /* 5. Un valor en digitos por sensor, en dos columnas. Se acomodan desde la pantalla */
    sd_fonts_t fonts = load_fonts(name, dir, &cfg);
    static const sensor_id_t order[] = {
        SENSOR_SPEED, SENSOR_WATER_TEMP, SENSOR_OIL_TEMP, SENSOR_OIL_PRESS,
        SENSOR_BOOST, SENSOR_BATTERY, SENSOR_AFR, SENSOR_RPM,
        /* Los nuevos solo aparecen si dashboard.txt los ubica (valor_pres_nafta=...),
         * asi no se pierde lo que ya estaba acomodado en los dashboards de antes */
        SENSOR_FUEL_PRESS, SENSOR_IAT, SENSOR_TPS, SENSOR_EGT,
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        sensor_id_t id = order[i];
        if (i >= 8 && !cfg.values[id].set) {
            continue;
        }
        els[n++] = (dash_element_t) {
            .type = DASH_EL_VALUE, .sensor = id, .movable = true,
            .hidden = (id == SENSOR_AFR || id == SENSOR_RPM),
            .x = (i % 8) < 4 ? 30 : 760, .y = 100 + (i % 4) * 120,
            .font = fonts.value ? fonts.value : VALUE_FONT, .caption_font = fonts.caption,
            .color = cfg.color, .caption_color = cfg.name_color,
            .caption = cfg.has_name[id] ? strdup(cfg.names[id]) : sensors_info(id)->name,
        };
        const sd_place_t *p = &cfg.values[id];
        if (p->set) {
            els[n - 1].x = p->x;
            els[n - 1].y = p->y;
            els[n - 1].scale = pct_to_scale(p->size_pct);
            els[n - 1].hidden = !p->visible;
            if (p->has_color) {
                els[n - 1].color = p->color;
            }
        }
    }

    /* 6. Imagen o GIF del shift light: arriba de todo (shift_00.png, shift_01.png, ...) */
    bool shift_img = false;
    if ((cfg.shift.mode & DASH_SHIFT_IMAGE) && shift_frames && cfg.shift.flash_rpm + cfg.shift.warn_rpm > 0) {
        char *fmt = malloc(320);
        if (fmt) {
            snprintf(fmt, 320, SD_THEMES_LV_DIR "/%s/shift_%%02u.%s", name, shift_ext);
            const sd_place_t *p = &cfg.shift_img;
            int x = p->x, y = p->y;
            if (!p->set) {
                /* sin shift_imagen=: centrada */
                char path[340];
                lv_image_header_t hd;
                snprintf(path, sizeof(path), fmt, 0u);
                bool ok = lv_image_decoder_get_info(path, &hd) == LV_RESULT_OK;
                x = ok && hd.w < 1024 ? (1024 - hd.w) / 2 : 0;
                y = ok && hd.h < 600 ? (600 - hd.h) / 2 : 0;
            }
            els[n++] = (dash_element_t) {
                .type = DASH_EL_FRAMES, .sensor = SENSOR_RPM, .movable = true, .caption = "Shift light",
                .x = x, .y = y, .scale = p->set ? pct_to_scale(p->size_pct) : 0,
                .hidden = p->set && !p->visible, .frame_path_fmt = fmt,
                .frame_count = (uint8_t)shift_frames, .preload = true, .shift_light = true,
                .min = cfg.rpm_min, .max = cfg.rpm_max,
            };
            shift_img = true;
        }
    } else if (cfg.shift.mode & DASH_SHIFT_IMAGE) {
        ESP_LOGW(TAG, "'%s': shift_modo=imagen pero %s", name,
                 shift_frames ? "falta shift_rpm o shift_aviso" : "faltan shift_00.png (o .jpg)");
    }

    th->name = strdup(name);
    th->bg_color = 0x000000;
    th->show_names = cfg.nombres != 0;
    th->layout_sig = cfg.sig;
    th->elements = els;
    th->element_count = n;
    memcpy(th->alarms, cfg.alarms, sizeof(th->alarms));
    th->alarm_border = cfg.alarm_border != 0;
    th->shift = cfg.shift;
    int alarms = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        alarms += cfg.alarms[i].use_low || cfg.alarms[i].use_high;
    }
    if (alarms || cfg.shift.flash_rpm > 0) {
        ESP_LOGI(TAG, "'%s': %d alarmas, shift light %s%s", name, alarms,
                 cfg.shift.flash_rpm > 0 ? "si" : "no",
                 shift_img ? (shift_frames > 1 ? " (GIF)" : " (imagen)") : "");
    }
    ESP_LOGI(TAG, "'%s': %d cuadros de fondo, fondo fijo %s, barra %s, %d animaciones, %d relojes, fuente %s",
             name, frames, bg_file ? bg_file : "no", has_bar ? "si" : "no", anims, gauges,
             fonts.value ? "propia" : "de fabrica");
    return th;
}

size_t sd_themes_load(const dash_theme_t **out, size_t max)
{
    DIR *d = opendir(SD_THEMES_DIR);
    if (!d) {
        ESP_LOGW(TAG, "No hay carpeta %s en la SD", SD_THEMES_DIR);
        return 0;
    }
    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char dir[300];
        struct stat st;
        snprintf(dir, sizeof(dir), SD_THEMES_DIR "/%s", ent->d_name);
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        dash_theme_t *th = build_theme(ent->d_name);
        if (th) {
            out[count++] = th;
        }
    }
    closedir(d);
    return count;
}
