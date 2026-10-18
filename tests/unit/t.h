/* t.h — minimal unit test harness */
#ifndef FNET_T_H
#define FNET_T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_fail, t_run;

#define CHECK(cond) do { \
    t_run++; \
    if (!(cond)) { t_fail++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    t_run++; \
    if (_a != _b) { t_fail++; fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                                      __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    t_run++; \
    if (strcmp(_a, _b)) { t_fail++; fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", \
                                           __FILE__, __LINE__, _a, _b); } \
} while (0)

#define RUN(fn) do { fprintf(stderr, "- %s\n", #fn); fn(); } while (0)

#define T_DONE() do { \
    fprintf(stderr, "%d checks, %d failed\n", t_run, t_fail); \
    return t_fail ? 1 : 0; \
} while (0)

#endif
