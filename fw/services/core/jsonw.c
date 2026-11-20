#include "services/core/jsonw.h"

#include <stdio.h>

static void put(jw_t *j, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (j->len + 1 >= j->cap) {
            j->overflow = true;
            return;
        }
        j->buf[j->len++] = s[i];
    }
    j->buf[j->len] = '\0';
}

static void puts_(jw_t *j, const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    put(j, s, n);
}

/* 值之前：数组里的第二个起要加逗号；对象里的逗号在 jw_key 里加 */
static void before_value(jw_t *j, bool is_key_value)
{
    if (is_key_value)
        return;
    if (j->depth > 0 && j->need_comma[j->depth])
        put(j, ",", 1);
    if (j->depth > 0)
        j->need_comma[j->depth] = true;
}

void jw_init(jw_t *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->cap = cap;
    j->len = 0;
    j->overflow = false;
    j->depth = 0;
    j->need_comma[0] = false;
    j->after_key = false;
    if (cap)
        buf[0] = '\0';
}

static void open_(jw_t *j, char c)
{
    before_value(j, j->after_key);
    j->after_key = false;
    put(j, &c, 1);
    if (j->depth < JW_MAX_DEPTH - 1)
        j->depth++;
    j->need_comma[j->depth] = false;
}

static void close_(jw_t *j, char c)
{
    put(j, &c, 1);
    if (j->depth > 0)
        j->depth--;
}

void jw_obj(jw_t *j) { open_(j, '{'); }
void jw_end_obj(jw_t *j) { close_(j, '}'); }
void jw_arr(jw_t *j) { open_(j, '['); }
void jw_end_arr(jw_t *j) { close_(j, ']'); }

static void put_str(jw_t *j, const char *s);

void jw_key(jw_t *j, const char *k)
{
    if (j->need_comma[j->depth])
        put(j, ",", 1);
    j->need_comma[j->depth] = true;
    put_str(j, k);
    put(j, ":", 1);
    j->after_key = true;
}

void jw_str(jw_t *j, const char *s)
{
    before_value(j, j->after_key);
    j->after_key = false;
    put_str(j, s);
}

static void put_str(jw_t *j, const char *s)
{
    put(j, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*p;
            put(j, esc, 2);
        } else if (*p < 0x20) {
            int n = snprintf(esc, sizeof esc, "\\u%04x", *p);
            put(j, esc, (size_t)n);
        } else {
            put(j, (const char *)p, 1);
        }
    }
    put(j, "\"", 1);
}

void jw_int(jw_t *j, int64_t v)
{
    char tmp[24];
    before_value(j, j->after_key);
    j->after_key = false;
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    put(j, tmp, (size_t)n);
}

void jw_bool(jw_t *j, bool v)
{
    before_value(j, j->after_key);
    j->after_key = false;
    puts_(j, v ? "true" : "false");
}

size_t jw_finish(jw_t *j)
{
    return j->len;
}

void jw_kstr(jw_t *j, const char *k, const char *v) { jw_key(j, k); jw_str(j, v); }
void jw_kint(jw_t *j, const char *k, int64_t v) { jw_key(j, k); jw_int(j, v); }
void jw_kbool(jw_t *j, const char *k, bool v) { jw_key(j, k); jw_bool(j, v); }
