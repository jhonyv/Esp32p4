#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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

/* De donde sale cada valor. Se elige por sensor desde el menu de la pantalla, asi un
 * mismo auto puede mezclar: RPM de la ECU y presion de aceite de un sensor agregado. */
typedef enum {
    SENSOR_SRC_OFF = 0,  /* desactivado */
    SENSOR_SRC_SIM,      /* valores falsos, para probar la pantalla */
    SENSOR_SRC_ANALOG,   /* sensor analogico en una entrada del ADC */
    SENSOR_SRC_PULSE,    /* pulsos: bobina / tacometro (RPM) o VSS (velocidad) */
    SENSOR_SRC_ECU,      /* lo manda la ECU (CAN / OBD-II), ver sensors_set_ecu() */
} sensor_source_t;

typedef struct {
    const char *key;    /* nombre corto en dashboard.txt: rpm, agua, turbo... */
    const char *name;   /* nombre corto, se muestra en modo edicion */
    const char *unit;
    int decimals;
} sensor_info_t;

/* Tipo de sensor analogico (curva voltaje -> valor). Los dos primeros son
 * personalizados; el resto son sensores comunes ya calibrados (ver sensors_type_label). */
#define SENSOR_TYPE_LINEAL  0   /* dos puntos: v1 -> val1, v2 -> val2 */
#define SENSOR_TYPE_NTC     1   /* NTC con r25 y beta */

/* Configuracion de un sensor. Se guarda en la memoria interna (NVS), no en la SD. */
typedef struct {
    uint8_t src;            /* sensor_source_t */
    int8_t gpio;            /* -1 = sin pin */
    uint8_t type;           /* SENSOR_TYPE_xxx o uno de la lista */
    uint8_t edge_rising;    /* pulsos: 0 = flanco de bajada (optoacoplador a masa) */
    float v1, val1, v2, val2;   /* tipo personalizado, en la unidad del sensor */
    float divisor;          /* voltaje en el sensor = voltaje en el pin * divisor */
    float filter;           /* suavizado 0..1 (1 = sin filtro) */
    float pullup, vpullup;  /* NTC: resistencia de pull-up (ohms) y a que voltaje va */
    float r25, beta;        /* NTC personalizada */
    float pulses;           /* rpm: pulsos por vuelta / velocidad: pulsos por km */
} sensor_setup_t;

/* Arranca la lectura de sensores con la configuracion guardada. Si nunca se
 * configuro nada, todos los valores quedan simulados (como hasta ahora). */
void sensors_start(void);

/* Para el menu de sensores */
void sensors_get_setup(sensor_id_t id, sensor_setup_t *out);
void sensors_default_setup(sensor_id_t id, sensor_setup_t *out);
/* Guarda la configuracion de todos los sensores; se aplica al reiniciar la pantalla */
esp_err_t sensors_save_setup(const sensor_setup_t setups[SENSOR_COUNT]);
/* Borra la configuracion guardada (vuelve a todo simulado al reiniciar) */
esp_err_t sensors_erase_setup(void);
int sensors_type_count(void);
const char *sensors_type_label(int type);
/* false si ese GPIO no se puede usar (lo usa la placa, o no tiene ADC y analog = true) */
bool sensors_gpio_ok(int gpio, bool analog);

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
