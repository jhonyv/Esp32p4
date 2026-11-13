#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Expansor analogico ADS1115 (16 bits, 4 entradas) en la ficha I2C de la placa.
 * Es el mismo bus del tactil (SDA GPIO7, SCL GPIO8): el driver I2C se encarga de que
 * no se pisen. Hasta dos modulos:
 *   #1 en 0x48 (pin ADDR a GND)   #2 en 0x49 (pin ADDR a VDD)
 * El modulo se alimenta con 3,3 V. A 5 V sus pull-up suben el I2C a 5 V y eso
 * puede romper el ESP32-P4.
 *
 * Lectura sin bloquear: cada llamada a ads_poll() lee la conversion anterior de cada
 * modulo y arranca la del canal siguiente. Llamandola cada 10 ms, un modulo con los 4
 * canales en uso actualiza cada canal 25 veces por segundo; los canales "rapidos"
 * (turbo, acelerador) se leen en uno de cada dos turnos. */

#define ADS_CHIPS        2
#define ADS_CHANNELS     4
#define ADS_FULL_SCALE   4.096f   /* rango elegido: +-4,096 V (con 3,3 V alcanza) */

/* Que canales se usan en cada modulo (bit 0 = A0) y cuales leer mas seguido.
 * Llamar antes de ads_start(). */
void ads_use_channel(int chip, int ch, bool fast);

/* Busca los modulos que tengan canales en uso. Devuelve cuantos encontro. */
int ads_start(void);

/* true si el modulo contesto (y no se perdio despues) */
bool ads_present(int chip);

/* Una vuelta de lectura. Solo desde la tarea de sensores. */
void ads_poll(void);

/* Ultimo voltaje leido en ese canal (en el pin del ADS). false si no hay lectura. */
bool ads_get_volts(int chip, int ch, float *out);
