#pragma once

#include <stdbool.h>

/* Lista de valores que el dashboard puede mostrar. Para agregar uno nuevo,
 * sumarlo aca y en la tabla de sensors.c. */
typedef enum {
    SENSOR_RPM = 0,
    SENSOR_SPEED,
    SENSOR_BOOST,
    SENSOR_WATER_TEMP,
    SENSOR_OIL_TEMP,
    SENSOR_OIL_PRESS,
    SENSOR_BATTERY,
    SENSOR_AFR,
    SENSOR_COUNT
} sensor_id_t;

typedef struct {
    const char *name;   /* nombre corto, se muestra en modo edicion */
    const char *unit;
    int decimals;
} sensor_info_t;

/* Arranca la lectura de sensores. Con simulate = true genera valores falsos
 * para probar la pantalla sin nada conectado. */
void sensors_start(bool simulate);

float sensors_get(sensor_id_t id);
const sensor_info_t *sensors_info(sensor_id_t id);
