/* SPDX-License-Identifier: MIT */
#include "platform/stm32h743_revd/usb_protocol.h"
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/tx_sched/tx_sm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static usb_command_t parse(const char *s)
{
    usb_command_t c;
    CHECK(usb_protocol_parse(s, strlen(s), &c) == USB_PARSE_OK);
    return c;
}
static void rejected(const char *s, usb_parse_result_t expected)
{
    usb_command_t c, before;
    memset(&c, 0xa5, sizeof c); before = c;
    CHECK(usb_protocol_parse(s, strlen(s), &c) == expected);
    CHECK(memcmp(&c, &before, sizeof c) == 0);
}
static void test_basics(void)
{
    CHECK(parse("HELP").kind == USB_CMD_HELP);
    CHECK(parse("STATUS\r\n").kind == USB_CMD_STATUS);
    CHECK(parse("  \t ").kind == USB_CMD_EMPTY);
    CHECK(parse("STREAM ON").stream);
    CHECK(!parse("STREAM OFF").stream);
    CHECK(parse("\tREC\tSTART ").event.type == EV_REQ_REC_START);
    CHECK(parse("REC STOP").event.b == 0);
    CHECK(parse("REC MARK").event.type == EV_REQ_REC_MARK);
    CHECK(parse("MSC ON").event.a == 1);
    CHECK(parse("MSC ON").event.b == 0);
    CHECK(parse("MSC OFF").event.a == 0);
    CHECK(parse("CH 1 MODE NORMAL").event.a == CAN_MODE_NORMAL);
    CHECK(parse("CH 1 MODE NORMAL").event.b == 0);
    CHECK(parse("CH 2 MODE LISTEN").event.ch == 1);
    CHECK(parse("CH 2 MODE RESTRICTED").event.a == CAN_MODE_RESTRICTED);
    CHECK(parse("CH 2 FD ON").event.type == EV_REQ_CH_FD);
    CHECK(parse("CH 1 FD OFF").event.a == 0);
    CHECK(parse("CH 1 RECOVER").event.type == EV_REQ_CH_RECOVER);
    usb_command_t c = parse("CH 2 RATE 500000 2000000");
    CHECK(c.event.ch == 1 && c.event.a == 500000 && c.event.b == 2000000);
    c = parse("TX 1 SEND 0");
    CHECK(c.event.src == SRC_HOST && c.event.type == EV_REQ_TX_SEND && c.event.b == 0);
    c = parse("TX 2 CYC 3 ON");
    CHECK(c.event.ch == 1 && c.event.a == 3 && c.event.b == 1);
    CHECK(parse("TX 1 CYC 0 OFF").event.b == 0);
    CHECK(parse("TX STOP").event.type == EV_REQ_TX_STOP_ALL);
    CHECK(strstr(usb_protocol_help(), "device confirmation") != NULL);
}
static void test_edit(void)
{
    usb_command_t c = parse("TX SET 1 0 100 123#0102");
    CHECK(c.kind == USB_CMD_TX_SET && c.frame.id == 0x123 && c.frame.ch == 0);
    CHECK(c.frame.len == 2 && c.frame.data[0] == 1 && c.frame.data[1] == 2 && c.period_ms == 100);
    c = parse("TX SET 2 3 0 00000123##1AABB");
    CHECK(c.frame.ch == 1 && (c.frame.flags & (CANF_FD | CANF_EXT | CANF_BRS)) == (CANF_FD | CANF_EXT | CANF_BRS));
    CHECK(c.slot == 3 && c.frame.len == 2 && c.frame.data[1] == 0xbb && c.period_ms == 0);
    CHECK(parse("TX SET 1 0 10 123#").frame.len == 0);
    CHECK(parse("TX SET 1 0 60000 123#R8").frame.len == 8);
    char cmd[256] = "TX SET 2 1 100 1FFFFFFF##1";
    size_t len = strlen(cmd);
    for (unsigned i = 0; i < 64; ++i) { cmd[len++] = 'A'; cmd[len++] = '5'; }
    cmd[len] = 0;
    c = parse(cmd);
    CHECK(c.frame.len == 64 && c.frame.id == 0x1fffffff && c.frame.data[63] == 0xa5);
    rejected("TX SET 1 0 1 123#00", USB_PARSE_RANGE);
    rejected("TX SET 1 0 60001 123#00", USB_PARSE_RANGE);
    rejected("TX SET 1 0 100 800#00", USB_PARSE_FRAME);
    rejected("TX SET 1 0 100 20000000#00", USB_PARSE_FRAME);
    rejected("TX SET 1 0 100 123#ABC", USB_PARSE_FRAME);
    rejected("TX SET 1 0 100 123##1AABBCCDDEEFF001122", USB_PARSE_FRAME);
    rejected("TX SET 1 0 100 123##9AA", USB_PARSE_FRAME);
}
static void test_rejections(void)
{
    static const char *syntax[] = {"help", "HELP MORE", "STATUS CONFIRMED", "REC STOP 1", "CONFIRM", "FORMAT", "MSC ON YES",
        "STREAM MAYBE", "CH 1 MODE ACTIVE", "CH 1 FD 1", "CH 1 RATE 500000", "TX 1 SEND 0 1", "TX 1 CYC 0 ON CONFIRM",
        "REC START\nREC STOP", "HELP A B C D E F G H", "TX SET", "TX SET 1 0 100 123#AA trailing"};
    for (size_t i = 0; i < sizeof syntax / sizeof syntax[0]; ++i) rejected(syntax[i], USB_PARSE_SYNTAX);
    static const char *range[] = {"CH 0 MODE NORMAL", "CH 3 MODE NORMAL", "CH -1 MODE LISTEN", "CH 1 RATE 0 1", "CH 1 RATE 500000 100000",
        "CH 1 RATE 1000001 2000000", "CH 1 RATE 500000 5000001", "CH 1 RATE 4294967296 500000", "CH 1 RATE +500000 2000000",
        "TX 1 SEND 4", "TX 2 SEND -1", "TX 0 SEND 0", "TX SET 1 4 100 123#AA"};
    for (size_t i = 0; i < sizeof range / sizeof range[0]; ++i) rejected(range[i], USB_PARSE_RANGE);
    char over[USB_COMMAND_MAX + 2u]; memset(over, 'A', sizeof over); over[sizeof over - 1u] = 0;
    rejected(over, USB_PARSE_TOO_LONG);
    usb_command_t c;
    char nul[] = {'H','E','L','P',0,'X'};
    CHECK(usb_protocol_parse(nul, sizeof nul, &c) == USB_PARSE_SYNTAX);
    const char raw[] = {'H','E','L','P'};
    CHECK(usb_protocol_parse(raw, sizeof raw, &c) == USB_PARSE_OK && c.kind == USB_CMD_HELP);
    CHECK(usb_protocol_parse(NULL, 0, &c) == USB_PARSE_SYNTAX);
    CHECK(usb_protocol_parse("HELP", 4, NULL) == USB_PARSE_SYNTAX);
}
static void test_confirmation_events(void)
{
    /* Every host action is unconfirmed. A client cannot smuggle a device answer
     * through integer overflow, an extra token, or the cyclic bit-field. */
    static const char *commands[] = {"REC STOP", "CH 1 MODE NORMAL", "TX 2 SEND 1", "MSC ON"};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        usb_command_t c = parse(commands[i]);
        CHECK(c.event.src == SRC_HOST && c.event.b == 0);
    }
    CHECK((parse("TX 1 CYC 1 ON").event.b & 2) == 0);
    /* Parser remains pure: it neither consumes nor posts an application event. */
    evt_init();
    (void)parse("TX 1 SEND 0");
    app_evt_t ev;
    CHECK(!evt_pop(&ev));
}
static void test_bench_parser(void)
{
    usb_command_t c = parse("BENCH CONFIG 64 250 50");
    CHECK(c.kind == USB_CMD_BENCH && c.bench_action == BENCH_CONFIG);
    CHECK(c.bench_config.segment_kib == 64 && c.bench_config.rotate_ms == 250 && c.bench_config.refresh_ms == 50);
    c = parse("BENCH CONFIG 65536 3600000 60000");
    CHECK(c.bench_config.segment_kib == 65536 && c.bench_config.rotate_ms == 3600000 && c.bench_config.refresh_ms == 60000);
    c = parse("BENCH CONFIG 1024 0 0");
    CHECK(c.bench_config.rotate_ms == 0 && c.bench_config.refresh_ms == 0);
    CHECK(parse("BENCH ON").bench_action == BENCH_ON);
    CHECK(parse("BENCH OFF").bench_action == BENCH_OFF);
    CHECK(parse("BENCH ROTATE").bench_action == BENCH_ROTATE);
    static const char *range[] = {
        "BENCH CONFIG 63 250 50", "BENCH CONFIG 65537 250 50",
        "BENCH CONFIG 64 1 50", "BENCH CONFIG 64 249 50", "BENCH CONFIG 64 3600001 50",
        "BENCH CONFIG 64 250 1", "BENCH CONFIG 64 250 49", "BENCH CONFIG 64 250 60001",
        "BENCH CONFIG -64 250 50", "BENCH CONFIG 64 4294967296 50"};
    for (size_t i = 0; i < sizeof range / sizeof range[0]; i++) rejected(range[i], USB_PARSE_RANGE);
    static const char *syntax[] = {"BENCH", "BENCH CONFIG", "BENCH ON CONFIRMED", "BENCH ROTATE 1", "BENCH CONFIG 64 250 50 YES"};
    for (size_t i = 0; i < sizeof syntax / sizeof syntax[0]; i++) rejected(syntax[i], USB_PARSE_SYNTAX);
    evt_init();
    c = parse("BENCH ON");
    CHECK(c.event.b == 0 && c.event.src == SRC_HOST);
    app_evt_t ev;
    CHECK(!evt_pop(&ev));
}
int main(void)
{
    test_basics(); test_edit(); test_rejections(); test_confirmation_events(); test_bench_parser();
    printf("USB protocol: %u checks passed\n", checks);
    return 0;
}
