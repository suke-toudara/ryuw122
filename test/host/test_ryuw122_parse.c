/*
 * Host-side unit tests for the RYUW122 protocol parsing layer.
 * Build & run:  ./test/host/run_tests.sh
 */
#include "ryuw122_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_checks;

#define CHECK(cond)                                                           \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int test_classify(void)
{
    CHECK(ryuw122_classify_line("+OK") == RYUW122_LINE_OK);
    CHECK(ryuw122_classify_line("+ERR=3") == RYUW122_LINE_ERR);
    CHECK(ryuw122_classify_line("+MODE=1") == RYUW122_LINE_RESPONSE);
    CHECK(ryuw122_classify_line("+ANCHOR_RCV=T1T1T1T1,4,ABCD,123,-78") == RYUW122_LINE_ANCHOR_RCV);
    CHECK(ryuw122_classify_line("+TAG_RCV=4,ABCD,-78") == RYUW122_LINE_TAG_RCV);
    CHECK(ryuw122_classify_line("") == RYUW122_LINE_EMPTY);
    CHECK(ryuw122_classify_line(NULL) == RYUW122_LINE_EMPTY);
    CHECK(ryuw122_classify_line("garbage") == RYUW122_LINE_UNKNOWN);
    return 0;
}

static int test_trim(void)
{
    char buf[32];
    strcpy(buf, "  +OK\r\n");
    CHECK(ryuw122_trim(buf) == 3);
    CHECK(strcmp(buf, "+OK") == 0);

    strcpy(buf, "\r\n");
    CHECK(ryuw122_trim(buf) == 0);
    CHECK(buf[0] == '\0');
    return 0;
}

static int test_err(void)
{
    CHECK(ryuw122_parse_err("+ERR=5") == 5);
    CHECK(ryuw122_parse_err("+ERR=") == -1);
    CHECK(ryuw122_parse_err("+OK") == -1);
    CHECK(strcmp(ryuw122_err_str(3), "parameter failure") == 0);
    return 0;
}

static int test_values(void)
{
    char buf[32];
    int32_t value = 0;

    CHECK(ryuw122_parse_value("+ADDRESS=ANCHOR01", "+ADDRESS=", buf, sizeof(buf)));
    CHECK(strcmp(buf, "ANCHOR01") == 0);
    CHECK(!ryuw122_parse_value("+ADDRESS=ANCHOR01", "+MODE=", buf, sizeof(buf)));
    /* value longer than the destination buffer must be rejected, not truncated */
    CHECK(!ryuw122_parse_value("+ADDRESS=ANCHOR01", "+ADDRESS=", buf, 4));

    CHECK(ryuw122_parse_int("+MODE=1", "+MODE=", &value) && value == 1);
    CHECK(ryuw122_parse_int("+CAL=-16", "+CAL=", &value) && value == -16);
    CHECK(!ryuw122_parse_int("+MODE=", "+MODE=", &value));
    return 0;
}

static int test_anchor_rcv(void)
{
    ryuw122_anchor_rcv_t rcv;

    CHECK(ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,4,ABCD,123,-78", &rcv));
    CHECK(strcmp(rcv.addr, "T1T1T1T1") == 0);
    CHECK(rcv.payload_len == 4);
    CHECK(strcmp(rcv.data, "ABCD") == 0);
    CHECK(rcv.distance_cm == 123);
    CHECK(rcv.rssi_valid && rcv.rssi_dbm == -78);

    /* RSSI reporting disabled (AT+RSSI=0) */
    CHECK(ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,4,ABCD,123", &rcv));
    CHECK(rcv.distance_cm == 123);
    CHECK(!rcv.rssi_valid);

    /* empty payload */
    CHECK(ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,0,,250,-80", &rcv));
    CHECK(rcv.payload_len == 0);
    CHECK(rcv.data[0] == '\0');
    CHECK(rcv.distance_cm == 250);
    CHECK(rcv.rssi_dbm == -80);

    /* payload containing commas must be sliced using the declared length */
    CHECK(ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,5,A,B,C,300,-70", &rcv));
    CHECK(rcv.payload_len == 5);
    CHECK(strcmp(rcv.data, "A,B,C") == 0);
    CHECK(rcv.distance_cm == 300);
    CHECK(rcv.rssi_dbm == -70);

    /* maximum payload length */
    CHECK(ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,12,123456789012,1,-1", &rcv));
    CHECK(rcv.payload_len == 12);
    CHECK(strcmp(rcv.data, "123456789012") == 0);

    /* malformed input must be rejected, not silently accepted */
    CHECK(!ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1", &rcv));
    CHECK(!ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,4,ABCD", &rcv));
    CHECK(!ryuw122_parse_anchor_rcv("+ANCHOR_RCV=,4,ABCD,10,-70", &rcv));
    CHECK(!ryuw122_parse_anchor_rcv("+ANCHOR_RCV=T1T1T1T1,99,ABCD,10,-70", &rcv));
    CHECK(!ryuw122_parse_anchor_rcv("+TAG_RCV=4,ABCD,-78", &rcv));
    CHECK(!ryuw122_parse_anchor_rcv(NULL, &rcv));
    return 0;
}

static int test_tag_rcv(void)
{
    ryuw122_tag_rcv_t rcv;

    CHECK(ryuw122_parse_tag_rcv("+TAG_RCV=4,ABCD,-78", &rcv));
    CHECK(rcv.payload_len == 4);
    CHECK(strcmp(rcv.data, "ABCD") == 0);
    CHECK(rcv.rssi_valid && rcv.rssi_dbm == -78);

    CHECK(ryuw122_parse_tag_rcv("+TAG_RCV=4,ABCD", &rcv));
    CHECK(!rcv.rssi_valid);

    CHECK(ryuw122_parse_tag_rcv("+TAG_RCV=0,,-90", &rcv));
    CHECK(rcv.payload_len == 0 && rcv.data[0] == '\0');
    CHECK(rcv.rssi_dbm == -90);

    CHECK(!ryuw122_parse_tag_rcv("+TAG_RCV=", &rcv));
    CHECK(!ryuw122_parse_tag_rcv("+ANCHOR_RCV=T1T1T1T1,4,ABCD,123,-78", &rcv));
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        {"classify", test_classify},
        {"trim", test_trim},
        {"err", test_err},
        {"values", test_values},
        {"anchor_rcv", test_anchor_rcv},
        {"tag_rcv", test_tag_rcv},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i].fn() != 0) {
            fprintf(stderr, "test '%s' failed\n", tests[i].name);
            return 1;
        }
        printf("ok - %s\n", tests[i].name);
    }
    printf("all tests passed (%d checks)\n", g_checks);
    return 0;
}
