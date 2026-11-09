#pragma once

#include <stdbool.h>

/* Lista de valores que el dashboard puede mostrar. Para agregar uno nuevo,
 * sumarlo aca y en la tabla de sensors.c (y en s_value_keys de sd_themes.c). */
typedef enum {
    SENSOR_RPM = 0,
    SENSOR_SPEED,
    SENSOR_BOOST,
    SENSOR_WATER_TEMP,
    SENSOR_OIL_TEMP,
    SENSOR_OIL_PRESS,
    SENSOR_BATTERY,
    SENSOR_AFR,
    SENSOR_FUEL_PRESS,
    SENSOR_IAT,
    SENSOR_TPS,
    SENSOR_EGT,
    SENSOR_COUNT
} sensor_id_t;

/* De donde sale cada valor. Se elige por sensor en /sdcard/sensores.txt, asi un
 * mismo auto puede mezclar: RPM de la ECU y presion de aceite de un sensor agregado. */
typedef enum {
    SENSOR_SRC_OFF = 0,  /* desactivado */
    SENSOR_SRC_SIM,      /* valores falsos, para probar la pantalla */
    SENSOR_SRC_ANALOG,   /* sensor analogico en una entrada del ADC */
    SENSOR_SRC_PULSE,    /* pulsos: bobina / tacometro (RPM) o VSS (velocidad) */
    SENSOR_SRC_ECU,      /* lo manda la ECU (CAN / OBD-II), ver sensors_set_ecu() */
} sensor_source_t;

typedef struct {
    const char *key;    /* nombre en los archivos de la SD: rpm, agua, turbo... */
    const char *name;   /* nombre corto, se muestra en modo edicion */
    const char *unit;
    int decimals;
} sensor_info_t;

/* Arranca la lectura de sensores. Lee /sdcard/sensores.txt (la SD tiene que estar
 * montada antes). Sin ese archivo, todos los valores quedan simulados como hasta ahora. */
void sensors_start(void);

float sensors_get(sensor_id_t id);
const sensor_info_t *sensors_info(sensor_id_t id);

sensor_source_t sensors_source(sensor_id_t id);
bool sensors_enabled(sensor_id_t id);

/* Voltaje que llega al sensor (ya corregido por el divisor de la entrada), para la
 * pantalla de diagnostico. NAN si el sensor no es analogico. */
float sensors_volts(sensor_id_t id);

/* Para el modulo de CAN / OBD-II: carga un valor que vino de la ECU. Solo se usa si
 * ese sensor tiene fuente "ecu"; si no, se ignora. Se puede llamar desde cualquier tarea. */
void sensors_set_ecu(sensor_id_t id, float value);

/* true si algun sensor esta configurado para leerse de la ECU */
bool sensors_ecu_needed(void);
