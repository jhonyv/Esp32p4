#include "sensors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "sensors";

#ifndef CONFIG_PATH
#define CONFIG_PATH     "/sdcard/sensores.txt"
#endif
#define LOOP_MS         10      /* cada cuanto se recalculan los valores */
#define ADC_SAMPLES     4       /* lecturas promediadas por vuelta */
#define PULSE_AVG       4       /* periodos promediados para RPM / velocidad */

static const sensor_info_t s_info[SENSOR_COUNT] = {
    [SENSOR_RPM]        = { "rpm",         "RPM",         "rpm",  0 },
    [SENSOR_SPEED]      = { "velocidad",   "VELOCIDAD",   "km/h", 0 },
    [SENSOR_BOOST]      = { "turbo",       "TURBO",       "psi",  1 },
    [SENSOR_WATER_TEMP] = { "agua",        "TEMP AGUA",   "\xC2\xB0" "C", 0 },
    [SENSOR_OIL_TEMP]   = { "aceite",      "TEMP ACEITE", "\xC2\xB0" "C", 0 },
    [SENSOR_OIL_PRESS]  = { "pres_aceite", "PRES ACEITE", "bar",  1 },
    [SENSOR_BATTERY]    = { "bateria",     "BATERIA",     "V",    1 },
    [SENSOR_AFR]        = { "afr",         "AFR",         "",     1 },
    [SENSOR_FUEL_PRESS] = { "pres_nafta",  "PRES NAFTA",  "bar",  1 },
    [SENSOR_IAT]        = { "admision",    "TEMP ADMISION", "\xC2\xB0" "C", 0 },
    [SENSOR_TPS]        = { "tps",         "ACELERADOR",  "%",    0 },
    [SENSOR_EGT]        = { "egt",         "EGT",         "\xC2\xB0" "C", 0 },
};

/* ------------------------------------------------------------------------- */
/* Curvas: como se pasa de voltios a valor                                    */
/* ------------------------------------------------------------------------- */

typedef enum { CURVE_LINEAR, CURVE_NTC_TABLE, CURVE_NTC_BETA } curve_kind_t;

typedef struct {
    float celsius;
    float ohms;
} ntc_pt_t;

/* Sensor de temperatura tipo Bosch (0 280 130 026 y muchos compatibles) */
static const ntc_pt_t s_ntc_bosch[] = {
    { -40, 45313 }, { -20, 15462 }, { -10, 9397 }, { 0, 5896 }, { 10, 3792 },
    { 20, 2500 }, { 30, 1707 }, { 40, 1175 }, { 50, 834 }, { 60, 596 },
    { 70, 436 }, { 80, 323 }, { 90, 243 }, { 100, 186 }, { 110, 144 },
    { 120, 113 }, { 130, 89 }, { 140, 71 },
};

/* Sensor de temperatura tipo GM (el de muchos autos americanos y de las ECU programables) */
static const ntc_pt_t s_ntc_gm[] = {
    { -40, 100700 }, { -20, 28680 }, { 0, 9420 }, { 10, 5670 }, { 20, 3520 },
    { 30, 2238 }, { 40, 1459 }, { 50, 973 }, { 60, 667 }, { 70, 467 },
    { 80, 332 }, { 90, 241 }, { 100, 177 }, { 110, 132 }, { 120, 100 },
};

/* Tipos de sensor que se eligen con tipo=... en sensores.txt.
 * Los lineales van de (v1, val1) a (v2, val2) en la unidad indicada; si el sensor
 * se muestra en otra unidad (por ejemplo bar en vez de psi) se convierte solo. */
typedef struct {
    const char *name;
    curve_kind_t kind;
    float v1, val1, v2, val2;
    const char *unit;
    const ntc_pt_t *table;
    int table_len;
} curve_preset_t;

#define NTC(t) CURVE_NTC_TABLE, 0, 0, 0, 0, "c", t, (int)(sizeof(t) / sizeof(t[0]))

static const curve_preset_t s_presets[] = {
    /* Presion: los comunes de 0,5 a 4,5 V */
    { "pres_100psi",    CURVE_LINEAR, 0.5f, 0,    4.5f, 100,  "psi", NULL, 0 },
    { "pres_150psi",    CURVE_LINEAR, 0.5f, 0,    4.5f, 150,  "psi", NULL, 0 },
    { "pres_10bar",     CURVE_LINEAR, 0.5f, 0,    4.5f, 10,   "bar", NULL, 0 },
    /* MAP: dan presion absoluta, se pasa a presion de turbo (0 = atmosferica) */
    { "map_2bar",       CURVE_LINEAR, 0.5f, 20,   4.5f, 200,  "kpa_abs", NULL, 0 },
    { "map_3bar",       CURVE_LINEAR, 0.5f, 20,   4.5f, 300,  "kpa_abs", NULL, 0 },
    { "map_4bar",       CURVE_LINEAR, 0.5f, 20,   4.5f, 400,  "kpa_abs", NULL, 0 },
    /* Banda ancha (salida analogica del controlador) */
    { "wideband_10_20", CURVE_LINEAR, 0.0f, 10,   5.0f, 20,   "afr", NULL, 0 },
    { "wideband_aem_x", CURVE_LINEAR, 0.5f, 8.5f, 4.5f, 18,   "afr", NULL, 0 },
    { "wideband_innovate", CURVE_LINEAR, 0.0f, 7.35f, 5.0f, 22.39f, "afr", NULL, 0 },
    /* Termocupla con amplificador de 0-5 V (5 mV/°C) */
    { "egt_5mv",        CURVE_LINEAR, 0.0f, 0,    5.0f, 1000, "c", NULL, 0 },
    /* Mide el voltaje directo (bateria) */
    { "volts",          CURVE_LINEAR, 0.0f, 0,    1.0f, 1,    "v", NULL, 0 },
    { "ntc_bosch",      NTC(s_ntc_bosch) },
    { "ntc_gm",         NTC(s_ntc_gm) },
};

/* Pasa una presion entre psi, bar y kPa absolutos. Lo demas queda igual. */
static bool pressure_to_kpa(const char *unit, float v, float *kpa)
{
    if (!strcasecmp(unit, "psi")) { *kpa = v * 6.89476f; return true; }
    if (!strcasecmp(unit, "bar")) { *kpa = v * 100.0f; return true; }
    if (!strcasecmp(unit, "kpa")) { *kpa = v; return true; }
    if (!strcasecmp(unit, "kpa_abs")) { *kpa = v - 101.325f; return true; }
    return false;
}

static float convert_unit(float v, const char *from, const char *to)
{
    float kpa;
    if (!from || !to || !strcasecmp(from, to) || !pressure_to_kpa(from, v, &kpa)) {
        return v;
    }
    if (!strcasecmp(to, "psi")) return kpa / 6.89476f;
    if (!strcasecmp(to, "bar")) return kpa / 100.0f;
    if (!strcasecmp(to, "kpa")) return kpa;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Estado de cada sensor                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    int gpio;
    int edge_rising;
    float scale;               /* valor = scale / periodo_us */
    uint32_t min_us;           /* pulsos mas cortos que esto son ruido */
    volatile int64_t last_us;
    volatile uint32_t periods[PULSE_AVG];
    volatile uint8_t idx, count, rejects;
    volatile bool restart;     /* estuvo parado: el proximo pulso solo marca el tiempo */
} pulse_t;

typedef struct {
    sensor_source_t src;
    /* analogico */
    int gpio;
    adc_unit_t unit;
    adc_channel_t chan;
    adc_cali_handle_t cali;
    float divisor;             /* voltaje en el sensor = voltaje en el pin * divisor */
    float filter;              /* 0..1: 1 = sin filtro */
    curve_kind_t kind;
    float v1, val1, v2, val2;
    char unit_from[12];
    const ntc_pt_t *table;
    int table_len;
    float pullup, vpullup;     /* NTC: resistencia de pull-up y a que voltaje va */
    float r25, beta;           /* NTC personalizado */
    bool have_value;
    /* pulsos */
    pulse_t pulse;
} sensor_cfg_t;

static sensor_cfg_t s_cfg[SENSOR_COUNT];
/* Un float de 32 bits se lee/escribe atomicamente en el P4, alcanza con volatile */
static volatile float s_values[SENSOR_COUNT];
static volatile float s_volts[SENSOR_COUNT];
static adc_oneshot_unit_handle_t s_adc[2];

/* ------------------------------------------------------------------------- */
/* Pulsos                                                                     */
/* ------------------------------------------------------------------------- */

static void IRAM_ATTR pulse_isr(void *arg)
{
    pulse_t *p = arg;
    int64_t now = esp_timer_get_time();
    int64_t last = p->last_us;
    p->last_us = now;
    if (last == 0 || p->restart || now - last > 2000000) {
        p->restart = false;
        p->rejects = 0;
        p->count = 0;          /* arranca de cero: el primer pulso solo marca el tiempo */
        return;
    }
    uint32_t dt = (uint32_t)(now - last);
    uint32_t prev = p->count ? p->periods[(p->idx + PULSE_AVG - 1) % PULSE_AVG] : 0;
    /* Rebotes de la bobina: pulso demasiado corto o mucho mas corto que el anterior.
     * Si pasa varias veces seguidas es un cambio real y se acepta. */
    if (dt < p->min_us || (prev && dt < prev * 3 / 10 && p->rejects < 3)) {
        p->last_us = last;
        p->rejects++;
        return;
    }
    p->rejects = 0;
    p->periods[p->idx] = dt;
    p->idx = (p->idx + 1) % PULSE_AVG;
    if (p->count < PULSE_AVG) {
        p->count++;
    }
}

static float pulse_value(pulse_t *p)
{
    uint8_t n = p->count;
    if (n == 0 || p->restart) {
        return 0;
    }
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += p->periods[(p->idx + PULSE_AVG - 1 - i) % PULSE_AVG];
    }
    float avg = (float)sum / n;
    /* Sin pulsos un rato = motor parado / auto quieto */
    int64_t timeout = (int64_t)(avg * 4);
    if (timeout < 300000) timeout = 300000;
    if (timeout > 2000000) timeout = 2000000;
    if (esp_timer_get_time() - p->last_us > timeout) {
        p->restart = true;
        return 0;
    }
    return p->scale / avg;
}

/* ------------------------------------------------------------------------- */
/* Analogicos                                                                 */
/* ------------------------------------------------------------------------- */

static float ntc_temp(const sensor_cfg_t *c, float volts)
{
    if (volts <= 0.01f || volts >= c->vpullup - 0.01f) {
        return NAN;            /* cable cortado o en corto */
    }
    float r = c->pullup * volts / (c->vpullup - volts);
    if (c->kind == CURVE_NTC_BETA) {
        float inv_t = 1.0f / 298.15f + logf(r / c->r25) / c->beta;
        return 1.0f / inv_t - 273.15f;
    }
    const ntc_pt_t *t = c->table;
    int n = c->table_len;
    /* La tabla va de frio (mucha resistencia) a caliente (poca) */
    if (r >= t[0].ohms) return t[0].celsius;
    if (r <= t[n - 1].ohms) return t[n - 1].celsius;
    for (int i = 1; i < n; i++) {
        if (r >= t[i].ohms) {
            /* Interpolacion en logaritmo: la NTC es exponencial */
            float f = (logf(t[i - 1].ohms) - logf(r)) / (logf(t[i - 1].ohms) - logf(t[i].ohms));
            return t[i - 1].celsius + f * (t[i].celsius - t[i - 1].celsius);
        }
    }
    return t[n - 1].celsius;
}

static float analog_curve(const sensor_cfg_t *c, sensor_id_t id, float volts)
{
    if (c->kind != CURVE_LINEAR) {
        return ntc_temp(c, volts);
    }
    float v = c->val1 + (volts - c->v1) * (c->val2 - c->val1) / (c->v2 - c->v1);
    return convert_unit(v, c->unit_from, s_info[id].unit);
}

static bool analog_read_volts(sensor_cfg_t *c, float *out)
{
    int sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc[c->unit], c->chan, &raw) != ESP_OK) {
            return false;
        }
        sum += raw;
    }
    int raw = sum / ADC_SAMPLES;
    int mv = 0;
    if (!c->cali || adc_cali_raw_to_voltage(c->cali, raw, &mv) != ESP_OK) {
        mv = raw * 3300 / 4095;  /* sin calibracion: aproximado */
    }
    *out = mv / 1000.0f * c->divisor;
    return true;
}

static bool analog_setup(sensor_cfg_t *c, sensor_id_t id)
{
    if (adc_oneshot_io_to_channel(c->gpio, &c->unit, &c->chan) != ESP_OK) {
        ESP_LOGE(TAG, "%s: el GPIO%d no tiene ADC (en el P4 son GPIO16-23 y GPIO49-54)",
                 s_info[id].key, c->gpio);
        return false;
    }
    if (!s_adc[c->unit]) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = c->unit };
        if (adc_oneshot_new_unit(&ucfg, &s_adc[c->unit]) != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo iniciar el ADC%d", c->unit + 1);
            return false;
        }
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc[c->unit], c->chan, &ccfg) != ESP_OK) {
        return false;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = c->unit, .chan = c->chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &c->cali) != ESP_OK) {
        c->cali = NULL;
    }
#endif
    if (!c->cali) {
        ESP_LOGW(TAG, "%s: ADC sin calibracion, el voltaje es aproximado", s_info[id].key);
    }
    return true;
}

static bool pulse_setup(sensor_cfg_t *c, sensor_id_t id)
{
    pulse_t *p = &c->pulse;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << p->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* optoacoplador / sensor hall a masa */
        .intr_type = p->edge_rising ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "%s: GPIO%d invalido", s_info[id].key, p->gpio);
        return false;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE = ya estaba */
        return false;
    }
    return gpio_isr_handler_add(p->gpio, pulse_isr, p) == ESP_OK;
}

/* Pines que ya usa la placa (pantalla, tactil, audio, SD, USB, consola) */
static bool gpio_reserved(int gpio)
{
    static const int used[] = { 7, 8, 9, 10, 11, 12, 13, 24, 25, 26, 27, 32, 33, 37, 38,
                                39, 40, 41, 42, 43, 44, 53 };
    for (size_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        if (used[i] == gpio) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* sensores.txt                                                               */
/* ------------------------------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int sensor_by_key(const char *k)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (!strcasecmp(k, s_info[i].key)) {
            return i;
        }
    }
    return -1;
}

static const curve_preset_t *find_preset(const char *name)
{
    for (size_t i = 0; i < sizeof(s_presets) / sizeof(s_presets[0]); i++) {
        if (!strcasecmp(name, s_presets[i].name)) {
            return &s_presets[i];
        }
    }
    return NULL;
}

static void apply_preset(sensor_cfg_t *c, const curve_preset_t *p)
{
    c->kind = p->kind;
    c->v1 = p->v1; c->val1 = p->val1; c->v2 = p->v2; c->val2 = p->val2;
    strlcpy(c->unit_from, p->unit, sizeof(c->unit_from));
    c->table = p->table;
    c->table_len = p->table_len;
}

/* Valores de arranque de cada sensor (antes de leer sus parametros) */
static void defaults(sensor_cfg_t *c, sensor_id_t id, float divisor)
{
    memset(c, 0, sizeof(*c));
    c->gpio = -1;
    c->divisor = divisor;
    c->pullup = 2200;
    c->vpullup = 3.3f;
    c->r25 = 2500;     /* parecido al Bosch */
    c->beta = 3500;
    c->kind = CURVE_LINEAR;
    c->v1 = 0; c->val1 = 0; c->v2 = 5; c->val2 = 5;
    strlcpy(c->unit_from, s_info[id].unit, sizeof(c->unit_from));
    switch (id) {
    case SENSOR_WATER_TEMP: case SENSOR_OIL_TEMP: case SENSOR_IAT:
        c->filter = 0.05f;   /* las temperaturas cambian lento: mucho filtro */
        apply_preset(c, find_preset("ntc_bosch"));
        break;
    case SENSOR_BATTERY:
        c->filter = 0.1f;
        c->divisor = 11;     /* divisor tipico: 100k + 10k */
        c->kind = CURVE_LINEAR; c->v1 = 0; c->val1 = 0; c->v2 = 1; c->val2 = 1;
        strlcpy(c->unit_from, "v", sizeof(c->unit_from));
        break;
    default:
        c->filter = 0.3f;
        break;
    }
    c->pulse.gpio = -1;
    c->pulse.min_us = 200;
    c->pulse.scale = id == SENSOR_SPEED ? 3.6e9f / 4000.0f   /* 4000 pulsos por km */
                                        : 60e6f / 2.0f;      /* 2 pulsos por vuelta (4 cil.) */
}

static void parse_params(sensor_cfg_t *c, sensor_id_t id, char *rest)
{
    float ppr = 0, ppk = 0;
    for (char *tok = strtok(rest, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char *k = trim(tok), *v = trim(eq + 1);
        float f = strtof(v, NULL);
        if (!strcasecmp(k, "gpio")) { c->gpio = atoi(v); c->pulse.gpio = c->gpio; }
        else if (!strcasecmp(k, "tipo")) {
            const curve_preset_t *preset = find_preset(v);
            bool found = preset != NULL;
            if (preset) apply_preset(c, preset);
            if (!strcasecmp(v, "ntc")) { c->kind = CURVE_NTC_BETA; found = true; }
            if (!strcasecmp(v, "lineal")) { c->kind = CURVE_LINEAR; found = true; }
            if (!found) ESP_LOGW(TAG, "%s: tipo '%s' desconocido", s_info[id].key, v);
        }
        else if (!strcasecmp(k, "v1")) c->v1 = f;
        else if (!strcasecmp(k, "val1")) c->val1 = f;
        else if (!strcasecmp(k, "v2")) c->v2 = f;
        else if (!strcasecmp(k, "val2")) c->val2 = f;
        else if (!strcasecmp(k, "unidad")) strlcpy(c->unit_from, v, sizeof(c->unit_from));
        else if (!strcasecmp(k, "divisor")) c->divisor = f;
        else if (!strcasecmp(k, "filtro")) c->filter = f;
        else if (!strcasecmp(k, "pullup")) c->pullup = f;
        else if (!strcasecmp(k, "vpullup")) c->vpullup = f;
        else if (!strcasecmp(k, "r25")) c->r25 = f;
        else if (!strcasecmp(k, "beta")) c->beta = f;
        else if (!strcasecmp(k, "pulsos_vuelta")) ppr = f;
        else if (!strcasecmp(k, "cilindros")) ppr = f / 2.0f;   /* 4 tiempos, una chispa cada 2 vueltas */
        else if (!strcasecmp(k, "pulsos_km")) ppk = f;
        else if (!strcasecmp(k, "flanco")) c->pulse.edge_rising = !strcasecmp(v, "subida");
        else if (!strcasecmp(k, "min_us")) c->pulse.min_us = (uint32_t)f;
        else ESP_LOGW(TAG, "%s: parametro '%s' desconocido", s_info[id].key, k);
    }
    if (ppr > 0) c->pulse.scale = 60e6f / ppr;
    if (ppk > 0) c->pulse.scale = 3.6e9f / ppk;
    if (c->filter <= 0 || c->filter > 1) c->filter = 1;
    if (c->kind == CURVE_LINEAR && c->v2 == c->v1) c->v2 = c->v1 + 1;
}

static sensor_source_t parse_source(const char *s)
{
    if (!strcasecmp(s, "analog") || !strcasecmp(s, "analogico")) return SENSOR_SRC_ANALOG;
    if (!strcasecmp(s, "pulso") || !strcasecmp(s, "pulsos")) return SENSOR_SRC_PULSE;
    if (!strcasecmp(s, "ecu") || !strcasecmp(s, "can") || !strcasecmp(s, "obd")) return SENSOR_SRC_ECU;
    if (!strcasecmp(s, "sim") || !strcasecmp(s, "simulado")) return SENSOR_SRC_SIM;
    return SENSOR_SRC_OFF;
}

/* false = no hay archivo */
static bool load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        return false;
    }
    char line[256];
    bool simulate_rest = false;
    float divisor = 1.0f;
    bool listed[SENSOR_COUNT] = { 0 };
    /* Primera pasada: opciones generales, que valen para todos los sensores */
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcasecmp(k, "simular")) simulate_rest = !strcasecmp(v, "si");
        else if (!strcasecmp(k, "divisor")) divisor = strtof(v, NULL);
    }
    for (int i = 0; i < SENSOR_COUNT; i++) {
        defaults(&s_cfg[i], i, divisor);
    }
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int id = sensor_by_key(trim(line));
        if (id < 0) {
            continue;          /* simular=, divisor= o una clave que no es de sensor */
        }
        char *rest = eq + 1;
        char *comma = strchr(rest, ',');
        if (comma) *comma = 0;
        s_cfg[id].src = parse_source(trim(rest));
        listed[id] = true;
        if (comma) {
            parse_params(&s_cfg[id], id, comma + 1);
        }
    }
    fclose(f);
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (!listed[i] && simulate_rest) {
            s_cfg[i].src = SENSOR_SRC_SIM;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Tarea de lectura                                                           */
/* ------------------------------------------------------------------------- */

static void simulate(sensor_id_t id, float t)
{
    float phase = fmodf(t, 8.0f) / 8.0f;   /* aceleraciones y cambios cada ~8 s */
    float throttle = phase < 0.6f ? phase / 0.6f : 1.0f - (phase - 0.6f) / 0.4f;
    float rpm = 900.0f + throttle * 6600.0f;
    float boost = -12.0f + throttle * throttle * 34.0f;
    float v = 0;
    switch (id) {
    case SENSOR_RPM:        v = rpm; break;
    case SENSOR_SPEED:      v = 20.0f + fmodf(t, 40.0f) * 4.0f * throttle; break;
    case SENSOR_BOOST:      v = boost; break;
    case SENSOR_WATER_TEMP: v = 85.0f + 6.0f * sinf(t * 0.1f); break;
    case SENSOR_OIL_TEMP:   v = 95.0f + 8.0f * sinf(t * 0.07f); break;
    case SENSOR_OIL_PRESS:  v = 1.2f + rpm / 7500.0f * 4.0f; break;
    case SENSOR_BATTERY:    v = 13.8f + 0.2f * sinf(t * 0.5f); break;
    case SENSOR_AFR:        v = 14.7f - throttle * 3.0f; break;
    case SENSOR_FUEL_PRESS: v = 3.0f + (boost > 0 ? boost * 0.069f : 0); break;
    case SENSOR_IAT:        v = 30.0f + throttle * 15.0f; break;
    case SENSOR_TPS:        v = throttle * 100.0f; break;
    case SENSOR_EGT:        v = 350.0f + throttle * 500.0f; break;
    default: break;
    }
    s_values[id] = v;
}

static void sensors_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    TickType_t period = pdMS_TO_TICKS(LOOP_MS) ? pdMS_TO_TICKS(LOOP_MS) : 1;
    while (1) {
        float t = esp_timer_get_time() / 1e6f;
        for (int i = 0; i < SENSOR_COUNT; i++) {
            sensor_cfg_t *c = &s_cfg[i];
            switch (c->src) {
            case SENSOR_SRC_SIM:
                simulate(i, t);
                break;
            case SENSOR_SRC_PULSE:
                s_values[i] = pulse_value(&c->pulse);
                break;
            case SENSOR_SRC_ANALOG: {
                float volts;
                if (!analog_read_volts(c, &volts)) break;
                s_volts[i] = volts;
                float v = analog_curve(c, i, volts);
                if (isnan(v)) break;           /* sensor desconectado: queda el ultimo valor */
                s_values[i] = c->have_value ? s_values[i] + (v - s_values[i]) * c->filter : v;
                c->have_value = true;
                break;
            }
            default:
                break;
            }
        }
        vTaskDelayUntil(&wake, period);
    }
}

static const char *src_name(sensor_source_t s)
{
    static const char *n[] = { "apagado", "simulado", "analogico", "pulsos", "ecu" };
    return n[s];
}

void sensors_start(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        s_volts[i] = NAN;
    }
    if (!load_config()) {
        ESP_LOGW(TAG, "No hay %s: todos los valores SIMULADOS", CONFIG_PATH);
        for (int i = 0; i < SENSOR_COUNT; i++) {
            defaults(&s_cfg[i], i, 1.0f);
            s_cfg[i].src = SENSOR_SRC_SIM;
        }
    }
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_cfg_t *c = &s_cfg[i];
        if (c->src == SENSOR_SRC_ANALOG || c->src == SENSOR_SRC_PULSE) {
            bool ok = c->gpio >= 0 && !gpio_reserved(c->gpio);
            if (c->gpio >= 0 && gpio_reserved(c->gpio)) {
                ESP_LOGE(TAG, "%s: el GPIO%d lo usa la placa", s_info[i].key, c->gpio);
            } else if (c->gpio < 0) {
                ESP_LOGE(TAG, "%s: falta gpio=", s_info[i].key);
            }
            if (ok) {
                ok = c->src == SENSOR_SRC_ANALOG ? analog_setup(c, i) : pulse_setup(c, i);
            }
            if (!ok) {
                c->src = SENSOR_SRC_OFF;
            }
        }
        if (c->src != SENSOR_SRC_OFF) {
            if (c->src == SENSOR_SRC_ANALOG || c->src == SENSOR_SRC_PULSE) {
                ESP_LOGI(TAG, "%-12s %s en GPIO%d", s_info[i].key, src_name(c->src), c->gpio);
            } else {
                ESP_LOGI(TAG, "%-12s %s", s_info[i].key, src_name(c->src));
            }
        }
    }
    xTaskCreate(sensors_task, "sensors", 4096, NULL, 4, NULL);
}

float sensors_get(sensor_id_t id)
{
    return id < SENSOR_COUNT ? s_values[id] : 0.0f;
}

const sensor_info_t *sensors_info(sensor_id_t id)
{
    return id < SENSOR_COUNT ? &s_info[id] : NULL;
}

sensor_source_t sensors_source(sensor_id_t id)
{
    return id < SENSOR_COUNT ? s_cfg[id].src : SENSOR_SRC_OFF;
}

bool sensors_enabled(sensor_id_t id)
{
    return sensors_source(id) != SENSOR_SRC_OFF;
}

float sensors_volts(sensor_id_t id)
{
    return id < SENSOR_COUNT ? s_volts[id] : NAN;
}

void sensors_set_ecu(sensor_id_t id, float value)
{
    if (id < SENSOR_COUNT && s_cfg[id].src == SENSOR_SRC_ECU) {
        s_values[id] = value;
    }
}

bool sensors_ecu_needed(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (s_cfg[i].src == SENSOR_SRC_ECU) {
            return true;
        }
    }
    return false;
}
