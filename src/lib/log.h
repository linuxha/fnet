/* log.h — leveled logging with per-port overrides (REQ-110..REQ-113) */
#ifndef FNET_LOG_H
#define FNET_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FL_ERROR, FL_WARN, FL_NOTICE, FL_INFO, FL_DEBUG, FL_TRACE };

enum log_sink { LOG_SINK_STDERR, LOG_SINK_JOURNAL, LOG_SINK_SYSLOG, LOG_SINK_FILE };

/* Per-port context. level < 0 inherits the global level. */
struct log_ctx {
    const char *name;
    int level;
};

int  log_level_parse(const char *s);           /* -1 if unknown */
const char *log_level_name(int lvl);
int  log_sink_parse(const char *s);            /* -1 if unknown */

int  log_open(enum log_sink sink, const char *file);
int  log_reopen(void);
void log_set_level(int lvl);
int  log_get_level(void);

bool log_enabled(int lvl, const struct log_ctx *c);
void flog(int lvl, const struct log_ctx *c, const char *sub, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void flog_hex(const struct log_ctx *c, const char *sub, const char *dir,
              const uint8_t *buf, size_t n);

#endif
