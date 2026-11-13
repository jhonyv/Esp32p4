# Instalación de sensores

La configuración se hace desde la pantalla: **mantener apretado > Configuración > Sensores**. Pide la clave de instalador, que de fábrica es `0000`.
Se guarda en la memoria interna del ESP32-P4, no en la SD. Si no se configura nada, todos los valores salen simulados.

Para cada valor se elige **de dónde se lee**:

- **Un sensor en la placa**: el sensor va a su borne de la placa de sensores, que ya tiene el circuito armado. En el menú solo se elige qué sensor es (por ejemplo "MAP 3 bar").
- **La ECU** (CAN / OBD-II / MegaSquirt): se configura en Configuración > ECU.
- **Simulado**: valores de demostración.
- **Apagado**.

Se pueden mezclar. Por ejemplo, las RPM pueden venir de la ECU y la presión de aceite de un sensor agregado.

El botón **Avanzado (instaladores)** muestra los datos técnicos: el pin, el divisor, la pull-up y el filtro. Sirve para un cableado distinto al de la placa.

## La placa de sensores

Cada valor tiene su borne fijo:

| Borne | Valor | Pin | Circuito |
|---|---|---|---|
| RPM | RPM | IO2 | optoacoplador |
| VELOCIDAD | velocidad | IO3 | optoacoplador |
| BATERIA | batería | IO49 | divisor 100k / 10k |
| AGUA | temperatura de agua | IO51 | NTC |
| ACEITE T | temperatura de aceite | IO52 | NTC |
| TURBO | turbo (MAP) | ADS1115 #1 A0 | 0-5 V |
| ACEITE P | presión de aceite | ADS1115 #1 A1 | 0-5 V |
| NAFTA P | presión de nafta | ADS1115 #1 A2 | 0-5 V |
| AFR | banda ancha | ADS1115 #1 A3 | 0-5 V |
| TPS | acelerador | ADS1115 #2 A0 | 0-5 V |
| EGT | temperatura de escape (con amplificador) | ADS1115 #2 A1 | 0-5 V |
| ADMISION | temperatura de admisión | ADS1115 #2 A2 | NTC |
| EXTRA | libre (se elige en Avanzado) | ADS1115 #2 A3 | 0-5 V |
| CAN TX / CAN RX | ECU | IO46 / IO47 | transceptor SN65HVD230 |

La placa se puede armar por partes:

- **Básica, sin ADS1115**: RPM, velocidad, batería, temperatura de agua y temperatura de aceite. Es lo típico de un carburado.
- **Con el ADS1115 #1**: suma turbo, presión de aceite, presión de nafta y AFR.
- **Con el ADS1115 #2**: suma acelerador, EGT, temperatura de admisión y una entrada libre.

Si un sensor usa un ADS1115 que no está conectado, el menú lo avisa ("no se encuentra el ADS1115 #1").

## Conexión a las fichas de la pantalla

La placa Waveshare ESP32-P4 7B tiene estos pines libres:

- Ficha de 12 pines: 3V3, GND, IO2, IO3, IO4, IO5, IO28, IO29, IO30, IO31, IO34 e IO36.
- Ficha de 10 pines: BAT, GND, 3V3, GND, IO46, IO47, IO48, IO49, IO51 e IO52.
- Ficha I2C: SCL (GPIO8), SDA (GPIO7), VCC y GND. Es el mismo bus del táctil.

Solo **IO49, IO51 e IO52** tienen entrada analógica. Por eso las demás analógicas van a los ADS1115.

Quedan sin usar IO4 e IO5 (para una salida de shift light o botones) y IO28 a 31 e IO48. Mejor no usar IO34 ni IO36 para entradas: son pines de arranque del ESP32-P4. La ficha "C6 UART" es la conexión con el chip de WiFi y no se toca.

### Los ADS1115

- **Alimentarlos con 3,3 V**, del pin 3V3 de las fichas. Si van a 5 V, sus resistencias de pull-up suben el I2C a 5 V y eso puede romper el ESP32-P4. Antes de usar el VCC de la ficha I2C, medir que sea 3,3 V.
- SDA y SCL de los dos módulos van a SDA y SCL de la ficha I2C.
- **ADS1115 #1**: el pin ADDR a GND (dirección 0x48).
- **ADS1115 #2**: el pin ADDR a VDD (dirección 0x49).
- No chocan con nada de la placa: el táctil usa 0x5D/0x14 y el audio 0x18 y 0x40.

## Circuito de cada entrada

Ningún pin del ESP32-P4 ni del ADS1115 aguanta más de 3,3 V. Cada borne tiene su protección.

### Entradas de 0-5 V

Son TURBO, ACEITE P, NAFTA P, AFR, TPS, EGT y EXTRA.

```
señal del sensor ──[10k]──┬───────────► ADS1115 Ax
                          ├──[20k]──── GND
                          ├──[100nF]── GND
                          └── BAT54S: un diodo a 3V3 y el otro a GND
```

El divisor pasa 5 V a 3,33 V; en el menú es "divisor 1,5" y ya viene puesto.
Los sensores de 3 cables llevan +5 V regulados (estables, no los 12 V del auto), masa y señal.

### Temperaturas NTC

Son AGUA, ACEITE T y ADMISION.

```
3V3 ──[1k 1%]──┬── al sensor (el otro lado del sensor a masa)
               └──[4,7k]──┬──────► IO51 / IO52 / ADS1115 #2 A2
                          ├──[100nF]── GND
                          └── BAT54S a 3V3 y GND
```

Con 1k la lectura es más fina entre 60 y 130 °C, que es donde importa.
Si el sensor es de un solo cable (hace masa por el block), la masa de la placa tiene que ir al block. Si no, la temperatura sale corrida.

### Batería

```
+12 V de contacto ──[100k]──┬──────► IO49
                            ├──[10k]──── GND
                            ├──[100nF]── GND
                            └── BAT54S a 3V3 y GND
```

### RPM y velocidad: optoacoplador PC817

```
señal ──[2,2k ½W]──┬── PC817 pata 1 (ánodo)     PC817 pata 4 (colector) ──► IO2 / IO3
                   │   PC817 pata 2 (cátodo) ── masa del auto
                   └── 1N4148 al revés, en paralelo con el LED (pata 1 y 2)
                                                PC817 pata 3 (emisor) ── GND de la placa
```

- Sirve para la salida de tacómetro de una ECU, un CDI o un MSD (onda cuadrada de 5 a 12 V) y para sensores de velocidad hall.
- El pin del ESP tiene pull-up interna, así que el PC817 tira a masa en cada pulso. Por eso el flanco es "bajada".
- **Negativo de bobina** (autos con platino o encendido sin salida de tacómetro): nunca directo, tiene picos de cientos de voltios.
  - Agregar en serie: 1N4007, 22k de 1 W y un zener de 18 V (el cátodo hacia la bobina) antes del PC817.
  - Así solo el pico de la chispa prende el LED, no los 12 V.
  - Este circuito todavía está por probar en un auto.
- Sensores VR de 2 cables (inductivos): necesitan un acondicionador (MAX9926 o LM1815). No van directo al PC817.

En el menú, para RPM se elige el motor ("4 cilindros / rotativo 2 rotores", "6 cilindros"...). Para velocidad se cargan los pulsos por km.

### CAN (ECU)

Módulo SN65HVD230 (de 3,3 V):

- 3V3 y GND.
- CTX a IO46 y CRX a IO47.
- CANH y CANL a la ECU, o al OBD: pin 6 CAN-H, pin 14 CAN-L.

El módulo trae una resistencia de 120 Ω. Con una MegaSquirt se deja. En el OBD de un auto de fábrica el bus ya tiene las suyas: si falla la lectura, sacarla.

## Tipos de sensor ya calibrados

El menú solo muestra los que sirven para cada valor.

| Tipo | Voltaje | Para |
|---|---|---|
| Presión 0-100 psi / 0-150 psi / 0-10 bar | 0,5-4,5 V | aceite, nafta, turbo |
| MAP 2 / 3 / 4 bar | 0,5-4,5 V | turbo (0 = presión atmosférica) |
| Banda ancha 0-5 V = 10-20 AFR | 0-5 V | AFR |
| Banda ancha AEM X-Series | 0,5-4,5 V = 8,5-18 AFR | AFR |
| Banda ancha Innovate LC-2 | 0-5 V = 7,35-22,39 AFR | AFR |
| EGT amplificador 5 mV/°C | 0-5 V = 0-1000 °C | EGT |
| Voltaje directo | | batería |
| Temperatura NTC tipo Bosch / tipo GM | | agua, aceite, admisión |
| Potenciómetro | se calibra con el pedal (botones "Tomar") | acelerador |
| Otro sensor (dos puntos) | dos voltajes con su valor | cualquiera |
| Otra NTC | R a 25 °C y beta | temperaturas |

Las presiones se pasan solas a la unidad de la pantalla: turbo en psi, aceite y nafta en bar.

## Lista de materiales (placa completa)

| Cantidad | Componente |
|---|---|
| 2 | Módulo ADS1115 (16 bits, 4 entradas) |
| 1 | Módulo SN65HVD230 (solo si se lee la ECU) |
| 2 | Optoacoplador PC817 |
| 8 | Resistencia 10k (7 entradas de 0-5 V y la batería) |
| 7 | Resistencia 20k |
| 3 | Resistencia 1k 1 % (pull-up de las NTC) |
| 3 | Resistencia 4,7k |
| 1 | Resistencia 100k |
| 2 | Resistencia 2,2k ½ W |
| 11 | BAT54S (doble diodo Schottky) |
| 11 | Capacitor 100 nF |
| 2 | 1N4148 |
| 1 | 1N4007 + resistencia 22k 1 W + zener 18 V (solo para negativo de bobina) |
| - | Borneras con el nombre de cada entrada |

Antes de hacer placas conviene armar una en protoboard y probar. El menú muestra en vivo los voltios de cada entrada, así se ve enseguida si un divisor o una pull-up quedaron mal.
