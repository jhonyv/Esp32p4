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

/* ------------------------------------------------------------------------- */
/* Entradas fijas de la placa de sensores                                     */
/* ------------------------------------------------------------------------- */
/* Cada valor tiene su entrada en la placa, con el circuito ya armado (divisor,
 * pull-up y proteccion), asi en el menu solo se elige que sensor esta conectado.
 * Los canales de los ADS1115 se guardan como "pines" 100 a 107. */
#define SENSOR_PIN_ADS(chip, ch)  (100 + (chip) * 4 + (ch))
#define SENSOR_PIN_IS_ADS(pin)    ((pin) >= 100 && (pin) < 108)
#define SENSOR_PIN_ADS_CHIP(pin)  (((pin) - 100) / 4)
#define SENSOR_PIN_ADS_CH(pin)    (((pin) - 100) % 4)

typedef enum {
    INPUT_KIND_5V = 0,    /* sensor de 0-5 V: divisor 10k arriba / 20k abajo */
    INPUT_KIND_NTC,       /* NTC a masa, pull-up de 1k a 3,3 V */
    INPUT_KIND_BATTERY,   /* divisor 47k / 10k */
    INPUT_KIND_PULSE,     /* optoacoplador (RPM, velocidad) */
} input_kind_t;

typedef struct {
    const char *name;     /* lo que dice la ficha de la placa: "TURBO", "AGUA"... */
    int8_t pin;           /* GPIO o SENSOR_PIN_ADS() */
    uint8_t kind;         /* input_kind_t */
} sensor_input_t;

/* Entrada fija de ese valor en la placa */
const sensor_input_t *sensors_board_input(sensor_id_t id);
/* Pone en s el pin y el circuito (divisor, pull-up) de la entrada fija */
void sensors_apply_board_input(sensor_id_t id, sensor_setup_t *s);
/* true si s usa la entrada fija tal cual (si no, es un cableado a medida) */
bool sensors_uses_board_input(sensor_id_t id, const sensor_setup_t *s);
/* "IO49", "ADS1115 #1 A0"... */
void sensors_pin_name(int pin, char *buf, int len);
/* true si ese tipo de sensor tiene sentido para ese valor (la lista corta del menu) */
bool sensors_type_fits(sensor_id_t id, int type);
/* Si el sensor no se puede leer, por que (por ejemplo "no se encuentra el ADS1115 #1").
 * NULL si anda. */
const char *sensors_problem(sensor_id_t id);

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
/* false si ese pin no se puede usar (no esta en las fichas de la placa, o no tiene ADC y
 * analog = true). Acepta los canales de los ADS1115 (solo analogicos). */
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
