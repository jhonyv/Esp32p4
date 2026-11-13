#include "ads1115.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "ads1115";

#define ADS_ADDR0        0x48
#define REG_CONVERSION   0x00
#define REG_CONFIG       0x01
#define I2C_SPEED_HZ     400000
#define I2C_TIMEOUT_MS   20
#define MAX_ERRORS       10       /* errores seguidos para darlo por perdido */
#define RETRY_POLLS      200      /* perdido: vuelve a buscarlo cada ~2 s */
#define SEQ_MAX          8

typedef struct {
    uint8_t used;                 /* canales en uso (bit 0 = A0) */
    uint8_t fast;                 /* canales que se leen mas seguido */
    i2c_master_dev_handle_t dev;
    bool present;
    uint8_t seq[SEQ_MAX];         /* orden de lectura de los canales */
    uint8_t seq_len, seq_pos;
    int8_t converting;            /* canal que esta convirtiendo, -1 ninguno */
    uint8_t errors;
    uint16_t retry;
    bool have[ADS_CHANNELS];
    float volts[ADS_CHANNELS];
} ads_chip_t;

static ads_chip_t s_chip[ADS_CHIPS];
static i2c_master_bus_handle_t s_bus;

void ads_use_channel(int chip, int ch, bool fast)
{
    if (chip < 0 || chip >= ADS_CHIPS || ch < 0 || ch >= ADS_CHANNELS) {
        return;
    }
    s_chip[chip].used |= 1 << ch;
    if (fast) {
        s_chip[chip].fast |= 1 << ch;
    }
}

/* Los rapidos van entre cada uno de los lentos: rapido, lento1, rapido, lento2... */
static void build_sequence(ads_chip_t *c)
{
    uint8_t fast = c->fast & c->used, slow = c->used & ~fast;
    c->seq_len = 0;
    for (int s = 0; s < ADS_CHANNELS; s++) {
        if (!(slow & (1 << s))) {
            continue;
        }
        for (int f = 0; f < ADS_CHANNELS; f++) {
            if (fast & (1 << f)) {
                c->seq[c->seq_len++] = f;
            }
        }
        c->seq[c->seq_len++] = s;
    }
    if (!slow) {
        for (int f = 0; f < ADS_CHANNELS; f++) {
            if (fast & (1 << f)) {
                c->seq[c->seq_len++] = f;
            }
        }
    }
    c->seq_pos = 0;
}

static bool chip_connect(int i)
{
    ads_chip_t *c = &s_chip[i];
    uint8_t addr = ADS_ADDR0 + i;
    if (i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (!c->dev) {
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = I2C_SPEED_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &dcfg, &c->dev) != ESP_OK) {
            c->dev = NULL;
            return false;
        }
    }
    c->present = true;
    c->errors = 0;
    c->converting = -1;
    return true;
}

int ads_start(void)
{
    int found = 0;
    bool wanted = false;
    for (int i = 0; i < ADS_CHIPS; i++) {
        s_chip[i].converting = -1;
        wanted |= s_chip[i].used != 0;
    }
    if (!wanted) {
        return 0;
    }
    if (bsp_i2c_init() != ESP_OK || !(s_bus = bsp_i2c_get_handle())) {
        ESP_LOGE(TAG, "No se pudo usar el bus I2C");
        return 0;
    }
    for (int i = 0; i < ADS_CHIPS; i++) {
        ads_chip_t *c = &s_chip[i];
        if (!c->used) {
            continue;
        }
        build_sequence(c);
        if (chip_connect(i)) {
            ESP_LOGI(TAG, "ADS1115 #%d encontrado en 0x%02X", i + 1, ADS_ADDR0 + i);
            found++;
        } else {
            ESP_LOGE(TAG, "ADS1115 #%d no contesta en 0x%02X (cableado? pin ADDR? alimentado a 3,3 V?)",
                     i + 1, ADS_ADDR0 + i);
        }
    }
    return found;
}

bool ads_present(int chip)
{
    return chip >= 0 && chip < ADS_CHIPS && s_chip[chip].present;
}

static void chip_error(int i)
{
    ads_chip_t *c = &s_chip[i];
    c->converting = -1;
    if (++c->errors >= MAX_ERRORS) {
        ESP_LOGE(TAG, "Se perdio el ADS1115 #%d", i + 1);
        c->present = false;
        c->retry = 0;
        memset(c->have, 0, sizeof(c->have));
    }
}

static void chip_poll(int i)
{
    ads_chip_t *c = &s_chip[i];
    if (!c->present) {
        if (++c->retry >= RETRY_POLLS) {
            c->retry = 0;
            if (chip_connect(i)) {
                ESP_LOGI(TAG, "ADS1115 #%d volvio", i + 1);
            }
        }
        return;
    }
    /* 1) Resultado de la conversion que se arranco en la vuelta anterior
     *    (a 250 muestras/s tarda 4 ms, y las vueltas son cada 10 ms) */
    if (c->converting >= 0) {
        uint8_t reg = REG_CONVERSION, rx[2];
        if (i2c_master_transmit_receive(c->dev, &reg, 1, rx, 2, I2C_TIMEOUT_MS) != ESP_OK) {
            chip_error(i);
            return;
        }
        int16_t raw = (int16_t)((rx[0] << 8) | rx[1]);
        float v = raw * (ADS_FULL_SCALE / 32768.0f);
        c->volts[c->converting] = v < 0 ? 0 : v;
        c->have[c->converting] = true;
    }
    /* 2) Arranca el canal que sigue */
    uint8_t ch = c->seq[c->seq_pos];
    c->seq_pos = (c->seq_pos + 1) % c->seq_len;
    uint16_t cfg = 0x8000                 /* OS: arrancar una conversion */
                 | ((4 + ch) << 12)       /* MUX: entrada AINx contra masa */
                 | (1 << 9)               /* PGA: +-4,096 V */
                 | (1 << 8)               /* una conversion y se duerme */
                 | (5 << 5)               /* 250 muestras/s: filtra mejor el ruido del encendido */
                 | 3;                     /* comparador apagado */
    uint8_t tx[3] = { REG_CONFIG, cfg >> 8, cfg & 0xFF };
    if (i2c_master_transmit(c->dev, tx, sizeof(tx), I2C_TIMEOUT_MS) != ESP_OK) {
        chip_error(i);
        return;
    }
    c->converting = ch;
    c->errors = 0;
}

void ads_poll(void)
{
    if (!s_bus) {
        return;
    }
    for (int i = 0; i < ADS_CHIPS; i++) {
        if (s_chip[i].used && s_chip[i].seq_len) {
            chip_poll(i);
        }
    }
}

bool ads_get_volts(int chip, int ch, float *out)
{
    if (chip < 0 || chip >= ADS_CHIPS || ch < 0 || ch >= ADS_CHANNELS || !s_chip[chip].have[ch]) {
        return false;
    }
    *out = s_chip[chip].volts[ch];
    return true;
}
