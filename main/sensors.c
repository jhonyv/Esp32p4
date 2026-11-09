#include "sensors.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sensors";

static const sensor_info_t s_info[SENSOR_COUNT] = {
    [SENSOR_RPM]        = { "RPM",        "rpm",  0 },
    [SENSOR_SPEED]      = { "VELOCIDAD",  "km/h", 0 },
    [SENSOR_BOOST]      = { "TURBO",      "psi",  1 },
    [SENSOR_WATER_TEMP] = { "TEMP AGUA",  "\xC2\xB0" "C", 0 },
    [SENSOR_OIL_TEMP]   = { "TEMP ACEITE", "\xC2\xB0" "C", 0 },
    [SENSOR_OIL_PRESS]  = { "PRES ACEITE", "bar", 1 },
    [SENSOR_BATTERY]    = { "BATERIA",    "V",    1 },
    [SENSOR_AFR]        = { "AFR",        "",     1 },
};

/* Un float de 32 bits se lee/escribe atomicamente en el P4, alcanza con volatile */
static volatile float s_values[SENSOR_COUNT];

static void sim_task(void *arg)
{
    while (1) {
        float t = esp_timer_get_time() / 1e6f;
        /* Aceleraciones y cambios cada ~8 s */
        float phase = fmodf(t, 8.0f) / 8.0f;
        float throttle = phase < 0.6f ? phase / 0.6f : 1.0f - (phase - 0.6f) / 0.4f;
        float rpm = 900.0f + throttle * 6600.0f;
        s_values[SENSOR_RPM] = rpm;
        s_values[SENSOR_SPEED] = 20.0f + fmodf(t, 40.0f) * 4.0f * throttle;
        s_values[SENSOR_BOOST] = -12.0f + throttle * throttle * 34.0f;
        s_values[SENSOR_WATER_TEMP] = 85.0f + 6.0f * sinf(t * 0.1f);
        s_values[SENSOR_OIL_TEMP] = 95.0f + 8.0f * sinf(t * 0.07f);
        s_values[SENSOR_OIL_PRESS] = 1.2f + rpm / 7500.0f * 4.0f;
        s_values[SENSOR_BATTERY] = 13.8f + 0.2f * sinf(t * 0.5f);
        s_values[SENSOR_AFR] = 14.7f - throttle * 3.0f;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void sensors_start(bool simulate)
{
    if (simulate) {
        ESP_LOGW(TAG, "Usando valores SIMULADOS");
        xTaskCreate(sim_task, "sensors_sim", 3072, NULL, 4, NULL);
    }
    /* TODO: lectura real de los sensores (ADC / entradas digitales) */
}

float sensors_get(sensor_id_t id)
{
    return id < SENSOR_COUNT ? s_values[id] : 0.0f;
}

const sensor_info_t *sensors_info(sensor_id_t id)
{
    return id < SENSOR_COUNT ? &s_info[id] : NULL;
}
