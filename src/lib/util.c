/* util.c — small helpers */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int parse_ulong(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long v;

    if (!s || !*s || !isdigit((unsigned char)*s))
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || *end || v > max)
        return -1;
    *out = v;
    return 0;
}

bool parse_bool(const char *s, bool *out)
{
    if (!strcasecmp(s, "yes") || !strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "on")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(s, "no") || !strcasecmp(s, "false") || !strcmp(s, "0") || !strcasecmp(s, "off")) {
        *out = false;
        return true;
    }
    return false;
}

size_t str_copy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

bool str_endswith_ci(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

char *str_trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}
