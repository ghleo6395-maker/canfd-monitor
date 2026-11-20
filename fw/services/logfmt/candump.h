/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LOGFMT_CANDUMP_H
#define LOGFMT_CANDUMP_H

#include "services/core/app_types.h"

#define LOGFMT_MAX_LINE 256u
#define LOGFMT_MAX_CHANNELS 16u

typedef enum {
    LOGFMT_OK = 0, LOGFMT_SKIP = 1,
    LOGFMT_ERR_SYNTAX = -1, LOGFMT_ERR_RANGE = -2,
    LOGFMT_ERR_LENGTH = -3, LOGFMT_ERR_SPACE = -4
} logfmt_result_t;

/* Bounded candump -L subset: (seconds.microseconds) canN ID#DATA / ID##flagsDATA.
 * can0 -> CH1/CAN1, can1 -> CH2/CAN2; can2..15 can be imported/exported but
 * never silently remapped. Whitespace and comment-only (#) lines are skipped.
 * Three/eight hex ID digits preserve standard/extended identity. Error records
 * use SocketCAN's 0x20000000 mask. No CAN XL or Classic len8_dlc extension.
 * Output untouched on parse error. All FD lengths must be representable DLCs. */
logfmt_result_t logfmt_parse_line(const char *line, size_t len, can_frame_t *out);
/* Canonical output includes newline and NUL. Returns bytes excluding NUL, or a
 * negative result. No partial frame is exposed on insufficient output space. */
int logfmt_format_line(const can_frame_t *frame, char *buf, size_t cap);
/* Exact canonical text bytes including newline, excluding NUL; no formatting
 * buffer or SD access. Used for recording segment accounting. */
int logfmt_line_size(const can_frame_t *frame);
bool logfmt_frame_valid(const can_frame_t *frame);
const char *logfmt_result_name(logfmt_result_t result);

#endif
