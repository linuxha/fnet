/* log.c — leveled logging (REQ-110..REQ-113) */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *const lvl_names[] = { "error", "warn", "notice", "info", "debug", "trace" };
static const int lvl_syslog[] = { LOG_ERR, LOG_WARNING, LOG_NOTICE, LOG_INFO, LOG_DEBUG, LOG_DEBUG };

static int g_level = FL_INFO;
static enum log_sink g_sink = LOG_SINK_STDERR;
static FILE *g_file;
static char g_path[512];

int log_level_parse(const char *s)
{
    for (int i = 0; i < (int)(sizeof lvl_names / sizeof lvl_names[0]); i++)
        if (!strcasecmp(s, lvl_names[i]))
            return i;
    if (!strcasecmp(s, "warning"))
        return FL_WARN;
    return -1;
}

const char *log_level_name(int lvl)
{
    if (lvl < FL_ERROR || lvl > FL_TRACE)
        return "?";
    return lvl_names[lvl];
}

int log_sink_parse(const char *s)
{
    if (!strcasecmp(s, "stderr"))  return LOG_SINK_STDERR;
    if (!strcasecmp(s, "journal")) return LOG_SINK_JOURNAL;
    if (!strcasecmp(s, "syslog"))  return LOG_SINK_SYSLOG;
    if (!strcasecmp(s, "file"))    return LOG_SINK_FILE;
    return -1;
}

int log_open(enum log_sink sink, const char *file)
{
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    g_sink = sink;
    if (sink == LOG_SINK_SYSLOG)
        openlog("fnetd", LOG_PID, LOG_DAEMON);
    if (sink == LOG_SINK_FILE) {
        snprintf(g_path, sizeof g_path, "%s", file ? file : "");
        g_file = fopen(g_path, "a");
        if (!g_file) {
            g_sink = LOG_SINK_STDERR;
            return -1;
        }
        setvbuf(g_file, NULL, _IOLBF, 0);
    }
    return 0;
}

int log_reopen(void)
{
    if (g_sink != LOG_SINK_FILE)
        return 0;
    FILE *f = fopen(g_path, "a");
    if (!f)
        return -1;
    setvbuf(f, NULL, _IOLBF, 0);
    if (g_file)
        fclose(g_file);
    g_file = f;
    return 0;
}

void log_set_level(int lvl) { g_level = lvl; }
int  log_get_level(void)    { return g_level; }

bool log_enabled(int lvl, const struct log_ctx *c)
{
    int eff = (c && c->level >= 0) ? c->level : g_level;
    return lvl <= eff;
}

static void emit(int lvl, const struct log_ctx *c, const char *sub, const char *msg)
{
    const char *port = (c && c->name) ? c->name : "-";

    switch (g_sink) {
    case LOG_SINK_SYSLOG:
        syslog(lvl_syslog[lvl], "%s [%s] %s: %s", lvl_names[lvl], port, sub, msg);
        return;
    case LOG_SINK_JOURNAL:
        /* sd-daemon(3) priority prefix; journald adds its own µs timestamp. */
        fprintf(stderr, "<%d>%s [%s] %s: %s\n", lvl_syslog[lvl], lvl_names[lvl], port, sub, msg);
        return;
    case LOG_SINK_STDERR:
    case LOG_SINK_FILE: {
        struct timeval tv;
        struct tm tm;
        char ts[32];
        FILE *out = (g_sink == LOG_SINK_FILE && g_file) ? g_file : stderr;

        gettimeofday(&tv, NULL);
        localtime_r(&tv.tv_sec, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(out, "%s.%06ld %-6s [%s] %s: %s\n", ts, (long)tv.tv_usec,
                lvl_names[lvl], port, sub, msg);
        return;
    }
    }
}

void flog(int lvl, const struct log_ctx *c, const char *sub, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    if (lvl < FL_ERROR)
        lvl = FL_ERROR;
    if (lvl > FL_TRACE)
        lvl = FL_TRACE;
    if (!log_enabled(lvl, c))
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    emit(lvl, c, sub, msg);
}

void flog_hex(const struct log_ctx *c, const char *sub, const char *dir,
              const uint8_t *buf, size_t n)
{
    if (!log_enabled(FL_TRACE, c))
        return;
    for (size_t off = 0; off < n; off += 16) {
        char line[128];
        size_t len = 0, end = off + 16 < n ? off + 16 : n;

        len += (size_t)snprintf(line + len, sizeof line - len, "%s %04zx:", dir, off);
        for (size_t i = off; i < end; i++)
            len += (size_t)snprintf(line + len, sizeof line - len, " %02x", buf[i]);
        for (size_t i = end; i < off + 16; i++)
            len += (size_t)snprintf(line + len, sizeof line - len, "   ");
        len += (size_t)snprintf(line + len, sizeof line - len, "  |");
        for (size_t i = off; i < end && len < sizeof line - 2; i++)
            line[len++] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : '.';
        line[len++] = '|';
        line[len] = '\0';
        emit(FL_TRACE, c, sub, line);
    }
}
