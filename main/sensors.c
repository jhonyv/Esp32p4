#include "sensors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "ads1115.h"

static const char *TAG = "sensors";

#define NVS_NS          "sensores"
#define NVS_KEY         "cfg"
#define SETUP_VERSION   1       /* subir si cambia sensor_setup_t */
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

/* Entrada fija de cada valor en la placa de sensores. Los nombres son los que van
 * impresos al lado de cada borne. Ver docs/instalacion_sensores.md. */
static const sensor_input_t s_inputs[SENSOR_COUNT] = {
    [SENSOR_RPM]        = { "RPM",        2,                     INPUT_KIND_PULSE },
    [SENSOR_SPEED]      = { "VELOCIDAD",  3,                     INPUT_KIND_PULSE },
    [SENSOR_BATTERY]    = { "BATERIA",    49,                    INPUT_KIND_BATTERY },
    [SENSOR_WATER_TEMP] = { "AGUA",       51,                    INPUT_KIND_NTC },
    [SENSOR_OIL_TEMP]   = { "ACEITE T",   52,                    INPUT_KIND_NTC },
    [SENSOR_BOOST]      = { "TURBO",      SENSOR_PIN_ADS(0, 0),  INPUT_KIND_5V },
    [SENSOR_OIL_PRESS]  = { "ACEITE P",   SENSOR_PIN_ADS(0, 1),  INPUT_KIND_5V },
    [SENSOR_FUEL_PRESS] = { "NAFTA P",    SENSOR_PIN_ADS(0, 2),  INPUT_KIND_5V },
    [SENSOR_AFR]        = { "AFR",        SENSOR_PIN_ADS(0, 3),  INPUT_KIND_5V },
    [SENSOR_TPS]        = { "TPS",        SENSOR_PIN_ADS(1, 0),  INPUT_KIND_5V },
    [SENSOR_EGT]        = { "EGT",        SENSOR_PIN_ADS(1, 1),  INPUT_KIND_5V },
    [SENSOR_IAT]        = { "ADMISION",   SENSOR_PIN_ADS(1, 2),  INPUT_KIND_NTC },
    /* SENSOR_PIN_ADS(1, 3) ("EXTRA") mide los 5 V que alimentan a los sensores, con el
     * mismo divisor 10k / 20k. Si un sensor la usa, deja de medirlos. */
};

#define REF5V_PIN        SENSOR_PIN_ADS(1, 3)

/* El circuito de cada tipo de entrada */
#define DIVISOR_5V       1.5f    /* 10k arriba, 20k abajo: 5 V -> 3,33 V */
#define DIVISOR_BATTERY  5.7f    /* 47k arriba, 10k abajo: lee hasta ~17 V */
#define NTC_PULLUP       1000.0f /* 1k a 3,3 V: buena resolucion entre 60 y 130 C */
#define NTC_VPULLUP      3.3f

/* Se leen mas seguido (cambian rapido) */
static bool fast_sensor(sensor_id_t id)
{
    return id == SENSOR_BOOST || id == SENSOR_TPS;
}

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

/* Tipos de sensor que se eligen en el menu. Los lineales van de (v1, val1) a (v2, val2)
 * en la unidad indicada; si el sensor se muestra en otra unidad (por ejemplo bar en vez
 * de psi) se convierte solo.
 * OJO: el numero de tipo se guarda en la memoria. Agregar tipos nuevos SOLO al final. */
typedef struct {
    const char *label;
    curve_kind_t kind;
    float v1, val1, v2, val2;
    const char *unit;
    const ntc_pt_t *table;
    int table_len;
} curve_preset_t;

#define NTC(t) CURVE_NTC_TABLE, 0, 0, 0, 0, "c", t, (int)(sizeof(t) / sizeof(t[0]))

static const curve_preset_t s_presets[] = {
    [SENSOR_TYPE_LINEAL] = { "Personalizado (2 puntos)", CURVE_LINEAR, 0, 0, 5, 5, NULL, NULL, 0 },
    [SENSOR_TYPE_NTC]    = { "NTC personalizada (R25 y beta)", CURVE_NTC_BETA, 0, 0, 0, 0, "c", NULL, 0 },
    /* Presion: los comunes de 0,5 a 4,5 V */
    { "Presion 0-100 psi (0,5-4,5 V)", CURVE_LINEAR, 0.5f, 0,    4.5f, 100,  "psi", NULL, 0 },
    { "Presion 0-150 psi (0,5-4,5 V)", CURVE_LINEAR, 0.5f, 0,    4.5f, 150,  "psi", NULL, 0 },
    { "Presion 0-10 bar (0,5-4,5 V)",  CURVE_LINEAR, 0.5f, 0,    4.5f, 10,   "bar", NULL, 0 },
    /* MAP: dan presion absoluta, se pasa a presion de turbo (0 = atmosferica) */
    { "MAP 2 bar (0,5-4,5 V)",         CURVE_LINEAR, 0.5f, 20,   4.5f, 200,  "kpa_abs", NULL, 0 },
    { "MAP 3 bar (0,5-4,5 V)",         CURVE_LINEAR, 0.5f, 20,   4.5f, 300,  "kpa_abs", NULL, 0 },
    { "MAP 4 bar (0,5-4,5 V)",         CURVE_LINEAR, 0.5f, 20,   4.5f, 400,  "kpa_abs", NULL, 0 },
    /* Banda ancha (salida analogica del controlador) */
    { "Banda ancha 0-5 V = 10-20 AFR", CURVE_LINEAR, 0.0f, 10,   5.0f, 20,   "afr", NULL, 0 },
    { "Banda ancha AEM X-Series",      CURVE_LINEAR, 0.5f, 8.5f, 4.5f, 18,   "afr", NULL, 0 },
    { "Banda ancha Innovate LC-2",     CURVE_LINEAR, 0.0f, 7.35f, 5.0f, 22.39f, "afr", NULL, 0 },
    /* Termocupla con amplificador de 0-5 V (5 mV/°C) */
    { "EGT amplificador 5 mV/C",       CURVE_LINEAR, 0.0f, 0,    5.0f, 1000, "c", NULL, 0 },
    /* Mide el voltaje directo (bateria) */
    { "Voltaje directo (bateria)",     CURVE_LINEAR, 0.0f, 0,    1.0f, 1,    "v", NULL, 0 },
    { "Temperatura NTC tipo Bosch",    NTC(s_ntc_bosch) },
    { "Temperatura NTC tipo GM",       NTC(s_ntc_gm) },
};

#define PRESET_COUNT    ((int)(sizeof(s_presets) / sizeof(s_presets[0])))
#define TYPE_PRESS_100PSI   2
#define TYPE_MAP_3BAR       6
#define TYPE_WIDEBAND_10_20 8
#define TYPE_EGT_5MV        11
#define TYPE_VOLTS          12
#define TYPE_NTC_BOSCH      13

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
    int edge_rising;
    float scale;               /* valor = scale / periodo_us */
    uint32_t min_us;           /* pulsos mas cortos que esto son ruido */
    volatile int64_t last_us;
    volatile uint32_t periods[PULSE_AVG];
    volatile uint8_t idx, count, rejects;
    volatile bool restart;     /* estuvo parado: el proximo pulso solo marca el tiempo */
} pulse_t;

typedef struct {
    sensor_setup_t set;        /* lo que se configura desde el menu */
    sensor_source_t src;       /* fuente en uso (OFF si el pin no sirvio) */
    const curve_preset_t *curve;
    adc_unit_t unit;
    adc_channel_t chan;
    adc_cali_handle_t cali;
    bool ads;                  /* entrada de un ADS1115 en vez del ADC del ESP */
    int8_t ads_chip, ads_ch;
    bool have_value;
    pulse_t pulse;
} sensor_cfg_t;

static sensor_cfg_t s_cfg[SENSOR_COUNT];
/* Un float de 32 bits se lee/escribe atomicamente en el P4, alcanza con volatile */
static volatile float s_values[SENSOR_COUNT];
/* Los sensores de presion, MAP y el TPS dan un voltaje proporcional a su alimentacion:
 * si les llegan 4,90 V en vez de 5,00 V marcan 2 % menos. Midiendo esos 5 V se corrige. */
static bool s_ref_on;
static volatile float s_ref5v = NAN;   /* lo que mide, NAN si no se mide o no da */
static float s_ref_ratio = 1.0f;       /* 5,00 / s_ref5v */
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
    const sensor_setup_t *s = &c->set;
    if (volts <= 0.01f || volts >= s->vpullup - 0.01f) {
        return NAN;            /* cable cortado o en corto */
    }
    float r = s->pullup * volts / (s->vpullup - volts);
    if (c->curve->kind == CURVE_NTC_BETA) {
        float inv_t = 1.0f / 298.15f + logf(r / s->r25) / s->beta;
        return 1.0f / inv_t - 273.15f;
    }
    const ntc_pt_t *t = c->curve->table;
    int n = c->curve->table_len;
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
    const curve_preset_t *p = c->curve;
    if (p->kind != CURVE_LINEAR) {
        return ntc_temp(c, volts);
    }
    if (c->set.type == SENSOR_TYPE_LINEAL) {
        /* Personalizado: los dos puntos ya estan en la unidad del sensor */
        const sensor_setup_t *s = &c->set;
        if (s->v2 == s->v1) {
            return s->val1;
        }
        return s->val1 + (volts - s->v1) * (s->val2 - s->val1) / (s->v2 - s->v1);
    }
    float v = p->val1 + (volts - p->v1) * (p->val2 - p->val1) / (p->v2 - p->v1);
    return convert_unit(v, p->unit, s_info[id].unit);
}

static bool analog_read_volts(sensor_cfg_t *c, float *out)
{
    if (c->ads) {
        float v;
        if (!ads_get_volts(c->ads_chip, c->ads_ch, &v)) {
            return false;
        }
        *out = v * c->set.divisor;
        return true;
    }
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
    *out = mv / 1000.0f * c->set.divisor;
    return true;
}

static bool analog_setup(sensor_cfg_t *c, sensor_id_t id)
{
    if (SENSOR_PIN_IS_ADS(c->set.gpio)) {
        c->ads = true;
        c->ads_chip = SENSOR_PIN_ADS_CHIP(c->set.gpio);
        c->ads_ch = SENSOR_PIN_ADS_CH(c->set.gpio);
        ads_use_channel(c->ads_chip, c->ads_ch, fast_sensor(id));
        return true;
    }
    if (adc_oneshot_io_to_channel(c->set.gpio, &c->unit, &c->chan) != ESP_OK) {
        ESP_LOGE(TAG, "%s: el GPIO%d no tiene ADC", s_info[id].key, c->set.gpio);
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
    float pulses = c->set.pulses > 0 ? c->set.pulses : (id == SENSOR_SPEED ? 4000 : 2);
    p->scale = id == SENSOR_SPEED ? 3.6e9f / pulses   /* km/h con pulsos por km */
                                  : 60e6f / pulses;   /* rpm con pulsos por vuelta */
    p->min_us = 200;
    p->edge_rising = c->set.edge_rising;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << c->set.gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* optoacoplador / sensor hall a masa */
        .intr_type = p->edge_rising ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "%s: GPIO%d invalido", s_info[id].key, c->set.gpio);
        return false;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE = ya estaba */
        return false;
    }
    return gpio_isr_handler_add(c->set.gpio, pulse_isr, p) == ESP_OK;
}

/* Los GPIO que salen a las fichas de la placa (Waveshare ESP32-P4 7B). Los demas los
 * usa la placa (pantalla, tactil, audio, SD, WiFi) o no tienen donde soldarse. */
static bool gpio_on_header(int gpio)
{
    static const int8_t pins[] = { 2, 3, 4, 5, 28, 29, 30, 31, 34, 36, 46, 47, 48, 49, 51, 52 };
    for (size_t i = 0; i < sizeof(pins); i++) {
        if (pins[i] == gpio) {
            return true;
        }
    }
    return false;
}

bool sensors_gpio_ok(int gpio, bool analog)
{
    if (SENSOR_PIN_IS_ADS(gpio)) {
        return analog;
    }
    if (!gpio_on_header(gpio)) {
        return false;
    }
    /* ADC del P4: GPIO16-23 y GPIO49-54 (en las fichas: 49, 51 y 52) */
    return !analog || (gpio >= 16 && gpio <= 23) || (gpio >= 49 && gpio <= 54);
}

void sensors_pin_name(int pin, char *buf, int len)
{
    if (SENSOR_PIN_IS_ADS(pin)) {
        snprintf(buf, len, "ADS1115 #%d A%d", SENSOR_PIN_ADS_CHIP(pin) + 1, SENSOR_PIN_ADS_CH(pin));
    } else if (pin >= 0) {
        snprintf(buf, len, "IO%d", pin);
    } else {
        snprintf(buf, len, "sin entrada");
    }
}

const sensor_input_t *sensors_board_input(sensor_id_t id)
{
    return id < SENSOR_COUNT && s_inputs[id].name ? &s_inputs[id] : NULL;
}

void sensors_apply_board_input(sensor_id_t id, sensor_setup_t *s)
{
    const sensor_input_t *in = sensors_board_input(id);
    if (!in) {
        return;
    }
    s->gpio = in->pin;
    switch (in->kind) {
    case INPUT_KIND_5V:
        s->divisor = DIVISOR_5V;
        break;
    case INPUT_KIND_NTC:
        s->divisor = 1.0f;
        s->pullup = NTC_PULLUP;
        s->vpullup = NTC_VPULLUP;
        break;
    case INPUT_KIND_BATTERY:
        s->divisor = DIVISOR_BATTERY;
        break;
    case INPUT_KIND_PULSE:
        s->edge_rising = 0;    /* el optoacoplador tira a masa */
        break;
    }
}

bool sensors_uses_board_input(sensor_id_t id, const sensor_setup_t *s)
{
    sensor_setup_t ref = *s;
    sensors_apply_board_input(id, &ref);
    return ref.gpio == s->gpio && ref.divisor == s->divisor && ref.pullup == s->pullup &&
           ref.vpullup == s->vpullup && ref.edge_rising == s->edge_rising;
}

/* ------------------------------------------------------------------------- */
/* Configuracion guardada en la memoria interna                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint16_t version;
    uint16_t size;             /* sizeof(sensor_setup_t), por si cambia */
    uint8_t count;             /* SENSOR_COUNT cuando se guardo */
    sensor_setup_t setups[SENSOR_COUNT];
} saved_setup_t;

void sensors_default_setup(sensor_id_t id, sensor_setup_t *s)
{
    memset(s, 0, sizeof(*s));
    s->src = SENSOR_SRC_OFF;
    s->gpio = -1;
    s->type = SENSOR_TYPE_LINEAL;
    s->v1 = 0; s->val1 = 0; s->v2 = 5; s->val2 = 100;
    s->divisor = 1.5f;         /* entrada de 5 V con divisor 10k / 20k */
    s->filter = 0.3f;
    s->pullup = 2200;
    s->vpullup = 3.3f;
    s->r25 = 2500;             /* parecido al Bosch */
    s->beta = 3500;
    switch (id) {
    case SENSOR_WATER_TEMP: case SENSOR_OIL_TEMP: case SENSOR_IAT:
        s->type = TYPE_NTC_BOSCH;
        s->filter = 0.05f;     /* las temperaturas cambian lento: mucho filtro */
        break;
    case SENSOR_BATTERY:
        s->type = TYPE_VOLTS;
        s->filter = 0.1f;
        break;
    case SENSOR_BOOST:
        s->type = TYPE_MAP_3BAR;
        break;
    case SENSOR_OIL_PRESS: case SENSOR_FUEL_PRESS:
        s->type = TYPE_PRESS_100PSI;
        break;
    case SENSOR_AFR:
        s->type = TYPE_WIDEBAND_10_20;
        break;
    case SENSOR_EGT:
        s->type = TYPE_EGT_5MV;
        break;
    case SENSOR_TPS:           /* potenciometro tipico: 0,5 V suelto, 4,5 V a fondo */
        s->v1 = 0.5f; s->val1 = 0; s->v2 = 4.5f; s->val2 = 100;
        s->filter = 0.5f;
        break;
    case SENSOR_RPM:
        s->pulses = 2;         /* 4 cilindros, 4 tiempos */
        break;
    case SENSOR_SPEED:
        s->pulses = 4000;
        break;
    default:
        break;
    }
    sensors_apply_board_input(id, s);
}

/* false = nunca se configuro (o la memoria tiene una version vieja) */
static bool load_setup(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    saved_setup_t *saved = calloc(1, sizeof(*saved));
    size_t size = sizeof(*saved);
    bool ok = saved && nvs_get_blob(h, NVS_KEY, saved, &size) == ESP_OK;
    nvs_close(h);
    ok = ok && saved->version == SETUP_VERSION && saved->size == sizeof(sensor_setup_t);
    if (ok) {
        /* Si despues se agregan sensores, los nuevos arrancan apagados */
        size_t n = saved->count < SENSOR_COUNT ? saved->count : SENSOR_COUNT;
        for (size_t i = 0; i < SENSOR_COUNT; i++) {
            if (i < n) {
                s_cfg[i].set = saved->setups[i];
            } else {
                sensors_default_setup(i, &s_cfg[i].set);
            }
        }
    }
    free(saved);
    return ok;
}

esp_err_t sensors_save_setup(const sensor_setup_t setups[SENSOR_COUNT])
{
    saved_setup_t *saved = calloc(1, sizeof(*saved));
    if (!saved) {
        return ESP_ERR_NO_MEM;
    }
    saved->version = SETUP_VERSION;
    saved->size = sizeof(sensor_setup_t);
    saved->count = SENSOR_COUNT;
    memcpy(saved->setups, setups, sizeof(saved->setups));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, saved, sizeof(*saved));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(saved);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo guardar la configuracion: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sensors_erase_setup(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

void sensors_get_setup(sensor_id_t id, sensor_setup_t *out)
{
    if (id < SENSOR_COUNT) {
        *out = s_cfg[id].set;
    }
}

int sensors_type_count(void)
{
    return PRESET_COUNT;
}

const char *sensors_type_label(int type)
{
    return type >= 0 && type < PRESET_COUNT ? s_presets[type].label : "?";
}

bool sensors_type_fits(sensor_id_t id, int type)
{
    if (type < 0 || type >= PRESET_COUNT) {
        return false;
    }
    if (type == SENSOR_TYPE_LINEAL) {
        return id != SENSOR_RPM && id != SENSOR_SPEED;   /* sirve para cualquier analogico */
    }
    const curve_preset_t *p = &s_presets[type];
    bool temp = id == SENSOR_WATER_TEMP || id == SENSOR_OIL_TEMP || id == SENSOR_IAT;
    if (p->kind != CURVE_LINEAR) {
        return temp;                                    /* NTC */
    }
    const char *u = p->unit;
    if (!strcasecmp(u, "kpa_abs")) return id == SENSOR_BOOST;
    if (!strcasecmp(u, "psi") || !strcasecmp(u, "bar")) {
        return id == SENSOR_OIL_PRESS || id == SENSOR_FUEL_PRESS || id == SENSOR_BOOST;
    }
    if (!strcasecmp(u, "afr")) return id == SENSOR_AFR;
    if (!strcasecmp(u, "v")) return id == SENSOR_BATTERY;
    if (!strcasecmp(u, "c")) return id == SENSOR_EGT;
    return false;
}

const char *sensors_problem(sensor_id_t id)
{
    if (id >= SENSOR_COUNT) {
        return NULL;
    }
    const sensor_cfg_t *c = &s_cfg[id];
    if (c->src == SENSOR_SRC_OFF && (c->set.src == SENSOR_SRC_ANALOG || c->set.src == SENSOR_SRC_PULSE)) {
        return "esa entrada no sirve para este sensor";
    }
    if (c->src == SENSOR_SRC_ANALOG && c->ads && !ads_present(c->ads_chip)) {
        return c->ads_chip == 0 ? "no se encuentra el ADS1115 #1 (0x48)"
                                : "no se encuentra el ADS1115 #2 (0x49)";
    }
    return NULL;
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

/* true si el sensor da un voltaje proporcional a su alimentacion (se corrige con s_ref5v) */
static bool ratiometric(const sensor_cfg_t *c, sensor_id_t id)
{
    if (!c->ads) {
        return false;           /* el ADC del ESP: bateria y temperaturas, no van con 5 V */
    }
    if (c->set.type == SENSOR_TYPE_LINEAL) {
        return id == SENSOR_TPS;   /* potenciometro alimentado con los 5 V de la placa */
    }
    const char *u = c->curve->unit;
    return c->curve->kind == CURVE_LINEAR && u &&
           (!strcasecmp(u, "psi") || !strcasecmp(u, "bar") || !strcasecmp(u, "kpa_abs"));
}

static void update_ref5v(void)
{
    float v;
    if (!s_ref_on || !ads_get_volts(1, 3, &v)) {
        s_ref5v = NAN;
        s_ref_ratio = 1.0f;
        return;
    }
    v *= DIVISOR_5V;
    if (v < 4.5f || v > 5.5f) {        /* no esta conectado o algo anda mal: no se corrige */
        s_ref5v = isnan(s_ref5v) ? v : s_ref5v + (v - s_ref5v) * 0.1f;
        s_ref_ratio = 1.0f;
        return;
    }
    s_ref5v = isnan(s_ref5v) || s_ref5v < 4.5f || s_ref5v > 5.5f ? v : s_ref5v + (v - s_ref5v) * 0.1f;
    s_ref_ratio = 5.0f / s_ref5v;
}

float sensors_ref5v(void)
{
    return s_ref5v;
}

static void sensors_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    TickType_t period = pdMS_TO_TICKS(LOOP_MS) ? pdMS_TO_TICKS(LOOP_MS) : 1;
    while (1) {
        ads_poll();            /* trae lo que convirtieron los ADS1115 y arranca lo siguiente */
        update_ref5v();
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
                if (!analog_read_volts(c, &volts)) {
                    s_volts[i] = NAN;          /* sin lectura: el menu lo muestra */
                    break;
                }
                s_volts[i] = volts;
                float v = analog_curve(c, i, ratiometric(c, i) ? volts * s_ref_ratio : volts);
                if (isnan(v)) break;           /* sensor desconectado: queda el ultimo valor */
                float k = c->set.filter > 0 && c->set.filter <= 1 ? c->set.filter : 1;
                s_values[i] = c->have_value ? s_values[i] + (v - s_values[i]) * k : v;
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
    return s <= SENSOR_SRC_ECU ? n[s] : "?";
}

void sensors_start(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        s_volts[i] = NAN;
    }
    if (!load_setup()) {
        ESP_LOGW(TAG, "Sensores sin configurar: todos los valores SIMULADOS");
        for (int i = 0; i < SENSOR_COUNT; i++) {
            sensors_default_setup(i, &s_cfg[i].set);
            s_cfg[i].set.src = SENSOR_SRC_SIM;
        }
    }
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_cfg_t *c = &s_cfg[i];
        c->src = c->set.src <= SENSOR_SRC_ECU ? (sensor_source_t)c->set.src : SENSOR_SRC_OFF;
        if (c->set.type >= PRESET_COUNT) {
            c->set.type = SENSOR_TYPE_LINEAL;
        }
        c->curve = &s_presets[c->set.type];
        if (c->src == SENSOR_SRC_ANALOG || c->src == SENSOR_SRC_PULSE) {
            bool analog = c->src == SENSOR_SRC_ANALOG;
            bool ok = sensors_gpio_ok(c->set.gpio, analog);
            if (!ok) {
                char pin[24];
                sensors_pin_name(c->set.gpio, pin, sizeof(pin));
                ESP_LOGE(TAG, "%s: %s no sirve para esto", s_info[i].key, pin);
            } else {
                ok = analog ? analog_setup(c, i) : pulse_setup(c, i);
            }
            if (!ok) {
                c->src = SENSOR_SRC_OFF;
            }
        }
        if (c->src == SENSOR_SRC_ANALOG || c->src == SENSOR_SRC_PULSE) {
            char pin[24];
            sensors_pin_name(c->set.gpio, pin, sizeof(pin));
            ESP_LOGI(TAG, "%-12s %s en %s", s_info[i].key, src_name(c->src), pin);
        } else if (c->src != SENSOR_SRC_OFF) {
            ESP_LOGI(TAG, "%-12s %s", s_info[i].key, src_name(c->src));
        }
    }
    /* Los 5 V de los sensores se miden si hay sensores que los necesitan en los ADS1115
     * y el canal EXTRA esta libre */
    bool want_ref = false, extra_used = false;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        const sensor_cfg_t *c = &s_cfg[i];
        if (c->src == SENSOR_SRC_ANALOG) {
            want_ref |= ratiometric(c, i);
            extra_used |= c->set.gpio == REF5V_PIN;
        }
    }
    s_ref_on = want_ref && !extra_used;
    if (s_ref_on) {
        ads_use_channel(1, 3, false);
    }
    ads_start();               /* solo busca los ADS1115 si algun sensor los usa */
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
