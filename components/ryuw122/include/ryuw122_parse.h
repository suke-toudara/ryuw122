/*
 * RYUW122 UWB module - protocol parsing layer (pure C, no ESP-IDF dependency)
 *
 * Reference: REYAX RYUW122 UART Interface 6.5GHz / 8GHz UWB Antenna
 * Transceiver Module datasheet (AT command set).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ADDRESS / NETWORKID are 8 byte ASCII strings. */
#define RYUW122_ADDR_LEN 8
#define RYUW122_NETWORKID_LEN 8
/** AES128 password (AT+CPIN) is a 32 character hex string. */
#define RYUW122_PASSWORD_LEN 32
/** Maximum payload carried by AT+ANCHOR_SEND / AT+TAG_SEND. */
#define RYUW122_MAX_PAYLOAD 12
/** Longest line we are willing to keep from the module. */
#define RYUW122_LINE_MAX 160

/** Classification of one line received from the module. */
typedef enum {
    RYUW122_LINE_EMPTY = 0,   /**< blank line */
    RYUW122_LINE_OK,          /**< "+OK" */
    RYUW122_LINE_ERR,         /**< "+ERR=n" */
    RYUW122_LINE_ANCHOR_RCV,  /**< "+ANCHOR_RCV=..." (unsolicited) */
    RYUW122_LINE_TAG_RCV,     /**< "+TAG_RCV=..." (unsolicited) */
    RYUW122_LINE_RESPONSE,    /**< any other "+XXX=..." query response */
    RYUW122_LINE_UNKNOWN,     /**< anything else (boot banner, noise, ...) */
} ryuw122_line_kind_t;

/** Payload of "+ANCHOR_RCV=<addr>,<len>,<data>,<distance>,<rssi>". */
typedef struct {
    char addr[RYUW122_ADDR_LEN + 1];      /**< address of the answering TAG */
    uint8_t payload_len;                  /**< 0..RYUW122_MAX_PAYLOAD */
    char data[RYUW122_MAX_PAYLOAD + 1];   /**< payload sent back by the TAG */
    int32_t distance_cm;                  /**< measured distance in cm */
    int32_t rssi_dbm;                     /**< RSSI, valid only if rssi_valid */
    bool rssi_valid;                      /**< false when AT+RSSI=0 */
} ryuw122_anchor_rcv_t;

/** Payload of "+TAG_RCV=<len>,<data>,<rssi>". */
typedef struct {
    uint8_t payload_len;
    char data[RYUW122_MAX_PAYLOAD + 1];
    int32_t rssi_dbm;
    bool rssi_valid;
} ryuw122_tag_rcv_t;

/** Error codes reported by the module as "+ERR=n". */
typedef enum {
    RYUW122_ERR_NO_CRLF = 1,       /**< missing <CR><LF> */
    RYUW122_ERR_BAD_HEADER = 2,    /**< command does not start with "AT" */
    RYUW122_ERR_BAD_PARAM = 3,     /**< parameter failure */
    RYUW122_ERR_CMD_FAILURE = 4,   /**< command failure */
    RYUW122_ERR_UNKNOWN_CMD = 5,   /**< unknown command */
} ryuw122_err_code_t;

/** Strip leading/trailing whitespace in place; returns the new length. */
size_t ryuw122_trim(char *s);

/** Classify one (already trimmed) line coming from the module. */
ryuw122_line_kind_t ryuw122_classify_line(const char *line);

/**
 * Parse "+ANCHOR_RCV=...".
 *
 * The payload itself may legally contain ',', so the declared payload length
 * is used to slice the data field instead of naively splitting on commas.
 *
 * @return true on success, false when the line is malformed.
 */
bool ryuw122_parse_anchor_rcv(const char *line, ryuw122_anchor_rcv_t *out);

/** Parse "+TAG_RCV=..."; same payload handling as ryuw122_parse_anchor_rcv(). */
bool ryuw122_parse_tag_rcv(const char *line, ryuw122_tag_rcv_t *out);

/** Return the numeric code of a "+ERR=n" line, or -1 if it is not one. */
int ryuw122_parse_err(const char *line);

/** Human readable description of a "+ERR=n" code. */
const char *ryuw122_err_str(int code);

/**
 * Copy the value of a "<prefix><value>" line (e.g. prefix "+MODE=").
 * @return true when the line starts with prefix and the value fits in out.
 */
bool ryuw122_parse_value(const char *line, const char *prefix, char *out, size_t out_sz);

/** Same as ryuw122_parse_value() but converts the value to an integer. */
bool ryuw122_parse_int(const char *line, const char *prefix, int32_t *out);

#ifdef __cplusplus
}
#endif
