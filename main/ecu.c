#include "ecu.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

static const char *TAG = "ecu";

#define NVS_NS              "ecu"
#define NVS_KEY             "cfg"
#define REQ_TIMEOUT_MS      100     /* espera de cada respuesta */
#define LOST_AFTER          25      /* sin respuesta tantas veces seguidas = se perdio */
#define RETRY_MS            3000    /* entre intentos de conexion (contacto apagado, etc.) */
#define BARO_EVERY_US       (30 * 1000000LL)

/* Que PID se pide para cada valor. Si el primero no esta, se usa el alternativo. */
static const struct {
    sensor_id_t id;
    uint8_t pid;
    uint8_t alt;
} s_map[] = {
    { SENSOR_SPEED,      0x0D, 0 },
    { SENSOR_WATER_TEMP, 0x05, 0 },
    { SENSOR_BOOST,      0x0B, 0 },     /* MAP absoluto -> turbo */
    { SENSOR_TPS,        0x11, 0 },
    { SENSOR_IAT,        0x0F, 0 },
    { SENSOR_BATTERY,    0x42, 0 },
    { SENSOR_OIL_TEMP,   0x5C, 0 },
    { SENSOR_AFR,        0x24, 0x34 },  /* lambda de la sonda 1 (banda ancha) */
    { SENSOR_FUEL_PRESS, 0x23, 0x0A },  /* presion de riel / de nafta */
};
#define MAP_COUNT ((int)(sizeof(s_map) / sizeof(s_map[0])))
#define PID_RPM   0x0C
#define PID_BARO  0x33

typedef struct {
    uint32_t id;
    bool ext;
    uint8_t len;
    uint8_t data[8];
} rx_msg_t;

static ecu_config_t s_cfg;
static twai_node_handle_t s_node;
static QueueHandle_t s_rxq;
static volatile bool s_bus_off;
static volatile ecu_status_t s_status;
static bool s_supported[256];
static float s_last[SENSOR_COUNT];
static bool s_have[SENSOR_COUNT];
static float s_baro = 101.3f;

/* ------------------------------------------------------------------------- */
/* Configuracion                                                              */
/* ------------------------------------------------------------------------- */

void ecu_get_config(ecu_config_t *out)
{
    ecu_config_t c = { .mode = ECU_MODE_OFF, .tx_gpio = -1, .rx_gpio = -1 };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t size = sizeof(c);
        ecu_config_t saved;
        if (nvs_get_blob(h, NVS_KEY, &saved, &size) == ESP_OK && size == sizeof(saved)) {
            c = saved;
        }
        nvs_close(h);
    }
    *out = c;
}

esp_err_t ecu_save_config(const ecu_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* Decodificacion (SAE J1979, modo 01)                                        */
/* ------------------------------------------------------------------------- */

bool ecu_decode_pid(uint8_t pid, const uint8_t *d, int len, float baro_kpa, sensor_id_t *id, float *value)
{
    if (len < 1) {
        return false;
    }
    float a = d[0], ab = len >= 2 ? d[0] * 256.0f + d[1] : 0;
    switch (pid) {
    case 0x0C: if (len < 2) return false; *id = SENSOR_RPM;        *value = ab / 4.0f; return true;
    case 0x0D:                             *id = SENSOR_SPEED;      *value = a; return true;
    case 0x05:                             *id = SENSOR_WATER_TEMP; *value = a - 40; return true;
    case 0x0F:                             *id = SENSOR_IAT;        *value = a - 40; return true;
    case 0x5C:                             *id = SENSOR_OIL_TEMP;   *value = a - 40; return true;
    case 0x11:                             *id = SENSOR_TPS;        *value = a * 100.0f / 255.0f; return true;
    case 0x0B:                             /* MAP absoluto en kPa -> turbo en psi */
        *id = SENSOR_BOOST;
        *value = (a - baro_kpa) * 0.145038f;
        return true;
    case 0x42: if (len < 2) return false; *id = SENSOR_BATTERY;    *value = ab / 1000.0f; return true;
    case 0x24: case 0x34:                  /* lambda = 2/65536 * (256A+B) */
        if (len < 2) return false;
        *id = SENSOR_AFR;
        *value = ab * 2.0f / 65536.0f * 14.7f;
        return true;
    case 0x23: if (len < 2) return false; /* presion de riel, 10 kPa por unidad -> bar */
        *id = SENSOR_FUEL_PRESS;
        *value = ab * 10.0f / 100.0f;
        return true;
    case 0x0A:                             /* presion de nafta, 3 kPa por unidad -> bar */
        *id = SENSOR_FUEL_PRESS;
        *value = a * 3.0f / 100.0f;
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* CAN                                                                        */
/* ------------------------------------------------------------------------- */

static bool rx_done_cb(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)edata;
    (void)user_ctx;
    uint8_t buf[8] = { 0 };
    twai_frame_t f = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(handle, &f) != ESP_OK) {
        return false;
    }
    rx_msg_t m = { .id = f.header.id, .ext = f.header.ide, .len = f.header.dlc > 8 ? 8 : f.header.dlc };
    memcpy(m.data, buf, sizeof(m.data));
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rxq, &m, &woken);
    return woken == pdTRUE;
}

static bool state_cb(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_bus_off = true;
    }
    return false;
}

static void node_close(void)
{
    if (s_node) {
        twai_node_disable(s_node);
        twai_node_delete(s_node);
        s_node = NULL;
    }
    xQueueReset(s_rxq);
}

static bool node_open(uint32_t bitrate, bool listen_only)
{
    node_close();
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = s_cfg.tx_gpio,
            .rx = s_cfg.rx_gpio,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = { .bitrate = bitrate },
        .fail_retry_cnt = 3,
        .tx_queue_depth = 4,
        .flags = { .enable_listen_only = listen_only },
    };
    if (twai_new_node_onchip(&cfg, &s_node) != ESP_OK) {
        s_node = NULL;
        return false;
    }
    twai_event_callbacks_t cbs = { .on_rx_done = rx_done_cb, .on_state_change = state_cb };
    s_bus_off = false;
    if (twai_node_register_event_callbacks(s_node, &cbs, NULL) != ESP_OK || twai_node_enable(s_node) != ESP_OK) {
        twai_node_delete(s_node);
        s_node = NULL;
        return false;
    }
    return true;
}

static bool send_request(uint8_t pid)
{
    static uint8_t data[8];
    static twai_frame_t f;
    memset(data, 0, sizeof(data));
    data[0] = 0x02;     /* largo */
    data[1] = 0x01;     /* modo 01: datos actuales */
    data[2] = pid;
    memset(&f, 0, sizeof(f));
    f.header.id = s_status.ext_id ? 0x18DB33F1 : 0x7DF;   /* pedido a todas las ECU */
    f.header.ide = s_status.ext_id;
    f.header.dlc = 8;
    f.buffer = data;
    f.buffer_len = 8;
    if (twai_node_transmit(s_node, &f, 20) != ESP_OK) {
        return false;
    }
    return twai_node_transmit_wait_all_done(s_node, 30) == ESP_OK;
}

static bool is_obd_reply(const rx_msg_t *m, bool ext)
{
    if (ext) {
        return m->ext && (m->id & 0x1FFFFF00u) == 0x18DAF100u;
    }
    return !m->ext && m->id >= 0x7E8 && m->id <= 0x7EF;
}

/* Espera la respuesta a un PID. Devuelve el largo de los datos (A, B...) o -1. */
static int wait_reply(uint8_t pid, uint8_t *out, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    rx_msg_t m;
    while (1) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return -1;
        }
        TickType_t ticks = pdMS_TO_TICKS(left / 1000);
        if (xQueueReceive(s_rxq, &m, ticks ? ticks : 1) != pdTRUE) {
            continue;
        }
        if (!is_obd_reply(&m, s_status.ext_id)) {
            continue;
        }
        int len = m.data[0] & 0x0F;
        if ((m.data[0] >> 4) != 0 || len < 2 || len > 7 || m.data[1] != 0x41 || m.data[2] != pid) {
            continue;   /* no es una respuesta corta a este PID */
        }
        memcpy(out, &m.data[3], len - 2);
        return len - 2;
    }
}

/* Pide un mapa de PIDs soportados (0x00, 0x20, 0x40...) y junta lo que contesten
 * todas las ECU (motor, caja...). */
static bool query_bitmap(uint8_t base)
{
    if (!send_request(base)) {
        return false;
    }
    uint8_t d[5];
    bool any = false;
    int timeout = REQ_TIMEOUT_MS;
    int n;
    while ((n = wait_reply(base, d, timeout)) >= 0) {
        if (n < 4) {
            continue;
        }
        any = true;
        for (int i = 0; i < 32; i++) {
            if (d[i / 8] & (0x80 >> (i % 8))) {
                s_supported[base + 1 + i] = true;
            }
        }
        timeout = 30;   /* ya contesto una: un ratito mas por si contesta otra */
    }
    return any;
}

/* Hay trafico en el bus a esta velocidad? (escuchando sin transmitir) */
static bool bus_has_traffic(uint32_t bitrate)
{
    if (!node_open(bitrate, true)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    int frames = uxQueueMessagesWaiting(s_rxq);
    node_close();
    return frames >= 2;
}

/* Busca la velocidad del bus y el tipo de ID. true = la ECU contesto. */
static bool detect(void)
{
    static const uint32_t rates[] = { 500000, 250000 };
    uint32_t candidates[2];
    int count = 0;
    for (int i = 0; i < 2; i++) {
        if (bus_has_traffic(rates[i])) {
            candidates[count++] = rates[i];
            break;
        }
    }
    if (count == 0) {
        /* Bus callado (muchos autos no hablan por el conector hasta que se les pregunta) */
        candidates[0] = rates[0];
        candidates[1] = rates[1];
        count = 2;
    }
    for (int i = 0; i < count; i++) {
        if (!node_open(candidates[i], false)) {
            s_status.state = ECU_STATE_ERROR;
            return false;
        }
        for (int ext = 0; ext < 2; ext++) {
            s_status.ext_id = ext;
            memset(s_supported, 0, sizeof(s_supported));
            if (query_bitmap(0x00)) {
                s_status.bitrate = candidates[i];
                ESP_LOGI(TAG, "ECU encontrada: %lu kbit/s, IDs de %d bits",
                         (unsigned long)(candidates[i] / 1000), ext ? 29 : 11);
                return true;
            }
        }
        node_close();
    }
    return false;
}

static void read_supported(void)
{
    for (int base = 0x20; base <= 0xA0; base += 0x20) {
        if (!s_supported[base] || !query_bitmap(base)) {
            break;
        }
    }
    int n = 0;
    for (int i = 1; i < 256; i++) {
        if (s_supported[i] && i % 0x20 != 0) {
            n++;
        }
    }
    s_status.pid_count = n;
}

static uint8_t pid_for(int map_idx)
{
    if (s_supported[s_map[map_idx].pid]) {
        return s_map[map_idx].pid;
    }
    if (s_map[map_idx].alt && s_supported[s_map[map_idx].alt]) {
        return s_map[map_idx].alt;
    }
    return 0;
}

bool ecu_supports(sensor_id_t id)
{
    if (s_status.state != ECU_STATE_CONNECTED) {
        return false;
    }
    if (id == SENSOR_RPM) {
        return s_supported[PID_RPM];
    }
    for (int i = 0; i < MAP_COUNT; i++) {
        if (s_map[i].id == id) {
            return pid_for(i) != 0;
        }
    }
    return false;
}

static bool request(uint8_t pid)
{
    uint8_t d[5];
    if (!send_request(pid)) {
        return false;
    }
    int n = wait_reply(pid, d, REQ_TIMEOUT_MS);
    if (n < 0) {
        return false;
    }
    if (pid == PID_BARO) {
        s_baro = d[0];
        return true;
    }
    sensor_id_t id;
    float v;
    if (ecu_decode_pid(pid, d, n, s_baro, &id, &v)) {
        s_last[id] = v;
        s_have[id] = true;
        sensors_set_ecu(id, v);
    }
    return true;
}

/* Pide los datos en ronda hasta que la ECU deje de contestar */
static void poll(void)
{
    uint8_t others[MAP_COUNT];
    int n = 0;
    for (int i = 0; i < MAP_COUNT; i++) {
        uint8_t pid = pid_for(i);
        if (pid) {
            others[n++] = pid;
        }
    }
    bool rpm = s_supported[PID_RPM];
    int next = 0, misses = 0, answers = 0;
    int64_t window = esp_timer_get_time(), last_baro = 0;
    bool slot_rpm = true;
    while (misses < LOST_AFTER && !s_bus_off) {
        uint8_t pid;
        int64_t now = esp_timer_get_time();
        if (s_supported[PID_BARO] && (last_baro == 0 || now - last_baro > BARO_EVERY_US)) {
            pid = PID_BARO;
            last_baro = now;
        } else if ((rpm && slot_rpm) || n == 0) {
            pid = PID_RPM;          /* las RPM en uno de cada dos pedidos */
        } else {
            pid = others[next];
            next = (next + 1) % n;
        }
        slot_rpm = !slot_rpm;
        if (request(pid)) {
            misses = 0;
            answers++;
        } else {
            misses++;
        }
        if (now - window >= 1000000) {
            s_status.responses_per_s = answers;
            answers = 0;
            window = now;
        }
    }
    ESP_LOGW(TAG, "La ECU dejo de contestar");
}

static void ecu_task(void *arg)
{
    (void)arg;
    while (1) {
        s_status.state = ECU_STATE_SEARCHING;
        s_status.responses_per_s = 0;
        s_status.pid_count = 0;
        if (detect()) {
            read_supported();
            s_status.state = ECU_STATE_CONNECTED;
            ESP_LOGI(TAG, "La ECU informa %d datos", s_status.pid_count);
            poll();
        }
        if (s_status.state == ECU_STATE_ERROR) {
            ESP_LOGE(TAG, "No se pudo iniciar el CAN en GPIO%d/GPIO%d", s_cfg.tx_gpio, s_cfg.rx_gpio);
        }
        node_close();
        s_status.state = s_status.state == ECU_STATE_ERROR ? ECU_STATE_ERROR : ECU_STATE_SEARCHING;
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
}

void ecu_start(void)
{
    ecu_get_config(&s_cfg);
    s_status.state = ECU_STATE_OFF;
    if (s_cfg.mode != ECU_MODE_OBD_CAN) {
        return;
    }
    if (s_cfg.tx_gpio < 0 || s_cfg.rx_gpio < 0 || s_cfg.tx_gpio == s_cfg.rx_gpio ||
        !sensors_gpio_ok(s_cfg.tx_gpio, false) || !sensors_gpio_ok(s_cfg.rx_gpio, false)) {
        ESP_LOGE(TAG, "Faltan los pines TX/RX del CAN (o no sirven)");
        return;
    }
    s_rxq = xQueueCreate(32, sizeof(rx_msg_t));
    s_status.state = ECU_STATE_SEARCHING;
    xTaskCreate(ecu_task, "ecu", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "OBD-II por CAN en TX GPIO%d / RX GPIO%d", s_cfg.tx_gpio, s_cfg.rx_gpio);
}

void ecu_get_status(ecu_status_t *out)
{
    out->state = s_status.state;
    out->bitrate = s_status.bitrate;
    out->ext_id = s_status.ext_id;
    out->responses_per_s = s_status.responses_per_s;
    out->pid_count = s_status.pid_count;
}

bool ecu_get_value(sensor_id_t id, float *out)
{
    if (id >= SENSOR_COUNT || !s_have[id]) {
        return false;
    }
    *out = s_last[id];
    return true;
}
