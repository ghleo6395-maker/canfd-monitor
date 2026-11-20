/* SPDX-License-Identifier: MIT
 * Bounded ASCII protocol. Parsing never mutates an application service. */
#ifndef REVD_USB_PROTOCOL_H
#define REVD_USB_PROTOCOL_H
#include "services/core/app_types.h"
#include "services/bench/bench.h"

#define USB_COMMAND_MAX 255u

typedef enum {
    USB_CMD_EMPTY, USB_CMD_HELP, USB_CMD_STATUS, USB_CMD_EVENT,
    USB_CMD_TX_SET, USB_CMD_STREAM, USB_CMD_BENCH
} usb_command_kind_t;
typedef enum {
    USB_PARSE_OK, USB_PARSE_TOO_LONG, USB_PARSE_SYNTAX, USB_PARSE_RANGE,
    USB_PARSE_FRAME
} usb_parse_result_t;
typedef struct {
    usb_command_kind_t kind;
    app_evt_t event;
    can_frame_t frame;
    uint32_t period_ms;
    uint8_t slot;
    bool stream;
    bench_action_t bench_action;
    bench_config_t bench_config;
} usb_command_t;

/* Input may omit NUL; maximum is USB_COMMAND_MAX bytes excluding CR/LF.
 * Case-sensitive. Error leaves *out untouched. No confirmation command exists. */
usb_parse_result_t usb_protocol_parse(const char *line, size_t len, usb_command_t *out);
const char *usb_protocol_error(usb_parse_result_t result);
const char *usb_protocol_help(void);
#endif
