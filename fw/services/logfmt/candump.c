/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * lib.c - library for command line tools
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <linux-can@vger.kernel.org>
 *
 */

/* Portable Classical CAN / CAN FD adaptation of asc2nibble(), parse_canframe(),
 * snprintf_canframe() and DLC mapping from linux-can/can-utils lib.c.
 * Upstream: https://github.com/linux-can/can-utils/blob/834ceef1f1eabec67625389172eebf1050d2f42b/lib.c
 * This adaptation chooses the BSD-3-Clause option. Linux structures, sockets,
 * CAN XL and len8_dlc are omitted. Timestamp/channel parsing, strict bounds,
 * full-consumption checks and legal DLC validation were added for this project.
 */
#include "services/logfmt/candump.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char hex_upper[] = "0123456789ABCDEF";
static const uint8_t dlc2len[] = { 0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64 };

/* Adapted from can-utils asc2nibble(). */
static unsigned nibble(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return 16;
}

static bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool logfmt_frame_valid(const can_frame_t *f)
{
    if (!f || f->ch >= LOGFMT_MAX_CHANNELS ||
        (f->flags & ~(CANF_EXT | CANF_FD | CANF_BRS | CANF_ESI | CANF_RTR | CANF_TX | CANF_ERR)))
        return false;
    if (f->id > ((f->flags & (CANF_EXT | CANF_ERR)) ? 0x1FFFFFFFu : 0x7FFu)) return false;
    if (f->flags & CANF_ERR)
        return f->len <= 8 && !(f->flags & (CANF_EXT | CANF_FD | CANF_RTR | CANF_BRS | CANF_ESI));
    if (f->flags & CANF_FD) {
        if (f->flags & CANF_RTR) return false;
        for (size_t i = 0; i < sizeof dlc2len; i++)
            if (f->len == dlc2len[i]) return true;
        return false;
    }
    return f->len <= 8 && !(f->flags & (CANF_BRS | CANF_ESI));
}

logfmt_result_t logfmt_parse_line(const char *line, size_t len, can_frame_t *out)
{
    if (!line || !out) return LOGFMT_ERR_SYNTAX;
    if (len > LOGFMT_MAX_LINE) return LOGFMT_ERR_LENGTH;
    size_t pos = 0;
    while (pos < len && space(line[pos])) pos++;
    if (pos == len || line[pos] == '#') return LOGFMT_SKIP;
    can_frame_t f = {0};
    if (line[pos++] != '(') return LOGFMT_ERR_SYNTAX;
    uint64_t seconds = 0;
    size_t digits = 0;
    while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
        unsigned d = (unsigned)(line[pos++] - '0');
        if (seconds > (UINT64_MAX - d) / 10u) return LOGFMT_ERR_RANGE;
        seconds = seconds * 10u + d;
        digits++;
    }
    if (!digits || pos >= len || line[pos++] != '.') return LOGFMT_ERR_SYNTAX;
    uint32_t fraction = 0;
    digits = 0;
    while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
        if (digits == 6) return LOGFMT_ERR_RANGE;
        fraction = fraction * 10u + (unsigned)(line[pos++] - '0');
        digits++;
    }
    if (!digits || pos >= len || line[pos++] != ')') return LOGFMT_ERR_SYNTAX;
    while (digits++ < 6) fraction *= 10u;
    if (seconds > (UINT64_MAX - fraction) / 1000000u) return LOGFMT_ERR_RANGE;
    f.ts_us = seconds * 1000000u + fraction;
    if (pos >= len || !space(line[pos])) return LOGFMT_ERR_SYNTAX;
    while (pos < len && space(line[pos])) pos++;
    if (len - pos < 4 || memcmp(line + pos, "can", 3)) return LOGFMT_ERR_SYNTAX;
    pos += 3;
    unsigned ch = 0;
    digits = 0;
    while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
        if (++digits > 2) return LOGFMT_ERR_RANGE;
        ch = ch * 10u + (unsigned)(line[pos++] - '0');
    }
    if (!digits || ch >= LOGFMT_MAX_CHANNELS) return LOGFMT_ERR_RANGE;
    f.ch = (uint8_t)ch;
    if (pos >= len || !space(line[pos])) return LOGFMT_ERR_SYNTAX;
    while (pos < len && space(line[pos])) pos++;
    size_t id_start = pos;
    while (pos < len && nibble(line[pos]) < 16) pos++;
    digits = pos - id_start;
    if ((digits != 3 && digits != 8) || pos >= len || line[pos++] != '#') return LOGFMT_ERR_SYNTAX;
    /* Adapted from can-utils parse_canframe() SFF/EFF decode. */
    for (size_t i = 0; i < digits; i++)
        f.id |= (uint32_t)nibble(line[id_start + i]) << (digits - 1u - i) * 4u;
    if (digits == 8) {
        if (f.id & 0x20000000u) {
            if (f.id & 0xC0000000u) return LOGFMT_ERR_RANGE;
            f.flags |= CANF_ERR;
            f.id &= 0x1FFFFFFFu;
        } else {
            if (f.id > 0x1FFFFFFFu) return LOGFMT_ERR_RANGE;
            f.flags |= CANF_EXT;
        }
    } else if (f.id > 0x7FFu) return LOGFMT_ERR_RANGE;
    if (pos < len && (line[pos] == 'R' || line[pos] == 'r')) {
        f.flags |= CANF_RTR;
        pos++;
        if (pos < len && !space(line[pos])) {
            unsigned dlc = nibble(line[pos++]);
            if (dlc > 8) return LOGFMT_ERR_LENGTH;
            f.len = (uint8_t)dlc;
        }
    } else {
        if (pos < len && line[pos] == '#') {
            pos++;
            if (pos >= len) return LOGFMT_ERR_SYNTAX;
            unsigned flags = nibble(line[pos++]);
            /* SocketCAN's FDF marker bit (4) is redundant but accepted. */
            if (flags > 7) return LOGFMT_ERR_RANGE;
            f.flags |= CANF_FD;
            if (flags & 1) f.flags |= CANF_BRS;
            if (flags & 2) f.flags |= CANF_ESI;
        }
        unsigned maxlen = (f.flags & CANF_FD) ? 64u : 8u;
        while (pos < len && !space(line[pos])) {
            if (line[pos] == '.') {
                pos++;
                if (pos == len || space(line[pos])) return LOGFMT_ERR_SYNTAX;
            }
            if (f.len == maxlen) return LOGFMT_ERR_LENGTH;
            if (len - pos < 2) return LOGFMT_ERR_SYNTAX;
            unsigned hi = nibble(line[pos++]);
            unsigned lo = nibble(line[pos++]);
            if (hi > 15 || lo > 15) return LOGFMT_ERR_SYNTAX;
            f.data[f.len++] = (uint8_t)((hi << 4) | lo);
        }
    }
    while (pos < len && space(line[pos])) pos++;
    if (pos != len) return LOGFMT_ERR_SYNTAX;
    if (!logfmt_frame_valid(&f)) return LOGFMT_ERR_LENGTH;
    *out = f;
    return LOGFMT_OK;
}

int logfmt_line_size(const can_frame_t *f)
{
    if (!logfmt_frame_valid(f)) return LOGFMT_ERR_RANGE;
    unsigned digits = 1;
    for (uint64_t seconds = f->ts_us / 1000000u; seconds >= 10u; seconds /= 10u) ++digits;
    unsigned prefix = 14u + digits + (f->ch >= 10u ? 2u : 1u);
    unsigned id = (f->flags & (CANF_EXT | CANF_ERR)) ? 8u : 3u;
    unsigned payload = (f->flags & CANF_RTR) ? 1u + (f->len != 0u) :
        2u * f->len + ((f->flags & CANF_FD) ? 2u : 0u);
    return (int)(prefix + id + 1u + payload + 1u);
}

int logfmt_format_line(const can_frame_t *f, char *buf, size_t cap)
{
    if (!buf || !cap) return LOGFMT_ERR_SPACE;
    buf[0] = 0;
    if (!logfmt_frame_valid(f)) return LOGFMT_ERR_RANGE;
    char tmp[LOGFMT_MAX_LINE + 1u];
    int n = snprintf(tmp, sizeof tmp, "(%" PRIu64 ".%06" PRIu64 ") can%u ",
                     f->ts_us / 1000000u, f->ts_us % 1000000u, (unsigned)f->ch);
    if (n < 0 || (size_t)n >= sizeof tmp) return LOGFMT_ERR_SPACE;
    size_t pos = (size_t)n;
    unsigned width = (f->flags & (CANF_EXT | CANF_ERR)) ? 8u : 3u;
    uint32_t id = f->id | ((f->flags & CANF_ERR) ? 0x20000000u : 0u);
    /* Adapted from can-utils _put_id()/put_hex_byte()/snprintf_canframe(). */
    for (unsigned i = 0; i < width; i++) {
        tmp[pos + width - 1u - i] = hex_upper[id & 15u];
        id >>= 4;
    }
    pos += width;
    tmp[pos++] = '#';
    if (f->flags & CANF_RTR) {
        tmp[pos++] = 'R';
        if (f->len) tmp[pos++] = hex_upper[f->len];
    } else {
        if (f->flags & CANF_FD) {
            tmp[pos++] = '#';
            tmp[pos++] = hex_upper[((f->flags & CANF_BRS) ? 1 : 0) | ((f->flags & CANF_ESI) ? 2 : 0)];
        }
        for (unsigned i = 0; i < f->len; i++) {
            tmp[pos++] = hex_upper[f->data[i] >> 4];
            tmp[pos++] = hex_upper[f->data[i] & 15u];
        }
    }
    tmp[pos++] = '\n';
    tmp[pos] = 0;
    if (pos >= cap) return LOGFMT_ERR_SPACE;
    memcpy(buf, tmp, pos + 1u);
    return (int)pos;
}

const char *logfmt_result_name(logfmt_result_t r)
{
    switch (r) {
    case LOGFMT_OK: return "ok";
    case LOGFMT_SKIP: return "skip";
    case LOGFMT_ERR_SYNTAX: return "syntax";
    case LOGFMT_ERR_RANGE: return "range";
    case LOGFMT_ERR_LENGTH: return "length";
    case LOGFMT_ERR_SPACE: return "space";
    }
    return "unknown";
}
