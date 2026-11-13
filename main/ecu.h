#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sensors.h"

/* Lectura de la ECU original por OBD-II sobre CAN (ISO 15765-4).
 * Hace falta un transceptor CAN de 3,3 V (SN65HVD230 o similar) entre los pines
 * TX/RX del ESP32-P4 y los pines 6 (CAN-H) y 14 (CAN-L) del conector OBD.
 *
 * Detecta sola la velocidad del bus (500 o 250 kbit/s) y si usa IDs de 11 o 29 bits,
 * pregunta que datos informa la ECU y despues los pide en ronda, con las RPM
 * en uno de cada dos pedidos para que la aguja se mueva fluida.
 * Los valores van a los sensores que tienen fuente "ECU" (sensors_set_ecu). */

typedef enum {
    ECU_MODE_OFF = 0,
    ECU_MODE_OBD_CAN,
} ecu_mode_t;

typedef struct {
    uint8_t mode;       /* ecu_mode_t */
    int8_t tx_gpio;     /* -1 = sin elegir */
    int8_t rx_gpio;
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
    uint8_t pid_count;      /* cuantos PIDs dice que soporta */
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
