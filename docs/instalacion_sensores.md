# Instalacion de sensores

La configuracion de sensores se hace desde la pantalla:
**mantener apretado > Configuracion > Sensores** (pide la clave de instalador, de fabrica `0000`).
Se guarda en la memoria interna del ESP32-P4, no en la SD. Sin configurar, todos los valores salen simulados.

Cada sensor tiene una **fuente**: Apagado, Simulado, Analogico, Pulsos (solo RPM y velocidad) o ECU (CAN / OBD-II, en desarrollo).
Se pueden mezclar: por ejemplo RPM de la ECU y presion de aceite de un sensor agregado.

## Entradas del ESP32-P4

- **Analogicas (ADC):** GPIO20 a 23 y GPIO49 a 52. GPIO16 a 19 tambien tienen ADC, pero en esta placa pueden estar conectados al chip de WiFi.
- **Pulsos:** cualquier GPIO libre.
- La placa ya usa: 7-13, 24-27, 32, 33, 37-44 y 53 (el menu no los ofrece).

## Cableado

- Los pines aguantan **como maximo 3,3 V**.
- Sensores de 5 V: divisor de 10k arriba y 20k abajo, y en el menu **Divisor = 1.5**.
- Bateria: divisor de 100k arriba y 10k abajo (**Divisor = 11**, el de fabrica).
- Temperaturas (NTC): un lado a masa, el otro al pin, con una resistencia de pull-up de 2,2k a 3,3 V (Divisor = 1).
- RPM desde la bobina: **siempre con optoacoplador o un modulo de tacometro**. El negativo de bobina tiene picos de cientos de voltios.
  Motor de 4 cilindros y 4 tiempos con una chispa por cilindro: 2 pulsos por vuelta.
- Velocidad: sensor VSS (hall a masa), en el menu los pulsos por km.

## Tipos de sensor ya calibrados

| Tipo | Voltaje | Valor |
|---|---|---|
| Presion 0-100 psi / 0-150 psi / 0-10 bar | 0,5-4,5 V | aceite, nafta |
| MAP 2 / 3 / 4 bar | 0,5-4,5 V | turbo (0 = presion atmosferica) |
| Banda ancha 0-5 V = 10-20 AFR | 0-5 V | |
| Banda ancha AEM X-Series | 0,5-4,5 V = 8,5-18 AFR | |
| Banda ancha Innovate LC-2 | 0-5 V = 7,35-22,39 AFR | |
| EGT amplificador 5 mV/C | 0-5 V = 0-1000 C | |
| Voltaje directo | | bateria |
| Temperatura NTC tipo Bosch / tipo GM | | agua, aceite, admision |
| Personalizado (2 puntos) | dos voltajes con su valor | cualquier otro |
| NTC personalizada | R a 25 C y beta | |

Las presiones se convierten solas a la unidad de la pantalla (turbo en psi, aceite y nafta en bar).
