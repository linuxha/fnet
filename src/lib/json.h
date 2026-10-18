/* json.h — minimal JSON emitter and flat-object parser (§4.2.4, REQ-114) */
#ifndef FNET_JSON_H
#define FNET_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct jb {
    char  *s;
    size_t len, cap;
    int    depth;
    bool   first[32];
    bool   after_key;
    bool   oom;
};

void jb_init(struct jb *j);
void jb_free(struct jb *j);
void jb_obj(struct jb *j);
void jb_obj_end(struct jb *j);
void jb_arr(struct jb *j);
void jb_arr_end(struct jb *j);
void jb_key(struct jb *j, const char *k);
void jb_str(struct jb *j, const char *v);
void jb_u64(struct jb *j, uint64_t v);
void jb_i64(struct jb *j, int64_t v);
void jb_bool(struct jb *j, bool v);
void jb_null(struct jb *j);

void jb_kstr(struct jb *j, const char *k, const char *v);
void jb_ku64(struct jb *j, const char *k, uint64_t v);
void jb_ki64(struct jb *j, const char *k, int64_t v);
void jb_kbool(struct jb *j, const char *k, bool v);

enum { J_STR, J_NUM, J_BOOL, J_NULL };

struct jpair {
    char key[64];
    char val[1024];
    int  type;
};

/* Parse {"k": scalar, ...}. Returns the pair count or -1. */
int json_parse_flat(const char *s, struct jpair *out, int max);
/* Like json_parse_flat but stops (successfully) at key 'stop'. */
int json_parse_until(const char *s, struct jpair *out, int max, const char *stop);
/* Find a top-level string value by key (skips nested values). */
int json_find_str(const char *s, const char *key, char *out, size_t n);
const char *jp_get(const struct jpair *p, int n, const char *key);
bool jp_bool(const struct jpair *p, int n, const char *key);
long jp_long(const struct jpair *p, int n, const char *key, long dflt);

#endif
