/* SPDX-License-Identifier: MIT */
#ifndef REVD_SEND_PLAN_JSON_H
#define REVD_SEND_PLAN_JSON_H
#include <stddef.h>
#include <stdint.h>
#include "sim/send_plan.h"
#define SP_JSON_MAX_BYTES 131072u
#define SP_JSON_OUTPUT_BYTES 8192u
/* Read up to capacity; 0 EOF, negative I/O error. Parser is single-owner,
 * streaming and heap-free, with strict v1 schema/UTF-8/range validation. */
typedef int (*sp_json_read_fn)(void *context,uint8_t *data,size_t capacity);
int sp_json_decode(sp_json_read_fn read,void *context,send_plan_t *out,
                   char project[48],char error[96]);
/* Compact UTF-8 JSON, compatible with the PC v1 schema. No output is accepted
 * unless all fields validate and the buffer fits. Size excludes final NUL. */
int sp_json_encode(const send_plan_t *plan,const char *project,
                   char *buffer,size_t capacity,size_t *length);
int sp_json_filename(const char *name);
int sp_json_safe_name(const char *name,size_t maximum,int allow_empty);
#endif
