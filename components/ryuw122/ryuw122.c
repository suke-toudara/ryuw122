/*
 * RYUW122 UWB transceiver driver for ESP-IDF.
 */
#include "ryuw122.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ryuw122";

#define RYUW122_UART_RX_BUF 1024
#define RYUW122_RX_CHUNK 64
#define RYUW122_RX_POLL_MS 50
#define RYUW122_RESET_LOW_MS 10
#define RYUW122_RESET_BOOT_MS 300
/* The module needs a short break after a setting is stored in flash. */
#define RYUW122_SETTLE_MS 100

/** One line of text handed from the RX task to the command layer. */
typedef struct {
    char text[RYUW122_LINE_MAX];
} ryuw122_line_t;

struct ryuw122_dev_t {
    uart_port_t uart_port;
    int rst_gpio;
    int baud_rate;
    uint32_t cmd_timeout_ms;
    SemaphoreHandle_t lock;      /* recursive: serialises AT transactions */
    QueueHandle_t response_queue;
    QueueHandle_t event_queue;
    TaskHandle_t rx_task;
    SemaphoreHandle_t rx_done;
    volatile bool running;
    ryuw122_event_cb_t event_cb;
    void *event_cb_arg;
};

/* ------------------------------------------------------------------ */
/* RX task                                                            */
/* ------------------------------------------------------------------ */

static void deliver_event(ryuw122_handle_t dev, const ryuw122_event_t *event)
{
    if (dev->event_cb != NULL) {
        dev->event_cb(event, dev->event_cb_arg);
    }
    if (xQueueSend(dev->event_queue, event, 0) != pdTRUE) {
        /* Queue full: drop the oldest report so the newest one survives. */
        ryuw122_event_t discarded;
        if (xQueueReceive(dev->event_queue, &discarded, 0) == pdTRUE) {
            ESP_LOGD(TAG, "event queue full, oldest report dropped");
        }
        xQueueSend(dev->event_queue, event, 0);
    }
}

static void handle_line(ryuw122_handle_t dev, char *line)
{
    if (ryuw122_trim(line) == 0) {
        return;
    }
    ESP_LOGD(TAG, "< %s", line);

    ryuw122_event_t event;
    switch (ryuw122_classify_line(line)) {
    case RYUW122_LINE_ANCHOR_RCV:
        event.type = RYUW122_EVENT_ANCHOR_RCV;
        if (ryuw122_parse_anchor_rcv(line, &event.anchor)) {
            deliver_event(dev, &event);
        } else {
            ESP_LOGW(TAG, "malformed report: %s", line);
        }
        return;
    case RYUW122_LINE_TAG_RCV:
        event.type = RYUW122_EVENT_TAG_RCV;
        if (ryuw122_parse_tag_rcv(line, &event.tag)) {
            deliver_event(dev, &event);
        } else {
            ESP_LOGW(TAG, "malformed report: %s", line);
        }
        return;
    default:
        break;
    }

    ryuw122_line_t item;
    strlcpy(item.text, line, sizeof(item.text));
    if (xQueueSend(dev->response_queue, &item, 0) != pdTRUE) {
        ryuw122_line_t discarded;
        if (xQueueReceive(dev->response_queue, &discarded, 0) == pdTRUE) {
            ESP_LOGD(TAG, "response queue full, dropped: %s", discarded.text);
        }
        xQueueSend(dev->response_queue, &item, 0);
    }
}

static void rx_task(void *arg)
{
    ryuw122_handle_t dev = (ryuw122_handle_t)arg;
    char line[RYUW122_LINE_MAX];
    size_t len = 0;
    uint8_t chunk[RYUW122_RX_CHUNK];

    while (dev->running) {
        int read = uart_read_bytes(dev->uart_port, chunk, sizeof(chunk),
                                   pdMS_TO_TICKS(RYUW122_RX_POLL_MS));
        for (int i = 0; i < read; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    handle_line(dev, line);
                    len = 0;
                }
                continue;
            }
            if (len + 1 < sizeof(line)) {
                line[len++] = c;
            } else {
                /* Overlong line: keep the tail, the head is unusable anyway. */
                ESP_LOGW(TAG, "line too long, discarded");
                len = 0;
            }
        }
    }

    xSemaphoreGive(dev->rx_done);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Command layer                                                      */
/* ------------------------------------------------------------------ */

static inline void lock(ryuw122_handle_t dev)
{
    xSemaphoreTakeRecursive(dev->lock, portMAX_DELAY);
}

static inline void unlock(ryuw122_handle_t dev)
{
    xSemaphoreGiveRecursive(dev->lock);
}

esp_err_t ryuw122_cmd(ryuw122_handle_t dev, const char *cmd, const char *expect,
                      char *resp, size_t resp_size, uint32_t timeout_ms)
{
    if (dev == NULL || cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = dev->cmd_timeout_ms;
    }

    lock(dev);
    xQueueReset(dev->response_queue);

    ESP_LOGD(TAG, "> %s", cmd);
    char buffer[RYUW122_LINE_MAX];
    int written = snprintf(buffer, sizeof(buffer), "%s\r\n", cmd);
    if (written <= 0 || (size_t)written >= sizeof(buffer)) {
        unlock(dev);
        ESP_LOGE(TAG, "command too long: %s", cmd);
        return ESP_ERR_INVALID_ARG;
    }
    if (uart_write_bytes(dev->uart_port, buffer, (size_t)written) != written) {
        unlock(dev);
        return ESP_FAIL;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    const size_t expect_len = (expect != NULL) ? strlen(expect) : 0;
    esp_err_t result = ESP_ERR_TIMEOUT;

    for (;;) {
        /* Signed difference so the comparison survives a tick counter wrap. */
        int32_t remaining = (int32_t)(deadline - xTaskGetTickCount());
        if (remaining <= 0) {
            break;
        }
        ryuw122_line_t item;
        if (xQueueReceive(dev->response_queue, &item, (TickType_t)remaining) != pdTRUE) {
            break;
        }

        if (expect != NULL && strncmp(item.text, expect, expect_len) == 0) {
            if (resp != NULL) {
                strlcpy(resp, item.text, resp_size);
            }
            result = ESP_OK;
            break;
        }

        switch (ryuw122_classify_line(item.text)) {
        case RYUW122_LINE_ERR: {
            int code = ryuw122_parse_err(item.text);
            ESP_LOGE(TAG, "'%s' failed: +ERR=%d (%s)", cmd, code, ryuw122_err_str(code));
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        case RYUW122_LINE_OK:
            if (expect == NULL) {
                if (resp != NULL) {
                    strlcpy(resp, item.text, resp_size);
                }
                result = ESP_OK;
            } else {
                /* "+OK" ack before the queried value: keep waiting. */
                continue;
            }
            break;
        default:
            /* Unrelated line (boot banner, other query answer): ignore. */
            continue;
        }
        break;
    }

    unlock(dev);
    if (result == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "'%s' timed out after %u ms", cmd, (unsigned)timeout_ms);
    }
    return result;
}

/** Setter helper: send the command, expect "+OK" and let the module settle. */
static esp_err_t cmd_set(ryuw122_handle_t dev, const char *cmd)
{
    esp_err_t err = ryuw122_cmd(dev, cmd, NULL, NULL, 0, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RYUW122_SETTLE_MS));
    }
    return err;
}

static esp_err_t query_string(ryuw122_handle_t dev, const char *cmd, const char *prefix,
                              char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[RYUW122_LINE_MAX];
    esp_err_t err = ryuw122_cmd(dev, cmd, prefix, resp, sizeof(resp), 0);
    if (err != ESP_OK) {
        return err;
    }
    if (!ryuw122_parse_value(resp, prefix, out, out_size)) {
        ESP_LOGE(TAG, "cannot parse '%s' answer: %s", cmd, resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t query_int(ryuw122_handle_t dev, const char *cmd, const char *prefix,
                           int32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[RYUW122_LINE_MAX];
    esp_err_t err = ryuw122_cmd(dev, cmd, prefix, resp, sizeof(resp), 0);
    if (err != ESP_OK) {
        return err;
    }
    if (!ryuw122_parse_int(resp, prefix, out)) {
        ESP_LOGE(TAG, "cannot parse '%s' answer: %s", cmd, resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void ryuw122_free(ryuw122_handle_t dev)
{
    if (dev == NULL) {
        return;
    }
    if (dev->response_queue != NULL) {
        vQueueDelete(dev->response_queue);
    }
    if (dev->event_queue != NULL) {
        vQueueDelete(dev->event_queue);
    }
    if (dev->lock != NULL) {
        vSemaphoreDelete(dev->lock);
    }
    if (dev->rx_done != NULL) {
        vSemaphoreDelete(dev->rx_done);
    }
    free(dev);
}

esp_err_t ryuw122_init(const ryuw122_config_t *config, ryuw122_handle_t *out_dev)
{
    if (config == NULL || out_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->tx_gpio < 0 || config->rx_gpio < 0) {
        ESP_LOGE(TAG, "tx_gpio and rx_gpio must be configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->baud_rate <= 0 || config->event_queue_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ryuw122_handle_t dev = calloc(1, sizeof(struct ryuw122_dev_t));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    dev->uart_port = config->uart_port;
    dev->rst_gpio = config->rst_gpio;
    dev->baud_rate = config->baud_rate;
    dev->cmd_timeout_ms = (config->cmd_timeout_ms > 0) ? config->cmd_timeout_ms : 1000;
    dev->lock = xSemaphoreCreateRecursiveMutex();
    dev->response_queue = xQueueCreate(8, sizeof(ryuw122_line_t));
    dev->event_queue = xQueueCreate(config->event_queue_len, sizeof(ryuw122_event_t));
    dev->rx_done = xSemaphoreCreateBinary();
    if (dev->lock == NULL || dev->response_queue == NULL || dev->event_queue == NULL ||
        dev->rx_done == NULL) {
        ryuw122_free(dev);
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(dev->uart_port, RYUW122_UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ryuw122_free(dev);
        return err;
    }
    err = uart_param_config(dev->uart_port, &uart_config);
    if (err == ESP_OK) {
        err = uart_set_pin(dev->uart_port, config->tx_gpio, config->rx_gpio,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(dev->uart_port);
        ryuw122_free(dev);
        return err;
    }

    if (dev->rst_gpio >= 0) {
        const gpio_config_t rst_config = {
            .pin_bit_mask = 1ULL << dev->rst_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&rst_config);
        if (err == ESP_OK) {
            err = gpio_set_level(dev->rst_gpio, 1); /* NRST is active low */
        }
        if (err != ESP_OK) {
            uart_driver_delete(dev->uart_port);
            ryuw122_free(dev);
            return err;
        }
    }

    dev->running = true;
    if (xTaskCreate(rx_task, "ryuw122_rx",
                    (config->rx_task_stack > 0) ? config->rx_task_stack : 4096, dev,
                    (config->rx_task_priority > 0) ? config->rx_task_priority : 10,
                    &dev->rx_task) != pdPASS) {
        dev->running = false;
        uart_driver_delete(dev->uart_port);
        ryuw122_free(dev);
        return ESP_ERR_NO_MEM;
    }

    if (dev->rst_gpio >= 0) {
        err = ryuw122_hw_reset(dev);
        if (err != ESP_OK) {
            ryuw122_deinit(dev);
            return err;
        }
    }

    *out_dev = dev;
    ESP_LOGI(TAG, "initialised on UART%d (tx=%d rx=%d rst=%d, %d bps)",
             (int)dev->uart_port, config->tx_gpio, config->rx_gpio, dev->rst_gpio,
             dev->baud_rate);
    return ESP_OK;
}

esp_err_t ryuw122_deinit(ryuw122_handle_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->running = false;
    /* The RX task wakes up at most every RYUW122_RX_POLL_MS. */
    if (xSemaphoreTake(dev->rx_done, pdMS_TO_TICKS(RYUW122_RX_POLL_MS * 10)) != pdTRUE) {
        ESP_LOGE(TAG, "RX task did not stop, leaking the driver instance");
        return ESP_ERR_TIMEOUT;
    }
    uart_driver_delete(dev->uart_port);
    ryuw122_free(dev);
    return ESP_OK;
}

void ryuw122_set_event_cb(ryuw122_handle_t dev, ryuw122_event_cb_t cb, void *arg)
{
    if (dev == NULL) {
        return;
    }
    dev->event_cb_arg = arg;
    dev->event_cb = cb;
}

esp_err_t ryuw122_wait_event(ryuw122_handle_t dev, ryuw122_event_t *out_event,
                             uint32_t timeout_ms)
{
    if (dev == NULL || out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueReceive(dev->event_queue, out_event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ryuw122_hw_reset(ryuw122_handle_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->rst_gpio < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    lock(dev);
    esp_err_t err = gpio_set_level(dev->rst_gpio, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RYUW122_RESET_LOW_MS));
        err = gpio_set_level(dev->rst_gpio, 1);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RYUW122_RESET_BOOT_MS));
        xQueueReset(dev->response_queue); /* drop the boot banner */
    }
    unlock(dev);
    return err;
}

/* ------------------------------------------------------------------ */
/* Generic commands                                                   */
/* ------------------------------------------------------------------ */

esp_err_t ryuw122_test(ryuw122_handle_t dev)
{
    return ryuw122_cmd(dev, "AT", NULL, NULL, 0, 0);
}

esp_err_t ryuw122_soft_reset(ryuw122_handle_t dev)
{
    esp_err_t err = ryuw122_cmd(dev, "AT+RESET", "+RESET", NULL, 0, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RYUW122_RESET_BOOT_MS));
    }
    return err;
}

esp_err_t ryuw122_factory_reset(ryuw122_handle_t dev)
{
    esp_err_t err = ryuw122_cmd(dev, "AT+FACTORY", "+FACTORY", NULL, 0, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RYUW122_RESET_BOOT_MS));
    }
    return err;
}

esp_err_t ryuw122_get_version(ryuw122_handle_t dev, char *version, size_t size)
{
    return query_string(dev, "AT+VER?", "+VER=", version, size);
}

esp_err_t ryuw122_get_uid(ryuw122_handle_t dev, char *uid, size_t size)
{
    return query_string(dev, "AT+UID?", "+UID=", uid, size);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

esp_err_t ryuw122_set_mode(ryuw122_handle_t dev, ryuw122_mode_t mode)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+MODE=%d", (int)mode);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_mode(ryuw122_handle_t dev, ryuw122_mode_t *out_mode)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+MODE?", "+MODE=", &value);
    if (err == ESP_OK) {
        *out_mode = (ryuw122_mode_t)value;
    }
    return err;
}

/** ADDRESS / NETWORKID must be exactly 8 printable ASCII characters. */
static bool valid_ascii_id(const char *id)
{
    if (id == NULL || strlen(id) != RYUW122_ADDR_LEN) {
        return false;
    }
    for (size_t i = 0; i < RYUW122_ADDR_LEN; i++) {
        if (!isprint((unsigned char)id[i]) || id[i] == ',') {
            return false;
        }
    }
    return true;
}

esp_err_t ryuw122_set_network_id(ryuw122_handle_t dev, const char *network_id)
{
    if (!valid_ascii_id(network_id)) {
        ESP_LOGE(TAG, "network id must be 8 printable ASCII characters");
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+NETWORKID=%s", network_id);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_network_id(ryuw122_handle_t dev, char *network_id, size_t size)
{
    return query_string(dev, "AT+NETWORKID?", "+NETWORKID=", network_id, size);
}

esp_err_t ryuw122_set_address(ryuw122_handle_t dev, const char *address)
{
    if (!valid_ascii_id(address)) {
        ESP_LOGE(TAG, "address must be 8 printable ASCII characters");
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%s", address);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_address(ryuw122_handle_t dev, char *address, size_t size)
{
    return query_string(dev, "AT+ADDRESS?", "+ADDRESS=", address, size);
}

esp_err_t ryuw122_set_password(ryuw122_handle_t dev, const char *password)
{
    if (password == NULL || strlen(password) != RYUW122_PASSWORD_LEN) {
        ESP_LOGE(TAG, "password must be %d hex characters", RYUW122_PASSWORD_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < RYUW122_PASSWORD_LEN; i++) {
        if (!isxdigit((unsigned char)password[i])) {
            ESP_LOGE(TAG, "password must be a hex string");
            return ESP_ERR_INVALID_ARG;
        }
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CPIN=%s", password);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_password(ryuw122_handle_t dev, char *password, size_t size)
{
    return query_string(dev, "AT+CPIN?", "+CPIN=", password, size);
}

esp_err_t ryuw122_set_channel(ryuw122_handle_t dev, ryuw122_channel_t channel)
{
    if (channel != RYUW122_CHANNEL_5 && channel != RYUW122_CHANNEL_9) {
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CHANNEL=%d", (int)channel);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_channel(ryuw122_handle_t dev, ryuw122_channel_t *out_channel)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+CHANNEL?", "+CHANNEL=", &value);
    if (err == ESP_OK) {
        *out_channel = (ryuw122_channel_t)value;
    }
    return err;
}

esp_err_t ryuw122_set_bandwidth(ryuw122_handle_t dev, ryuw122_bandwidth_t bandwidth)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+BANDWIDTH=%d", (int)bandwidth);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_bandwidth(ryuw122_handle_t dev, ryuw122_bandwidth_t *out_bandwidth)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+BANDWIDTH?", "+BANDWIDTH=", &value);
    if (err == ESP_OK) {
        *out_bandwidth = (ryuw122_bandwidth_t)value;
    }
    return err;
}

esp_err_t ryuw122_set_rf_power(ryuw122_handle_t dev, ryuw122_rf_power_t power)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", (int)power);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_rf_power(ryuw122_handle_t dev, ryuw122_rf_power_t *out_power)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+CRFOP?", "+CRFOP=", &value);
    if (err == ESP_OK) {
        *out_power = (ryuw122_rf_power_t)value;
    }
    return err;
}

esp_err_t ryuw122_set_rssi_report(ryuw122_handle_t dev, bool enable)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+RSSI=%d", enable ? 1 : 0);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_rssi_report(ryuw122_handle_t dev, bool *out_enable)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+RSSI?", "+RSSI=", &value);
    if (err == ESP_OK) {
        *out_enable = (value != 0);
    }
    return err;
}

esp_err_t ryuw122_set_tag_duty_cycle(ryuw122_handle_t dev, int rf_on_ms, int rf_off_ms)
{
    if (rf_on_ms < 10 || rf_on_ms > 28000 || rf_off_ms < 10 || rf_off_ms > 28000) {
        ESP_LOGE(TAG, "duty cycle times must be within 10..28000 ms");
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+TAGD=%d,%d", rf_on_ms, rf_off_ms);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_tag_duty_cycle(ryuw122_handle_t dev, int *out_on_ms, int *out_off_ms)
{
    if (out_on_ms == NULL || out_off_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[RYUW122_LINE_MAX];
    esp_err_t err = ryuw122_cmd(dev, "AT+TAGD?", "+TAGD=", resp, sizeof(resp), 0);
    if (err != ESP_OK) {
        return err;
    }
    if (sscanf(resp, "+TAGD=%d,%d", out_on_ms, out_off_ms) != 2) {
        ESP_LOGE(TAG, "cannot parse 'AT+TAGD?' answer: %s", resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ryuw122_set_calibration(ryuw122_handle_t dev, int calibration)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CAL=%d", calibration);
    return cmd_set(dev, cmd);
}

esp_err_t ryuw122_get_calibration(ryuw122_handle_t dev, int *out_calibration)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+CAL?", "+CAL=", &value);
    if (err == ESP_OK) {
        *out_calibration = (int)value;
    }
    return err;
}

esp_err_t ryuw122_set_baud_rate(ryuw122_handle_t dev, int baud_rate)
{
    if (baud_rate != 9600 && baud_rate != 57600 && baud_rate != 115200) {
        ESP_LOGE(TAG, "unsupported baud rate %d", baud_rate);
        return ESP_ERR_INVALID_ARG;
    }
    lock(dev);
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+IPR=%d", baud_rate);
    esp_err_t err = ryuw122_cmd(dev, cmd, NULL, NULL, 0, 0);
    if (err == ESP_OK) {
        /* Let the module finish answering at the old rate before switching. */
        vTaskDelay(pdMS_TO_TICKS(RYUW122_SETTLE_MS));
        err = uart_set_baudrate(dev->uart_port, baud_rate);
        if (err == ESP_OK) {
            dev->baud_rate = baud_rate;
            uart_flush_input(dev->uart_port);
            xQueueReset(dev->response_queue);
        }
    }
    unlock(dev);
    return err;
}

esp_err_t ryuw122_get_baud_rate(ryuw122_handle_t dev, int *out_baud_rate)
{
    int32_t value = 0;
    esp_err_t err = query_int(dev, "AT+IPR?", "+IPR=", &value);
    if (err == ESP_OK) {
        *out_baud_rate = (int)value;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Ranging / data exchange                                            */
/* ------------------------------------------------------------------ */

/*
 * The payload travels inside a text AT command, so bytes that would break the
 * command framing (control characters and ',') cannot be sent as-is. Encode
 * binary payloads (e.g. as hex) before handing them to the driver.
 */
static esp_err_t check_payload(const void *data, size_t len)
{
    if (len > RYUW122_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "payload of %u bytes exceeds the %d byte limit",
                 (unsigned)len, RYUW122_MAX_PAYLOAD);
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        if (!isprint(bytes[i])) {
            ESP_LOGE(TAG, "payload byte %u (0x%02x) is not printable ASCII",
                     (unsigned)i, bytes[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t build_anchor_send(char *cmd, size_t cmd_size, const char *tag_address,
                                   const void *data, size_t len)
{
    if (!valid_ascii_id(tag_address)) {
        ESP_LOGE(TAG, "tag address must be 8 printable ASCII characters");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = check_payload(data, len);
    if (err != ESP_OK) {
        return err;
    }
    int written = snprintf(cmd, cmd_size, "AT+ANCHOR_SEND=%s,%u,%.*s", tag_address,
                           (unsigned)len, (int)len, (const char *)(data ? data : ""));
    if (written <= 0 || (size_t)written >= cmd_size) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ryuw122_anchor_send(ryuw122_handle_t dev, const char *tag_address,
                              const void *data, size_t len)
{
    char cmd[RYUW122_LINE_MAX];
    esp_err_t err = build_anchor_send(cmd, sizeof(cmd), tag_address, data, len);
    if (err != ESP_OK) {
        return err;
    }
    return ryuw122_cmd(dev, cmd, NULL, NULL, 0, 0);
}

esp_err_t ryuw122_anchor_range(ryuw122_handle_t dev, const char *tag_address,
                               const void *data, size_t len,
                               ryuw122_anchor_rcv_t *out_result, uint32_t timeout_ms)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[RYUW122_LINE_MAX];
    esp_err_t err = build_anchor_send(cmd, sizeof(cmd), tag_address, data, len);
    if (err != ESP_OK) {
        return err;
    }
    if (timeout_ms == 0) {
        timeout_ms = dev->cmd_timeout_ms;
    }

    lock(dev);
    /* Drop reports left over from a previous exchange that timed out. */
    xQueueReset(dev->event_queue);

    err = ryuw122_cmd(dev, cmd, NULL, NULL, 0, 0);
    if (err != ESP_OK) {
        unlock(dev);
        return err;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    err = ESP_ERR_TIMEOUT;
    for (;;) {
        int32_t remaining = (int32_t)(deadline - xTaskGetTickCount());
        if (remaining <= 0) {
            break;
        }
        ryuw122_event_t event;
        if (xQueueReceive(dev->event_queue, &event, (TickType_t)remaining) != pdTRUE) {
            break;
        }
        if (event.type != RYUW122_EVENT_ANCHOR_RCV ||
            strcmp(event.anchor.addr, tag_address) != 0) {
            continue; /* report from another tag */
        }
        if (out_result != NULL) {
            *out_result = event.anchor;
        }
        err = ESP_OK;
        break;
    }
    unlock(dev);
    return err;
}

esp_err_t ryuw122_tag_set_payload(ryuw122_handle_t dev, const void *data, size_t len)
{
    esp_err_t err = check_payload(data, len);
    if (err != ESP_OK) {
        return err;
    }
    char cmd[RYUW122_LINE_MAX];
    int written = snprintf(cmd, sizeof(cmd), "AT+TAG_SEND=%u,%.*s", (unsigned)len,
                           (int)len, (const char *)(data ? data : ""));
    if (written <= 0 || (size_t)written >= sizeof(cmd)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ryuw122_cmd(dev, cmd, NULL, NULL, 0, 0);
}
