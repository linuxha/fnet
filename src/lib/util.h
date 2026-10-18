/* util.h — small helpers shared by fnetd, fnetctl and fnetimg */
#ifndef FNET_UTIL_H
#define FNET_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

uint64_t now_us(void);                 /* CLOCK_MONOTONIC, microseconds */
int  set_nonblock(int fd);
int  parse_ulong(const char *s, unsigned long max, unsigned long *out);
bool parse_bool(const char *s, bool *out);
size_t str_copy(char *dst, const char *src, size_t size);   /* strlcpy semantics */
bool str_endswith_ci(const char *s, const char *suffix);
char *str_trim(char *s);

#endif
