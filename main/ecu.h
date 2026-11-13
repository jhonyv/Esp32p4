#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sensors.h"

/* Lectura de la ECU por CAN. Dos formas:
 *  - OBD-II (ISO 15765-4): autos de fabrica. Se le pregunta dato por dato.
 *  - MegaSquirt (MS2/Extra 3.4+, MS3 1.4+, MicroSquirt): la ECU transmite sola
 *    ("CAN realtime data broadcasting"), solo hay que escuchar.
 *
 * Hace falta un transceptor CAN de 3,3 V (SN65HVD230 o similar) entre los pines
 * TX/RX del ESP32-P4 y CAN-H / CAN-L (en OBD: pines 6 y 14 del conector).
 *
 * OBD-II: detecta sola la velocidad del bus (500 o 250 kbit/s) y si usa IDs de 11 o
 * 29 bits, pregunta que datos informa la ECU y despues los pide en ronda, con las RPM
 * en uno de cada dos pedidos para que la aguja se mueva fluida.
 * MegaSquirt: 500 kbit/s; el formato (dash simplificado o avanzado) y el ID base
 * se detectan solos.
 * Los valores van a los sensores que tienen fuente "ECU" (sensors_set_ecu). */

typedef enum {
    ECU_MODE_OFF = 0,
    ECU_MODE_OBD_CAN,
    ECU_MODE_MEGASQUIRT,
} ecu_mode_t;

/* Los dos formatos que transmite la MegaSquirt (TunerStudio > CAN-bus/Testmodes) */
typedef enum {
    MS_FORMAT_AUTO = 0,
    MS_FORMAT_DASH,      /* "Dash broadcasting" simplificado: 5 mensajes, base 1512 */
    MS_FORMAT_ADVANCED,  /* "Realtime data broadcasting": grupos elegidos, base 1520 */
} ms_format_t;

typedef struct {
    uint8_t mode;       /* ecu_mode_t */
    int8_t tx_gpio;     /* -1 = sin elegir */
    int8_t rx_gpio;
    uint8_t ms_format;  /* MegaSquirt: ms_format_t */
    uint16_t ms_base;   /* MegaSquirt: ID base del broadcast, 0 = detectarlo solo */
} ecu_config_t;

typedef enum {
    ECU_STATE_OFF = 0,      /* desactivada o sin pines */
    ECU_STATE_SEARCHING,    /* buscando la velocidad del bus / esperando respuesta */
    ECU_STATE_CONNECTED,    /* la ECU contesta */
    ECU_STATE_ERROR,        /* no se pudo iniciar el CAN */
} ecu_state_t;

typedef struct {
    ecu_state_t state;
    uint32_t bitrate;       /* 500000 / 250000, 0 si no se sabe */
    bool ext_id;            /* true = IDs de 29 bits */
    uint16_t responses_per_s;
    uint8_t pid_count;      /* OBD: cuantos PIDs dice que soporta */
    uint16_t ms_base;       /* MegaSquirt: ID base en uso (0 = todavia no se sabe) */
    uint8_t ms_format;      /* MegaSquirt: formato detectado */
} ecu_status_t;

void ecu_get_config(ecu_config_t *out);
esp_err_t ecu_save_config(const ecu_config_t *cfg);

/* Arranca con la configuracion guardada (no hace nada si esta apagada) */
void ecu_start(void);

void ecu_get_status(ecu_status_t *out);

/* true si la ECU conectada informa ese valor */
bool ecu_supports(sensor_id_t id);

/* Ultimo valor que mando la ECU para ese sensor (aunque el sensor no use la ECU).
 * false si nunca llego. */
bool ecu_get_value(sensor_id_t id, float *out);

/* Decodifica una respuesta de modo 01 (datos A, B... despues del PID).
 * Publica para poder probarla en la compu. baro_kpa: presion atmosferica. */
bool ecu_decode_pid(uint8_t pid, const uint8_t *d, int len, float baro_kpa, sensor_id_t *id, float *value);

/* Decodifica un grupo del broadcast de MegaSquirt (8 bytes, big-endian).
 * Llama a out(id, valor) por cada valor que sirve. Publica para probarla en la compu. */
void ecu_decode_megasquirt(int group, const uint8_t *d, void (*out)(sensor_id_t id, float value));
/* Igual, para el "dash broadcasting" simplificado (msg = 0..4 desde la base) */
void ecu_decode_megasquirt_dash(int msg, const uint8_t *d, void (*out)(sensor_id_t id, float value));
