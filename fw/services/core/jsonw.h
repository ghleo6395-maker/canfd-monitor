/* 极简 JSON 写入器：给模拟器快照和命令行 status 用。不分配内存，写满即截断。 */
#ifndef JSONW_H
#define JSONW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JW_MAX_DEPTH 12

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   overflow;
    int    depth;
    bool   need_comma[JW_MAX_DEPTH];
    bool   after_key;   /* 刚写完键名，下一个值不加逗号 */
} jw_t;

void   jw_init(jw_t *j, char *buf, size_t cap);
void   jw_obj(jw_t *j);              /* { */
void   jw_end_obj(jw_t *j);          /* } */
void   jw_arr(jw_t *j);              /* [ */
void   jw_end_arr(jw_t *j);          /* ] */
void   jw_key(jw_t *j, const char *k);
void   jw_str(jw_t *j, const char *s);
void   jw_int(jw_t *j, int64_t v);
void   jw_bool(jw_t *j, bool v);
size_t jw_finish(jw_t *j);

/* 便捷：键值对 */
void jw_kstr(jw_t *j, const char *k, const char *v);
void jw_kint(jw_t *j, const char *k, int64_t v);
void jw_kbool(jw_t *j, const char *k, bool v);

#endif
