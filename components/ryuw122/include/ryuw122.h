/*
 * RYUW122 UWB transceiver driver for ESP-IDF (tested target: ESP32-C6).
 *
 * The module is driven over UART with the REYAX AT command set. A background
 * task reads lines from the UART, routes command responses to the caller of
 * ryuw122_cmd() and delivers unsolicited "+ANCHOR_RCV=" / "+TAG_RCV=" reports
 * as events (queue and/or callback).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "ryuw122_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque driver handle. */
typedef struct ryuw122_dev_t *ryuw122_handle_t;

/** AT+MODE - operating mode. */
typedef enum {
    RYUW122_MODE_TAG = 0,     /**< TAG (mobile node, answers ANCHOR requests) */
    RYUW122_MODE_ANCHOR = 1,  /**< ANCHOR (initiates the ranging exchange) */
    RYUW122_MODE_SLEEP = 2,   /**< low power sleep */
} ryuw122_mode_t;

/** AT+CHANNEL - UWB channel. */
typedef enum {
    RYUW122_CHANNEL_5 = 5,  /**< 6489.6 MHz */
    RYUW122_CHANNEL_9 = 9,  /**< 7987.2 MHz */
} ryuw122_channel_t;

/** AT+BANDWIDTH - over the air data rate. */
typedef enum {
    RYUW122_BANDWIDTH_850K = 0, /**< 850 kbps (longer range) */
    RYUW122_BANDWIDTH_6M8 = 1,  /**< 6.8 Mbps (shorter exchange) */
} ryuw122_bandwidth_t;

/** AT+CRFOP - RF output power. */
typedef enum {
    RYUW122_RF_POWER_M65DBM = 0,
    RYUW122_RF_POWER_M50DBM = 1,
    RYUW122_RF_POWER_M45DBM = 2,
    RYUW122_RF_POWER_M40DBM = 3,
    RYUW122_RF_POWER_M35DBM = 4,
    RYUW122_RF_POWER_M32DBM = 5,
} ryuw122_rf_power_t;

/** Driver configuration. */
typedef struct {
    uart_port_t uart_port;   /**< UART peripheral used to talk to the module */
    int tx_gpio;             /**< ESP32 TX -> module RX */
    int rx_gpio;             /**< ESP32 RX <- module TX */
    int rst_gpio;            /**< module NRST (active low), -1 if not wired */
    int baud_rate;           /**< module UART baud rate (default 115200) */
    uint32_t cmd_timeout_ms; /**< default timeout for AT commands */
    int rx_task_stack;       /**< stack size of the RX task, bytes */
    int rx_task_priority;    /**< priority of the RX task */
    int event_queue_len;     /**< depth of the unsolicited event queue */
} ryuw122_config_t;

/** Reasonable defaults; override the GPIOs for your board. */
#define RYUW122_DEFAULT_CONFIG()      \
    {                                 \
        .uart_port = UART_NUM_1,      \
        .tx_gpio = -1,                \
        .rx_gpio = -1,                \
        .rst_gpio = -1,               \
        .baud_rate = 115200,          \
        .cmd_timeout_ms = 1000,       \
        .rx_task_stack = 4096,        \
        .rx_task_priority = 10,       \
        .event_queue_len = 8,         \
    }

/** Unsolicited report from the module. */
typedef enum {
    RYUW122_EVENT_ANCHOR_RCV, /**< answer of a TAG to our ranging request */
    RYUW122_EVENT_TAG_RCV,    /**< data pushed to us by an ANCHOR (TAG mode) */
} ryuw122_event_type_t;

typedef struct {
    ryuw122_event_type_t type;
    union {
        ryuw122_anchor_rcv_t anchor; /**< valid for RYUW122_EVENT_ANCHOR_RCV */
        ryuw122_tag_rcv_t tag;       /**< valid for RYUW122_EVENT_TAG_RCV */
    };
} ryuw122_event_t;

/**
 * Event callback. Invoked from the driver RX task, so keep it short and do not
 * call blocking ryuw122_* functions from it.
 */
typedef void (*ryuw122_event_cb_t)(const ryuw122_event_t *event, void *arg);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/** Install the UART driver, reset the module (if NRST is wired) and start RX. */
esp_err_t ryuw122_init(const ryuw122_config_t *config, ryuw122_handle_t *out_dev);

/** Stop the RX task, uninstall the UART driver and free the handle. */
esp_err_t ryuw122_deinit(ryuw122_handle_t dev);

/** Register (or clear, with cb == NULL) the unsolicited event callback. */
void ryuw122_set_event_cb(ryuw122_handle_t dev, ryuw122_event_cb_t cb, void *arg);

/** Wait for the next unsolicited event. Returns ESP_ERR_TIMEOUT on timeout. */
esp_err_t ryuw122_wait_event(ryuw122_handle_t dev, ryuw122_event_t *out_event,
                             uint32_t timeout_ms);

/** Pulse NRST low to hardware reset the module (requires rst_gpio >= 0). */
esp_err_t ryuw122_hw_reset(ryuw122_handle_t dev);

/* ------------------------------------------------------------------ */
/* Generic AT access                                                  */
/* ------------------------------------------------------------------ */

/**
 * Send one AT command and wait for its answer.
 *
 * @param cmd          command without line terminator, e.g. "AT+MODE?".
 * @param expect       expected response prefix (e.g. "+MODE="); NULL waits for "+OK".
 * @param resp         buffer receiving the matched line; may be NULL.
 * @param resp_size    size of resp.
 * @param timeout_ms   0 uses the configured default timeout.
 *
 * @return ESP_OK, ESP_ERR_TIMEOUT, or ESP_ERR_INVALID_RESPONSE when the module
 *         answered "+ERR=n" (the code is logged).
 */
esp_err_t ryuw122_cmd(ryuw122_handle_t dev, const char *cmd, const char *expect,
                      char *resp, size_t resp_size, uint32_t timeout_ms);

/** Send a bare "AT" and check that the module answers "+OK". */
esp_err_t ryuw122_test(ryuw122_handle_t dev);

/** AT+RESET - software reset. */
esp_err_t ryuw122_soft_reset(ryuw122_handle_t dev);

/** AT+FACTORY - restore factory settings. */
esp_err_t ryuw122_factory_reset(ryuw122_handle_t dev);

/** AT+VER? - firmware version string. */
esp_err_t ryuw122_get_version(ryuw122_handle_t dev, char *version, size_t size);

/** AT+UID? - unique chip id. */
esp_err_t ryuw122_get_uid(ryuw122_handle_t dev, char *uid, size_t size);

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

esp_err_t ryuw122_set_mode(ryuw122_handle_t dev, ryuw122_mode_t mode);
esp_err_t ryuw122_get_mode(ryuw122_handle_t dev, ryuw122_mode_t *out_mode);

/** AT+NETWORKID - 8 byte ASCII network group; peers must share it. */
esp_err_t ryuw122_set_network_id(ryuw122_handle_t dev, const char *network_id);
esp_err_t ryuw122_get_network_id(ryuw122_handle_t dev, char *network_id, size_t size);

/** AT+ADDRESS - 8 byte ASCII address of this node. */
esp_err_t ryuw122_set_address(ryuw122_handle_t dev, const char *address);
esp_err_t ryuw122_get_address(ryuw122_handle_t dev, char *address, size_t size);

/** AT+CPIN - AES128 password, 32 hex characters; must match on both sides. */
esp_err_t ryuw122_set_password(ryuw122_handle_t dev, const char *password);
esp_err_t ryuw122_get_password(ryuw122_handle_t dev, char *password, size_t size);

esp_err_t ryuw122_set_channel(ryuw122_handle_t dev, ryuw122_channel_t channel);
esp_err_t ryuw122_get_channel(ryuw122_handle_t dev, ryuw122_channel_t *out_channel);

esp_err_t ryuw122_set_bandwidth(ryuw122_handle_t dev, ryuw122_bandwidth_t bandwidth);
esp_err_t ryuw122_get_bandwidth(ryuw122_handle_t dev, ryuw122_bandwidth_t *out_bandwidth);

esp_err_t ryuw122_set_rf_power(ryuw122_handle_t dev, ryuw122_rf_power_t power);
esp_err_t ryuw122_get_rf_power(ryuw122_handle_t dev, ryuw122_rf_power_t *out_power);

/** AT+RSSI - append the RSSI field to the RCV reports. */
esp_err_t ryuw122_set_rssi_report(ryuw122_handle_t dev, bool enable);
esp_err_t ryuw122_get_rssi_report(ryuw122_handle_t dev, bool *out_enable);

/** AT+TAGD - TAG RF duty cycle, both values in ms (10..28000). */
esp_err_t ryuw122_set_tag_duty_cycle(ryuw122_handle_t dev, int rf_on_ms, int rf_off_ms);
esp_err_t ryuw122_get_tag_duty_cycle(ryuw122_handle_t dev, int *out_on_ms, int *out_off_ms);

/** AT+CAL - distance calibration offset applied by the module. */
esp_err_t ryuw122_set_calibration(ryuw122_handle_t dev, int calibration);
esp_err_t ryuw122_get_calibration(ryuw122_handle_t dev, int *out_calibration);

/**
 * AT+IPR - change the module UART baud rate and re-configure the local UART to
 * match. Only 9600, 57600 and 115200 are supported by the module.
 */
esp_err_t ryuw122_set_baud_rate(ryuw122_handle_t dev, int baud_rate);
esp_err_t ryuw122_get_baud_rate(ryuw122_handle_t dev, int *out_baud_rate);

/* ------------------------------------------------------------------ */
/* Ranging / data exchange                                            */
/* ------------------------------------------------------------------ */

/**
 * ANCHOR mode: start one ranging exchange with a TAG and wait for its answer.
 *
 * @param tag_address 8 character address of the target TAG.
 * @param data        payload sent to the TAG, may be NULL when len is 0.
 * @param len         payload length, 0..RYUW122_MAX_PAYLOAD.
 * @param out_result  receives distance, payload and RSSI; may be NULL.
 * @param timeout_ms  0 uses the configured default timeout.
 *
 * @return ESP_OK when the TAG answered, ESP_ERR_TIMEOUT when it did not
 *         (out of range, powered off or a different network id).
 */
esp_err_t ryuw122_anchor_range(ryuw122_handle_t dev, const char *tag_address,
                               const void *data, size_t len,
                               ryuw122_anchor_rcv_t *out_result, uint32_t timeout_ms);

/** ANCHOR mode: send the request without waiting for "+ANCHOR_RCV=". */
esp_err_t ryuw122_anchor_send(ryuw122_handle_t dev, const char *tag_address,
                              const void *data, size_t len);

/**
 * TAG mode: stage the payload that will be returned to the next ANCHOR
 * request. Must be re-armed after every exchange.
 */
esp_err_t ryuw122_tag_set_payload(ryuw122_handle_t dev, const void *data, size_t len);

/** Convenience: centimetres -> metres. */
static inline float ryuw122_cm_to_m(int32_t distance_cm)
{
    return (float)distance_cm / 100.0f;
}

#ifdef __cplusplus
}
#endif
