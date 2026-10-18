/* json.c — minimal JSON emitter and flat-object parser */
#include "json.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jb_init(struct jb *j)
{
    memset(j, 0, sizeof *j);
}

void jb_free(struct jb *j)
{
    free(j->s);
    memset(j, 0, sizeof *j);
}

static void put(struct jb *j, const char *s, size_t n)
{
    if (j->oom)
        return;
    if (j->len + n + 1 > j->cap) {
        size_t nc = j->cap ? j->cap * 2 : 1024;
        char *ns;
        while (nc < j->len + n + 1)
            nc *= 2;
        ns = realloc(j->s, nc);
        if (!ns) {
            j->oom = true;
            return;
        }
        j->s = ns;
        j->cap = nc;
    }
    memcpy(j->s + j->len, s, n);
    j->len += n;
    j->s[j->len] = '\0';
}

static void puts_(struct jb *j, const char *s) { put(j, s, strlen(s)); }

/* Emit a separator before a value or key if needed. */
static void sep(struct jb *j)
{
    if (j->after_key) {
        j->after_key = false;
        return;
    }
    if (j->depth > 0) {
        if (!j->first[j->depth])
            puts_(j, ",");
        j->first[j->depth] = false;
    }
}

static void open_(struct jb *j, const char *c)
{
    sep(j);
    puts_(j, c);
    if (j->depth < (int)(sizeof j->first / sizeof j->first[0]) - 1)
        j->depth++;
    j->first[j->depth] = true;
}

static void close_(struct jb *j, const char *c)
{
    puts_(j, c);
    if (j->depth > 0)
        j->depth--;
}

void jb_obj(struct jb *j)     { open_(j, "{"); }
void jb_obj_end(struct jb *j) { close_(j, "}"); }
void jb_arr(struct jb *j)     { open_(j, "["); }
void jb_arr_end(struct jb *j) { close_(j, "]"); }

static void quoted(struct jb *j, const char *s)
{
    puts_(j, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"':  puts_(j, "\\\""); break;
        case '\\': puts_(j, "\\\\"); break;
        case '\n': puts_(j, "\\n"); break;
        case '\r': puts_(j, "\\r"); break;
        case '\t': puts_(j, "\\t"); break;
        default:
            if (*p < 0x20 || *p >= 0x7f) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                puts_(j, esc);
            } else {
                put(j, (const char *)p, 1);
            }
        }
    }
    puts_(j, "\"");
}

void jb_key(struct jb *j, const char *k)
{
    sep(j);
    quoted(j, k);
    puts_(j, ":");
    j->after_key = true;
}

void jb_str(struct jb *j, const char *v)
{
    sep(j);
    quoted(j, v ? v : "");
}

void jb_u64(struct jb *j, uint64_t v)
{
    char b[32];
    sep(j);
    snprintf(b, sizeof b, "%" PRIu64, v);
    puts_(j, b);
}

void jb_i64(struct jb *j, int64_t v)
{
    char b[32];
    sep(j);
    snprintf(b, sizeof b, "%" PRId64, v);
    puts_(j, b);
}

void jb_bool(struct jb *j, bool v) { sep(j); puts_(j, v ? "true" : "false"); }
void jb_null(struct jb *j)         { sep(j); puts_(j, "null"); }

void jb_kstr(struct jb *j, const char *k, const char *v) { jb_key(j, k); jb_str(j, v); }
void jb_ku64(struct jb *j, const char *k, uint64_t v)    { jb_key(j, k); jb_u64(j, v); }
void jb_ki64(struct jb *j, const char *k, int64_t v)     { jb_key(j, k); jb_i64(j, v); }
void jb_kbool(struct jb *j, const char *k, bool v)       { jb_key(j, k); jb_bool(j, v); }

/* ---------------------------------------------------------------- parser */

static const char *ws(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return s;
}

static const char *pstring(const char *s, char *out, size_t n)
{
    size_t o = 0;

    if (*s != '"')
        return NULL;
    s++;
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\') {
            c = *s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)s[i]))
                        return NULL;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)s[i]) ? s[i] - '0'
                                                                         : (tolower((unsigned char)s[i]) - 'a' + 10));
                }
                s += 4;
                c = v < 0x80 ? (char)v : '?';
                break;
            }
            case '"': case '\\': case '/': break;
            default: return NULL;
            }
        }
        if (o + 1 >= n)
            return NULL;
        out[o++] = c;
    }
    if (*s != '"')
        return NULL;
    out[o] = '\0';
    return s + 1;
}

int json_parse_flat(const char *s, struct jpair *out, int max)
{
    return json_parse_until(s, out, max, NULL);
}

int json_parse_until(const char *s, struct jpair *out, int max, const char *stop)
{
    int n = 0;

    s = ws(s);
    if (*s++ != '{')
        return -1;
    s = ws(s);
    if (*s == '}')
        return 0;
    for (;;) {
        struct jpair *p;

        if (n == max)
            return -1;
        p = &out[n];
        s = pstring(ws(s), p->key, sizeof p->key);
        if (!s)
            return -1;
        s = ws(s);
        if (*s++ != ':')
            return -1;
        s = ws(s);
        if (stop && !strcmp(p->key, stop))
            return n;
        if (*s == '"') {
            s = pstring(s, p->val, sizeof p->val);
            if (!s)
                return -1;
            p->type = J_STR;
        } else if (!strncmp(s, "true", 4) || !strncmp(s, "false", 5)) {
            bool t = *s == 't';
            strcpy(p->val, t ? "true" : "false");
            s += t ? 4 : 5;
            p->type = J_BOOL;
        } else if (!strncmp(s, "null", 4)) {
            p->val[0] = '\0';
            s += 4;
            p->type = J_NULL;
        } else if (*s == '-' || isdigit((unsigned char)*s)) {
            size_t o = 0;
            while ((*s == '-' || *s == '+' || *s == '.' || *s == 'e' || *s == 'E' ||
                    isdigit((unsigned char)*s)) && o + 1 < sizeof p->val)
                p->val[o++] = *s++;
            p->val[o] = '\0';
            p->type = J_NUM;
        } else {
            return -1;
        }
        n++;
        s = ws(s);
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s == '}')
            return n;
        return -1;
    }
}

const char *jp_get(const struct jpair *p, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(p[i].key, key))
            return p[i].val;
    return NULL;
}

bool jp_bool(const struct jpair *p, int n, const char *key)
{
    const char *v = jp_get(p, n, key);
    return v && (!strcmp(v, "true") || !strcmp(v, "1"));
}

long jp_long(const struct jpair *p, int n, const char *key, long dflt)
{
    const char *v = jp_get(p, n, key);
    char *end;
    long r;

    if (!v || !*v)
        return dflt;
    r = strtol(v, &end, 10);
    return *end ? dflt : r;
}

/* Skip one JSON value (scalar, string, object or array). */
static const char *skip_value(const char *s)
{
    int depth = 0;

    s = ws(s);
    do {
        if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1])
                    s++;
                s++;
            }
            if (*s != '"')
                return NULL;
            s++;
        } else if (*s == '{' || *s == '[') {
            depth++;
            s++;
        } else if (*s == '}' || *s == ']') {
            if (--depth < 0)
                return NULL;
            s++;
        } else if (*s == '\0') {
            return NULL;
        } else if (depth == 0) {
            while (*s && *s != ',' && *s != '}' && *s != ']' && !isspace((unsigned char)*s))
                s++;
        } else {
            s++;
        }
        s = ws(s);
    } while (depth > 0);
    return s;
}

int json_find_str(const char *s, const char *key, char *out, size_t n)
{
    char k[64];

    s = ws(s);
    if (*s++ != '{')
        return -1;
    for (;;) {
        s = ws(s);
        if (*s == '}')
            return -1;
        if (!(s = pstring(s, k, sizeof k)))
            return -1;
        s = ws(s);
        if (*s++ != ':')
            return -1;
        s = ws(s);
        if (!strcmp(k, key))
            return pstring(s, out, n) ? 0 : -1;
        if (!(s = skip_value(s)))
            return -1;
        if (*s == ',')
            s++;
    }
}
