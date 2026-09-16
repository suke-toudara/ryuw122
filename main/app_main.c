/*
 * RYUW122 UWB firmware for ESP32-C6.
 *
 * Build one board as ANCHOR and another one as TAG (menuconfig ->
 * "RYUW122 UWB firmware"). The anchor periodically ranges against the tag and
 * prints the distance; the tag answers every request with a small payload.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ryuw122.h"
#include "sdkconfig.h"

static const char *TAG = "uwb";

#if CONFIG_RYUW122_CHANNEL_9
#define APP_CHANNEL RYUW122_CHANNEL_9
#else
#define APP_CHANNEL RYUW122_CHANNEL_5
#endif

#if CONFIG_RYUW122_BANDWIDTH_6M8
#define APP_BANDWIDTH RYUW122_BANDWIDTH_6M8
#else
#define APP_BANDWIDTH RYUW122_BANDWIDTH_850K
#endif

#if CONFIG_RYUW122_ROLE_ANCHOR
#define APP_MODE RYUW122_MODE_ANCHOR
#else
#define APP_MODE RYUW122_MODE_TAG
#endif

#if CONFIG_RYUW122_RSSI_REPORT
#define APP_RSSI_REPORT true
#else
#define APP_RSSI_REPORT false
#endif

/** Weight of the newest sample in the exponential moving average. */
#define APP_EMA_ALPHA 0.3f

/* ------------------------------------------------------------------ */
/* Bring-up                                                           */
/* ------------------------------------------------------------------ */

/** Poll the module with a bare "AT" until it answers, resetting if needed. */
static esp_err_t wait_for_module(ryuw122_handle_t dev)
{
    for (int attempt = 1; attempt <= 5; attempt++) {
        if (ryuw122_test(dev) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "module did not answer (attempt %d/5)", attempt);
        if (ryuw122_hw_reset(dev) == ESP_ERR_NOT_SUPPORTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    ESP_LOGE(TAG, "module is not responding - check wiring, 3.3V supply and baud rate");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t configure_module(ryuw122_handle_t dev)
{
    ESP_RETURN_ON_ERROR(ryuw122_set_mode(dev, APP_MODE), TAG, "cannot set mode");
    ESP_RETURN_ON_ERROR(ryuw122_set_network_id(dev, CONFIG_RYUW122_NETWORK_ID), TAG,
                        "cannot set network id");
    ESP_RETURN_ON_ERROR(ryuw122_set_address(dev, CONFIG_RYUW122_LOCAL_ADDRESS), TAG,
                        "cannot set address");
#if CONFIG_RYUW122_USE_PASSWORD
    ESP_RETURN_ON_ERROR(ryuw122_set_password(dev, CONFIG_RYUW122_PASSWORD), TAG,
                        "cannot set password");
#endif
    ESP_RETURN_ON_ERROR(ryuw122_set_channel(dev, APP_CHANNEL), TAG, "cannot set channel");
    ESP_RETURN_ON_ERROR(ryuw122_set_bandwidth(dev, APP_BANDWIDTH), TAG,
                        "cannot set bandwidth");
    ESP_RETURN_ON_ERROR(ryuw122_set_rf_power(dev, (ryuw122_rf_power_t)CONFIG_RYUW122_RF_POWER),
                        TAG, "cannot set RF power");
    ESP_RETURN_ON_ERROR(ryuw122_set_rssi_report(dev, APP_RSSI_REPORT), TAG,
                        "cannot set RSSI reporting");
#if CONFIG_RYUW122_SET_CALIBRATION
    ESP_RETURN_ON_ERROR(ryuw122_set_calibration(dev, CONFIG_RYUW122_CALIBRATION), TAG,
                        "cannot set calibration");
#endif
#if CONFIG_RYUW122_ROLE_TAG && CONFIG_RYUW122_TAG_DUTY_CYCLE
    ESP_RETURN_ON_ERROR(ryuw122_set_tag_duty_cycle(dev, CONFIG_RYUW122_TAG_DUTY_ON_MS,
                                                   CONFIG_RYUW122_TAG_DUTY_OFF_MS),
                        TAG, "cannot set tag duty cycle");
#endif
    return ESP_OK;
}

/** Read the settings back from the module so the log shows what is active. */
static void log_module_state(ryuw122_handle_t dev)
{
    char text[64];

    if (ryuw122_get_version(dev, text, sizeof(text)) == ESP_OK) {
        ESP_LOGI(TAG, "firmware     : %s", text);
    }
    if (ryuw122_get_uid(dev, text, sizeof(text)) == ESP_OK) {
        ESP_LOGI(TAG, "uid          : %s", text);
    }
    if (ryuw122_get_address(dev, text, sizeof(text)) == ESP_OK) {
        ESP_LOGI(TAG, "address      : %s", text);
    }
    if (ryuw122_get_network_id(dev, text, sizeof(text)) == ESP_OK) {
        ESP_LOGI(TAG, "network id   : %s", text);
    }

    ryuw122_mode_t mode;
    if (ryuw122_get_mode(dev, &mode) == ESP_OK) {
        ESP_LOGI(TAG, "mode         : %s",
                 (mode == RYUW122_MODE_ANCHOR) ? "ANCHOR"
                 : (mode == RYUW122_MODE_TAG)  ? "TAG"
                                               : "SLEEP");
    }
    ryuw122_channel_t channel;
    if (ryuw122_get_channel(dev, &channel) == ESP_OK) {
        ESP_LOGI(TAG, "channel      : %d (%s)", (int)channel,
                 (channel == RYUW122_CHANNEL_9) ? "7987.2 MHz" : "6489.6 MHz");
    }
    ryuw122_bandwidth_t bandwidth;
    if (ryuw122_get_bandwidth(dev, &bandwidth) == ESP_OK) {
        ESP_LOGI(TAG, "bandwidth    : %s",
                 (bandwidth == RYUW122_BANDWIDTH_6M8) ? "6.8 Mbps" : "850 kbps");
    }
    int calibration;
    if (ryuw122_get_calibration(dev, &calibration) == ESP_OK) {
        ESP_LOGI(TAG, "calibration  : %d", calibration);
    }
}

/* ------------------------------------------------------------------ */
/* ANCHOR role                                                        */
/* ------------------------------------------------------------------ */

#if CONFIG_RYUW122_ROLE_ANCHOR

static void anchor_loop(ryuw122_handle_t dev)
{
    const char *tag_address = CONFIG_RYUW122_PEER_TAG_ADDRESS;
    uint32_t sequence = 0;
    uint32_t misses = 0;
    float ema_cm = 0.0f;
    bool ema_valid = false;

    ESP_LOGI(TAG, "ranging against tag '%s' every %d ms", tag_address,
             CONFIG_RYUW122_RANGING_INTERVAL_MS);

    while (true) {
        char payload[RYUW122_MAX_PAYLOAD + 1];
        int len = snprintf(payload, sizeof(payload), "SEQ%05" PRIu32, sequence % 100000);

        ryuw122_anchor_rcv_t result;
        esp_err_t err = ryuw122_anchor_range(dev, tag_address, payload, (size_t)len,
                                             &result, CONFIG_RYUW122_RANGING_TIMEOUT_MS);
        if (err == ESP_OK) {
            ema_cm = ema_valid ? (APP_EMA_ALPHA * (float)result.distance_cm +
                                  (1.0f - APP_EMA_ALPHA) * ema_cm)
                               : (float)result.distance_cm;
            ema_valid = true;

            if (result.rssi_valid) {
                ESP_LOGI(TAG, "#%" PRIu32 " distance %" PRId32 " cm (%.2f m, avg %.2f m)"
                              ", rssi %" PRId32 " dBm, tag payload \"%s\"",
                         sequence, result.distance_cm, ryuw122_cm_to_m(result.distance_cm),
                         ema_cm / 100.0f, result.rssi_dbm, result.data);
            } else {
                ESP_LOGI(TAG, "#%" PRIu32 " distance %" PRId32 " cm (%.2f m, avg %.2f m)"
                              ", tag payload \"%s\"",
                         sequence, result.distance_cm, ryuw122_cm_to_m(result.distance_cm),
                         ema_cm / 100.0f, result.data);
            }
            misses = 0;
        } else {
            misses++;
            /* Stay quiet while the tag is simply out of range. */
            if (misses == 1 || misses % 10 == 0) {
                ESP_LOGW(TAG, "no answer from '%s' (%" PRIu32 " in a row): %s", tag_address,
                         misses, esp_err_to_name(err));
            }
            ema_valid = false;
        }

        sequence++;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RYUW122_RANGING_INTERVAL_MS));
    }
}

#else /* CONFIG_RYUW122_ROLE_TAG */

/* ------------------------------------------------------------------ */
/* TAG role                                                           */
/* ------------------------------------------------------------------ */

/*
 * The payload staged with AT+TAG_SEND is consumed by one anchor request, so it
 * has to be re-armed after every exchange.
 */
static esp_err_t tag_arm_payload(ryuw122_handle_t dev, uint32_t counter)
{
    char payload[RYUW122_MAX_PAYLOAD + 1];
    int len = snprintf(payload, sizeof(payload), "ACK%05" PRIu32, counter % 100000);
    return ryuw122_tag_set_payload(dev, payload, (size_t)len);
}

static void tag_loop(ryuw122_handle_t dev)
{
    uint32_t counter = 0;

    ESP_LOGI(TAG, "tag '%s' waiting for anchor requests", CONFIG_RYUW122_LOCAL_ADDRESS);
    esp_err_t armed = tag_arm_payload(dev, counter);
    if (armed != ESP_OK) {
        ESP_LOGW(TAG, "cannot arm the answer payload: %s", esp_err_to_name(armed));
    }

    while (true) {
        ryuw122_event_t event;
        esp_err_t err = ryuw122_wait_event(dev, &event, 5000);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "no anchor request in the last 5 s");
            continue;
        }
        if (event.type != RYUW122_EVENT_TAG_RCV) {
            continue;
        }

        counter++;
        if (event.tag.rssi_valid) {
            ESP_LOGI(TAG, "#%" PRIu32 " anchor payload \"%s\" (%u bytes), rssi %" PRId32 " dBm",
                     counter, event.tag.data, (unsigned)event.tag.payload_len,
                     event.tag.rssi_dbm);
        } else {
            ESP_LOGI(TAG, "#%" PRIu32 " anchor payload \"%s\" (%u bytes)", counter,
                     event.tag.data, (unsigned)event.tag.payload_len);
        }

        armed = tag_arm_payload(dev, counter);
        if (armed != ESP_OK) {
            ESP_LOGW(TAG, "cannot re-arm the answer payload: %s", esp_err_to_name(armed));
        }
    }
}

#endif /* CONFIG_RYUW122_ROLE_ANCHOR */

/* ------------------------------------------------------------------ */

/** Log the failure and reboot: a half configured radio is worse than a retry. */
static void fatal(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed: %s - restarting in 5 s", what, esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

void app_main(void)
{
    ryuw122_config_t config = RYUW122_DEFAULT_CONFIG();
    config.uart_port = (uart_port_t)CONFIG_RYUW122_UART_PORT;
    config.tx_gpio = CONFIG_RYUW122_UART_TX_GPIO;
    config.rx_gpio = CONFIG_RYUW122_UART_RX_GPIO;
    config.rst_gpio = CONFIG_RYUW122_RST_GPIO;
    config.baud_rate = CONFIG_RYUW122_UART_BAUD;

    ESP_LOGI(TAG, "RYUW122 UWB firmware, role %s",
             (APP_MODE == RYUW122_MODE_ANCHOR) ? "ANCHOR" : "TAG");

    ryuw122_handle_t dev = NULL;
    esp_err_t err = ryuw122_init(&config, &dev);
    if (err != ESP_OK) {
        fatal("driver init", err);
    }

    err = wait_for_module(dev);
    if (err != ESP_OK) {
        fatal("module probe", err);
    }

    err = configure_module(dev);
    if (err != ESP_OK) {
        fatal("module configuration", err);
    }
    log_module_state(dev);

#if CONFIG_RYUW122_ROLE_ANCHOR
    anchor_loop(dev);
#else
    tag_loop(dev);
#endif
}
