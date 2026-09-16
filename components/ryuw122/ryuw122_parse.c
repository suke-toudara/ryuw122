/*
 * RYUW122 UWB module - protocol parsing layer (pure C, no ESP-IDF dependency).
 */
#include "ryuw122_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

size_t ryuw122_trim(char *s)
{
    if (s == NULL) {
        return 0;
    }
    char *start = s;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) {
        len--;
    }
    if (start != s) {
        memmove(s, start, len);
    }
    s[len] = '\0';
    return len;
}

ryuw122_line_kind_t ryuw122_classify_line(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return RYUW122_LINE_EMPTY;
    }
    if (strncmp(line, "+ANCHOR_RCV=", 12) == 0) {
        return RYUW122_LINE_ANCHOR_RCV;
    }
    if (strncmp(line, "+TAG_RCV=", 9) == 0) {
        return RYUW122_LINE_TAG_RCV;
    }
    if (strncmp(line, "+ERR=", 5) == 0) {
        return RYUW122_LINE_ERR;
    }
    if (strncmp(line, "+OK", 3) == 0) {
        return RYUW122_LINE_OK;
    }
    if (line[0] == '+') {
        return RYUW122_LINE_RESPONSE;
    }
    return RYUW122_LINE_UNKNOWN;
}

int ryuw122_parse_err(const char *line)
{
    if (line == NULL || strncmp(line, "+ERR=", 5) != 0) {
        return -1;
    }
    char *end = NULL;
    long code = strtol(line + 5, &end, 10);
    if (end == line + 5 || code < 0 || code > 255) {
        return -1;
    }
    return (int)code;
}

const char *ryuw122_err_str(int code)
{
    switch (code) {
    case RYUW122_ERR_NO_CRLF:      return "missing <CR><LF>";
    case RYUW122_ERR_BAD_HEADER:   return "command does not start with AT";
    case RYUW122_ERR_BAD_PARAM:    return "parameter failure";
    case RYUW122_ERR_CMD_FAILURE:  return "command failure";
    case RYUW122_ERR_UNKNOWN_CMD:  return "unknown command";
    default:                       return "unknown error code";
    }
}

bool ryuw122_parse_value(const char *line, const char *prefix, char *out, size_t out_sz)
{
    if (line == NULL || prefix == NULL || out == NULL || out_sz == 0) {
        return false;
    }
    size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0) {
        return false;
    }
    const char *value = line + plen;
    size_t vlen = strlen(value);
    if (vlen >= out_sz) {
        return false;
    }
    memcpy(out, value, vlen);
    out[vlen] = '\0';
    return true;
}

bool ryuw122_parse_int(const char *line, const char *prefix, int32_t *out)
{
    char buf[32];
    if (out == NULL || !ryuw122_parse_value(line, prefix, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    long value = strtol(buf, &end, 10);
    if (end == buf) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

/*
 * Copy the characters up to the next ',' (or end of string) into out.
 * Returns the position of the separator, or NULL when the field does not fit.
 * *had_comma tells the caller whether more fields follow.
 */
static const char *take_field(const char *p, char *out, size_t out_sz, bool *had_comma)
{
    const char *comma = strchr(p, ',');
    size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
    if (len >= out_sz) {
        return NULL;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    *had_comma = (comma != NULL);
    return (comma != NULL) ? comma + 1 : p + len;
}

static bool parse_int_field(const char *s, int32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

/*
 * Common tail of both RCV notifications:
 *   <payload length>,<data>,<distance>,<rssi>   (anchor, distance present)
 *   <payload length>,<data>,<rssi>              (tag, no distance)
 *
 * The payload may contain ',' characters, so the declared length is used to
 * slice it out. If the declared length does not line up with the remaining
 * text, we fall back to splitting on the first comma so that a module with a
 * slightly different payload accounting still yields usable data.
 */
static bool parse_payload(const char *p, uint8_t *len_out, char *data, size_t data_sz,
                          const char **rest_out)
{
    char field[16];
    bool had_comma = false;
    const char *rest = take_field(p, field, sizeof(field), &had_comma);
    if (rest == NULL || !had_comma) {
        return false;
    }
    int32_t declared = 0;
    if (!parse_int_field(field, &declared) || declared < 0 || (size_t)declared >= data_sz) {
        return false;
    }

    size_t remaining = strlen(rest);
    if ((size_t)declared <= remaining &&
        (rest[declared] == ',' || rest[declared] == '\0')) {
        memcpy(data, rest, (size_t)declared);
        data[declared] = '\0';
        *len_out = (uint8_t)declared;
        rest += declared;
        *rest_out = (*rest == ',') ? rest + 1 : rest;
        return true;
    }

    /* Fallback: trust the comma layout rather than the declared length. */
    const char *after = take_field(rest, data, data_sz, &had_comma);
    if (after == NULL) {
        return false;
    }
    *len_out = (uint8_t)strlen(data);
    *rest_out = after;
    return true;
}

static void parse_optional_rssi(const char *p, int32_t *rssi, bool *valid)
{
    *rssi = 0;
    *valid = false;
    if (p == NULL) {
        return;
    }
    char field[16];
    bool had_comma = false;
    if (take_field(p, field, sizeof(field), &had_comma) == NULL) {
        return;
    }
    int32_t value = 0;
    if (parse_int_field(field, &value)) {
        *rssi = value;
        *valid = true;
    }
}

bool ryuw122_parse_anchor_rcv(const char *line, ryuw122_anchor_rcv_t *out)
{
    if (line == NULL || out == NULL || strncmp(line, "+ANCHOR_RCV=", 12) != 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *p = line + 12;
    bool had_comma = false;
    p = take_field(p, out->addr, sizeof(out->addr), &had_comma);
    if (p == NULL || !had_comma || out->addr[0] == '\0') {
        return false;
    }

    const char *rest = NULL;
    if (!parse_payload(p, &out->payload_len, out->data, sizeof(out->data), &rest)) {
        return false;
    }

    char field[16];
    const char *after = take_field(rest, field, sizeof(field), &had_comma);
    if (after == NULL || !parse_int_field(field, &out->distance_cm)) {
        return false;
    }
    if (had_comma) {
        parse_optional_rssi(after, &out->rssi_dbm, &out->rssi_valid);
    }
    return true;
}

bool ryuw122_parse_tag_rcv(const char *line, ryuw122_tag_rcv_t *out)
{
    if (line == NULL || out == NULL || strncmp(line, "+TAG_RCV=", 9) != 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *rest = NULL;
    if (!parse_payload(line + 9, &out->payload_len, out->data, sizeof(out->data), &rest)) {
        return false;
    }
    if (rest != NULL && *rest != '\0') {
        parse_optional_rssi(rest, &out->rssi_dbm, &out->rssi_valid);
    }
    return true;
}
