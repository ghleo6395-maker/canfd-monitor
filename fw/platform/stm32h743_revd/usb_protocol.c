/* SPDX-License-Identifier: MIT */
#include "usb_protocol.h"
#include "services/logfmt/candump.h"
#include "services/tx_sched/tx_sm.h"
#include <stdio.h>
#include <string.h>

#define TOKENS_MAX 8u
static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static bool decimal(const char *s, uint32_t low, uint32_t high, uint32_t *out)
{
    uint32_t v = 0;
    if (*s == '\0') return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        uint32_t digit = (uint32_t)(*s - '0');
        if (v > (UINT32_MAX - digit) / 10u) return false;
        v = v * 10u + digit;
    }
    if (v < low || v > high) return false;
    *out = v;
    return true;
}
static bool onoff(const char *s, bool *v)
{
    if (eq(s, "ON")) { *v = true; return true; }
    if (eq(s, "OFF")) { *v = false; return true; }
    return false;
}
static bool channel(const char *s, uint8_t *ch)
{
    uint32_t n;
    if (!decimal(s, 1, CH_NUM, &n)) return false;
    *ch = (uint8_t)(n - 1u);
    return true;
}
static bool slot(const char *s, uint8_t *idx)
{
    uint32_t n;
    if (!decimal(s, 0, TX_ENTRIES - 1u, &n)) return false;
    *idx = (uint8_t)n;
    return true;
}
static usb_parse_result_t parse_tokens(char **t, size_t n, usb_command_t *c)
{
    c->event.src = SRC_HOST;
    if (n == 0) { c->kind = USB_CMD_EMPTY; return USB_PARSE_OK; }
    if (n == 1 && eq(t[0], "HELP")) { c->kind = USB_CMD_HELP; return USB_PARSE_OK; }
    if (n == 1 && eq(t[0], "STATUS")) { c->kind = USB_CMD_STATUS; return USB_PARSE_OK; }
    if (eq(t[0], "BENCH")) {
        c->kind = USB_CMD_BENCH;
        if (n == 2) {
            if (eq(t[1], "ON")) c->bench_action = BENCH_ON;
            else if (eq(t[1], "OFF")) c->bench_action = BENCH_OFF;
            else if (eq(t[1], "ROTATE")) c->bench_action = BENCH_ROTATE;
            else return USB_PARSE_SYNTAX;
            return USB_PARSE_OK;
        }
        if (n != 5 || !eq(t[1], "CONFIG")) return USB_PARSE_SYNTAX;
        bench_config_t *b = &c->bench_config;
        if (!decimal(t[2], 64u, 65536u, &b->segment_kib) ||
            !decimal(t[3], 0u, 3600000u, &b->rotate_ms) ||
            !decimal(t[4], 0u, 60000u, &b->refresh_ms) ||
            (b->rotate_ms && b->rotate_ms < 250u) ||
            (b->refresh_ms && b->refresh_ms < 50u)) return USB_PARSE_RANGE;
        c->bench_action = BENCH_CONFIG;
        return USB_PARSE_OK;
    }
    if (n == 2 && eq(t[0], "STREAM")) {
        if (!onoff(t[1], &c->stream)) return USB_PARSE_SYNTAX;
        c->kind = USB_CMD_STREAM;
        return USB_PARSE_OK;
    }
    c->kind = USB_CMD_EVENT;
    if (n == 2 && eq(t[0], "REC")) {
        if (eq(t[1], "START")) c->event.type = EV_REQ_REC_START;
        else if (eq(t[1], "STOP")) c->event.type = EV_REQ_REC_STOP;
        else if (eq(t[1], "MARK")) c->event.type = EV_REQ_REC_MARK;
        else return USB_PARSE_SYNTAX;
        return USB_PARSE_OK;
    }
    if (n == 2 && eq(t[0], "MSC")) {
        bool enabled;
        if (!onoff(t[1], &enabled)) return USB_PARSE_SYNTAX;
        c->event.type = EV_REQ_MSC; c->event.a = enabled;
        return USB_PARSE_OK;
    }
    if (n >= 3 && eq(t[0], "CH")) {
        if (!channel(t[1], &c->event.ch)) return USB_PARSE_RANGE;
        if (n == 4 && eq(t[2], "MODE")) {
            c->event.type = EV_REQ_CH_MODE;
            if (eq(t[3], "LISTEN")) c->event.a = CAN_MODE_LISTEN;
            else if (eq(t[3], "RESTRICTED")) c->event.a = CAN_MODE_RESTRICTED;
            else if (eq(t[3], "NORMAL")) c->event.a = CAN_MODE_NORMAL;
            else return USB_PARSE_SYNTAX;
            return USB_PARSE_OK;
        }
        if (n == 5 && eq(t[2], "RATE")) {
            uint32_t nbr, dbr;
            if (!decimal(t[3], 10000u, 1000000u, &nbr) ||
                !decimal(t[4], 10000u, 5000000u, &dbr) || dbr < nbr)
                return USB_PARSE_RANGE;
            c->event.type = EV_REQ_CH_RATE;
            c->event.a = (int32_t)nbr; c->event.b = (int32_t)dbr;
            return USB_PARSE_OK;
        }
        if (n == 4 && eq(t[2], "FD")) {
            bool enabled;
            if (!onoff(t[3], &enabled)) return USB_PARSE_SYNTAX;
            c->event.type = EV_REQ_CH_FD; c->event.a = enabled;
            return USB_PARSE_OK;
        }
        if (n == 3 && eq(t[2], "RECOVER")) {
            c->event.type = EV_REQ_CH_RECOVER;
            return USB_PARSE_OK;
        }
        return USB_PARSE_SYNTAX;
    }
    if (eq(t[0], "TX")) {
        if (n == 2 && eq(t[1], "STOP")) { c->event.type = EV_REQ_TX_STOP_ALL; return USB_PARSE_OK; }
        if (n >= 2 && eq(t[1], "SET")) {
            if (n != 6) return USB_PARSE_SYNTAX;
            uint8_t ch;
            if (!channel(t[2], &ch) || !slot(t[3], &c->slot) ||
                !decimal(t[4], 0u, 60000u, &c->period_ms)) return USB_PARSE_RANGE;
            if (c->period_ms != 0u && c->period_ms < 10u) return USB_PARSE_RANGE;
            char sample[USB_COMMAND_MAX + 1u];
            int len = snprintf(sample, sizeof sample, "(0.000000) can%u %s", ch, t[5]);
            if (len < 0 || (size_t)len >= sizeof sample ||
                logfmt_parse_line(sample, (size_t)len, &c->frame) != LOGFMT_OK ||
                (c->frame.flags & (CANF_TX | CANF_ERR)) != 0u) return USB_PARSE_FRAME;
            c->kind = USB_CMD_TX_SET;
            return USB_PARSE_OK;
        }
        if (n >= 4) {
            if (!channel(t[1], &c->event.ch) || !slot(t[3], &c->slot)) return USB_PARSE_RANGE;
            c->event.a = c->slot;
            if (n == 4 && eq(t[2], "SEND")) {
                c->event.type = EV_REQ_TX_SEND;
                return USB_PARSE_OK;
            }
            if (n == 5 && eq(t[2], "CYC")) {
                bool enabled;
                if (!onoff(t[4], &enabled)) return USB_PARSE_SYNTAX;
                c->event.type = EV_REQ_TX_CYC;
                /* bit 1 is device confirmation, intentionally always clear. */
                c->event.b = enabled ? 1 : 0;
                return USB_PARSE_OK;
            }
        }
    }
    return USB_PARSE_SYNTAX;
}

usb_parse_result_t usb_protocol_parse(const char *line, size_t len, usb_command_t *out)
{
    if (!line || !out) return USB_PARSE_SYNTAX;
    while (len && (line[len - 1u] == '\r' || line[len - 1u] == '\n')) --len;
    if (len > USB_COMMAND_MAX) return USB_PARSE_TOO_LONG;
    char buf[USB_COMMAND_MAX + 1u];
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 32u && c != '\t') || c > 126u) return USB_PARSE_SYNTAX;
        buf[i] = (char)c;
    }
    buf[len] = '\0';
    char *tokens[TOKENS_MAX];
    size_t n = 0, i = 0;
    while (i < len) {
        while (i < len && (buf[i] == ' ' || buf[i] == '\t')) ++i;
        if (i == len) break;
        if (n == TOKENS_MAX) return USB_PARSE_SYNTAX;
        tokens[n++] = &buf[i];
        while (i < len && buf[i] != ' ' && buf[i] != '\t') ++i;
        if (i < len) buf[i++] = '\0';
    }
    usb_command_t command = {0};
    usb_parse_result_t result = parse_tokens(tokens, n, &command);
    if (result == USB_PARSE_OK) *out = command;
    return result;
}

const char *usb_protocol_error(usb_parse_result_t result)
{
    switch (result) {
    case USB_PARSE_OK: return "OK";
    case USB_PARSE_TOO_LONG: return "LINE_TOO_LONG";
    case USB_PARSE_RANGE: return "RANGE";
    case USB_PARSE_FRAME: return "FRAME";
    default: return "SYNTAX";
    }
}
const char *usb_protocol_help(void)
{
    return "CANFD-REVD CDC 1\r\n"
           "HELP | STATUS | STREAM ON|OFF\r\n"
           "REC START|STOP|MARK | MSC ON|OFF\r\n"
           "BENCH CONFIG <segment_KiB64..65536> <rotate_ms0|250..3600000> <refresh_ms0|50..60000>\r\n"
           "BENCH ON|OFF|ROTATE (explicit bench only; no transmit or record auto-start)\r\n"
           "CH <1|2> MODE LISTEN|RESTRICTED|NORMAL\r\n"
           "CH <1|2> RATE <nominal_bps> <data_bps>\r\n"
           "CH <1|2> FD ON|OFF | CH <1|2> RECOVER\r\n"
           "TX SET <ch> <slot> <period_ms> <ID#DATA|ID##flagsDATA>\r\n"
           "TX <ch> SEND <slot> | TX <ch> CYC <slot> ON|OFF | TX STOP\r\n"
           "Channels 1..2; slots 0..3; period 0 or 10..60000 ms.\r\n"
           "OK QUEUED means request queued; use STATUS for outcome.\r\n"
           "Transmit mode/first send, recorder stop and MSC use device confirmation.\r\n";
}
