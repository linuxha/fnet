/* config.c — fnetd.conf INI parser */
#include "config.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

static const unsigned tty_bauds[] = {
    50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600,
    19200, 38400, 57600, 115200, 230400, 460800,
};

const char *port_type_name(int t)
{
    switch (t) {
    case PT_TTY:     return "tty";
    case PT_RFC2217: return "rfc2217";
    case PT_LISTEN:  return "listen";
    }
    return "?";
}

void config_defaults(struct config *c)
{
    struct port_cfg *d = &c->defaults;

    memset(c, 0, sizeof *c);
    str_copy(c->user, "fnet", sizeof c->user);
    c->log_sink = LOG_SINK_JOURNAL;
    str_copy(c->log_file, "/var/log/fnetd/fnetd.log", sizeof c->log_file);
    c->log_level = FL_INFO;
    str_copy(c->stats_file, "/run/fnetd/stats.json", sizeof c->stats_file);
    c->stats_interval = 10;
    str_copy(c->ctl_socket, "/run/fnetd/ctl", sizeof c->ctl_socket);

    d->type = PT_TTY;
    d->enabled = true;
    d->on_new_replace = true;
    d->telnet = true;
    d->baud = 19200;
    str_copy(d->flexnet_root, "/srv/fnet/flex", sizeof d->flexnet_root);
    str_copy(d->tape_root, "/srv/fnet/tape", sizeof d->tape_root);
    str_copy(d->default_cpu, "6800", sizeof d->default_cpu);
    d->xfer_timeout_ms = 2000;
    d->sync_loss_count = 3;
    d->sync_loss_garbage = 16;
    d->sync_interval = 5;
    d->load_eol = EOL_CRLF;
    d->load_missing = MISS_S9;
    d->load_validate = VALID_WARN;
    d->punch_mode = PUNCH_OVERWRITE;
    d->punch_timeout = 30;
    d->legacy_drive = 0;
    d->log_level = -1;
}

int cidr_parse(const char *s, struct cidr *out)
{
    char buf[80], *slash;
    unsigned long bits;

    memset(out, 0, sizeof *out);
    str_copy(buf, s, sizeof buf);
    slash = strchr(buf, '/');
    if (slash)
        *slash++ = '\0';
    if (inet_pton(AF_INET, buf, out->addr) == 1) {
        out->family = AF_INET;
        out->bits = 32;
    } else if (inet_pton(AF_INET6, buf, out->addr) == 1) {
        out->family = AF_INET6;
        out->bits = 128;
    } else {
        return -1;
    }
    if (slash) {
        if (parse_ulong(slash, out->bits, &bits) < 0)
            return -1;
        out->bits = (unsigned)bits;
    }
    return 0;
}

bool cidr_match(const struct cidr *c, int family, const void *addr)
{
    const uint8_t *a = addr;
    uint8_t mapped[4];
    unsigned full, rem;

    /* IPv4-mapped IPv6 peer against an IPv4 rule */
    if (family == AF_INET6 && c->family == AF_INET &&
        !memcmp(a, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12)) {
        memcpy(mapped, a + 12, 4);
        a = mapped;
        family = AF_INET;
    }
    if (family != c->family)
        return false;
    full = c->bits / 8;
    rem = c->bits % 8;
    if (memcmp(a, c->addr, full))
        return false;
    if (rem) {
        uint8_t mask = (uint8_t)(0xffu << (8 - rem));
        if ((a[full] & mask) != (c->addr[full] & mask))
            return false;
    }
    return true;
}

static int set_str(char *dst, size_t sz, const char *val, const char *key, char *err, size_t errsz)
{
    if (str_copy(dst, val, sz) >= sz) {
        snprintf(err, errsz, "%s: value too long", key);
        return -1;
    }
    return 0;
}

static int set_uint(unsigned *dst, const char *val, unsigned long max, const char *key,
                    char *err, size_t errsz)
{
    unsigned long v;
    if (parse_ulong(val, max, &v) < 0) {
        snprintf(err, errsz, "%s: expected an integer 0..%lu", key, max);
        return -1;
    }
    *dst = (unsigned)v;
    return 0;
}

static int set_bool(bool *dst, const char *val, const char *key, char *err, size_t errsz)
{
    if (!parse_bool(val, dst)) {
        snprintf(err, errsz, "%s: expected yes/no", key);
        return -1;
    }
    return 0;
}

static int set_enum(int *dst, const char *val, const char *const *names, const char *key,
                    char *err, size_t errsz)
{
    for (int i = 0; names[i]; i++) {
        if (!strcasecmp(val, names[i])) {
            *dst = i;
            return 0;
        }
    }
    snprintf(err, errsz, "%s: invalid value '%s'", key, val);
    return -1;
}

static bool valid_tag(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 16)
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-')
            return false;
    return true;
}

static const char *const type_names[]  = { "tty", "rfc2217", "listen", NULL };
static const char *const eol_names[]   = { "crlf", "cr", "lf", "raw", NULL };
static const char *const miss_names[]  = { "s9", "none", NULL };
static const char *const valid_names[] = { "warn", "reject", "off", NULL };
static const char *const punch_names[] = { "overwrite", "append", "version", NULL };
static const char *const onnew_names[] = { "refuse", "replace", NULL };

static const char *const link_keys[] = {
    "type", "device", "host", "port", "bind", "allow", "on_new", "telnet", "baud",
    "flexnet_root", "tape_root", "enabled", NULL,
};

bool config_key_is_link(const char *key)
{
    for (int i = 0; link_keys[i]; i++)
        if (!strcmp(key, link_keys[i]))
            return true;
    return false;
}

int config_port_kv(struct port_cfg *pc, const char *key, const char *val, char *err, size_t errsz)
{
    int tmp;

    if (!strcmp(key, "type"))
        return set_enum(&pc->type, val, type_names, key, err, errsz);
    if (!strcmp(key, "enabled"))
        return set_bool(&pc->enabled, val, key, err, errsz);
    if (!strcmp(key, "device"))
        return set_str(pc->device, sizeof pc->device, val, key, err, errsz);
    if (!strcmp(key, "host"))
        return set_str(pc->host, sizeof pc->host, val, key, err, errsz);
    if (!strcmp(key, "port"))
        return set_uint(&pc->port, val, 65535, key, err, errsz);
    if (!strcmp(key, "bind"))
        return set_str(pc->bind, sizeof pc->bind, val, key, err, errsz);
    if (!strcmp(key, "allow")) {
        char buf[1024], *save = NULL, *tok;
        pc->nallow = 0;
        memset(pc->allow, 0, sizeof pc->allow);
        str_copy(buf, val, sizeof buf);
        for (tok = strtok_r(buf, " ,\t", &save); tok; tok = strtok_r(NULL, " ,\t", &save)) {
            if (pc->nallow == CFG_ALLOW_MAX) {
                snprintf(err, errsz, "allow: at most %d entries", CFG_ALLOW_MAX);
                return -1;
            }
            if (cidr_parse(tok, &pc->allow[pc->nallow]) < 0) {
                snprintf(err, errsz, "allow: invalid address '%s'", tok);
                return -1;
            }
            pc->nallow++;
        }
        return 0;
    }
    if (!strcmp(key, "on_new")) {
        if (set_enum(&tmp, val, onnew_names, key, err, errsz) < 0)
            return -1;
        pc->on_new_replace = tmp == 1;
        return 0;
    }
    if (!strcmp(key, "telnet"))
        return set_bool(&pc->telnet, val, key, err, errsz);
    if (!strcmp(key, "baud"))
        return set_uint(&pc->baud, val, 4000000, key, err, errsz);
    if (!strcmp(key, "flexnet_root"))
        return set_str(pc->flexnet_root, sizeof pc->flexnet_root, val, key, err, errsz);
    if (!strcmp(key, "tape_root"))
        return set_str(pc->tape_root, sizeof pc->tape_root, val, key, err, errsz);
    if (!strcmp(key, "default_cpu")) {
        if (!valid_tag(val)) {
            snprintf(err, errsz, "default_cpu: invalid processor tag '%s'", val);
            return -1;
        }
        return set_str(pc->default_cpu, sizeof pc->default_cpu, val, key, err, errsz);
    }
    if (!strcmp(key, "xfer_timeout_ms"))
        return set_uint(&pc->xfer_timeout_ms, val, 600000, key, err, errsz);
    if (!strcmp(key, "sync_loss_count"))
        return set_uint(&pc->sync_loss_count, val, 1000, key, err, errsz);
    if (!strcmp(key, "sync_loss_garbage"))
        return set_uint(&pc->sync_loss_garbage, val, 100000, key, err, errsz);
    if (!strcmp(key, "sync_interval"))
        return set_uint(&pc->sync_interval, val, 3600, key, err, errsz);
    if (!strcmp(key, "char_delay_us"))
        return set_uint(&pc->char_delay_us, val, 10000000, key, err, errsz);
    if (!strcmp(key, "line_delay_ms"))
        return set_uint(&pc->line_delay_ms, val, 60000, key, err, errsz);
    if (!strcmp(key, "load_eol"))
        return set_enum(&pc->load_eol, val, eol_names, key, err, errsz);
    if (!strcmp(key, "load_missing"))
        return set_enum(&pc->load_missing, val, miss_names, key, err, errsz);
    if (!strcmp(key, "load_validate"))
        return set_enum(&pc->load_validate, val, valid_names, key, err, errsz);
    if (!strcmp(key, "punch_mode"))
        return set_enum(&pc->punch_mode, val, punch_names, key, err, errsz);
    if (!strcmp(key, "punch_timeout"))
        return set_uint(&pc->punch_timeout, val, 86400, key, err, errsz);
    if (!strcmp(key, "read_only"))
        return set_bool(&pc->read_only, val, key, err, errsz);
    if (!strcmp(key, "legacy_drive"))
        return set_uint(&pc->legacy_drive, val, 3, key, err, errsz);
    if (!strcmp(key, "log_level")) {
        if (!strcasecmp(val, "inherit")) {
            pc->log_level = -1;
            return 0;
        }
        pc->log_level = log_level_parse(val);
        if (pc->log_level < 0) {
            snprintf(err, errsz, "log_level: invalid level '%s'", val);
            return -1;
        }
        return 0;
    }
    snprintf(err, errsz, "unknown key '%s'", key);
    return -1;
}

int config_port_validate(const struct port_cfg *pc, char *err, size_t errsz)
{
    switch (pc->type) {
    case PT_TTY: {
        bool ok = false;
        if (!pc->device[0]) {
            snprintf(err, errsz, "port %s: 'device' is required for type tty", pc->name);
            return -1;
        }
        for (size_t i = 0; i < ARRAY_LEN(tty_bauds); i++)
            ok |= tty_bauds[i] == pc->baud;
        if (!ok) {
            snprintf(err, errsz, "port %s: unsupported tty baud rate %u", pc->name, pc->baud);
            return -1;
        }
        break;
    }
    case PT_RFC2217:
        if (!pc->host[0] || !pc->port) {
            snprintf(err, errsz, "port %s: 'host' and 'port' are required for type rfc2217", pc->name);
            return -1;
        }
        break;
    case PT_LISTEN:
        if (!pc->bind[0] || !pc->port) {
            snprintf(err, errsz, "port %s: 'bind' and 'port' are required for type listen (REQ-130)",
                     pc->name);
            return -1;
        }
        break;
    }
    if (pc->baud < 50 || pc->baud > 4000000) {
        snprintf(err, errsz, "port %s: baud out of range", pc->name);
        return -1;
    }
    if (!pc->flexnet_root[0] || pc->flexnet_root[0] != '/' ||
        !pc->tape_root[0] || pc->tape_root[0] != '/') {
        snprintf(err, errsz, "port %s: flexnet_root and tape_root must be absolute paths", pc->name);
        return -1;
    }
    return 0;
}

bool port_cfg_equal(const struct port_cfg *a, const struct port_cfg *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static int daemon_kv(struct config *c, const char *key, const char *val, char *err, size_t errsz)
{
    if (!strcmp(key, "user"))
        return set_str(c->user, sizeof c->user, val, key, err, errsz);
    if (!strcmp(key, "log_sink")) {
        c->log_sink = log_sink_parse(val);
        if (c->log_sink < 0) {
            snprintf(err, errsz, "log_sink: invalid value '%s'", val);
            return -1;
        }
        return 0;
    }
    if (!strcmp(key, "log_file"))
        return set_str(c->log_file, sizeof c->log_file, val, key, err, errsz);
    if (!strcmp(key, "log_level")) {
        c->log_level = log_level_parse(val);
        if (c->log_level < 0) {
            snprintf(err, errsz, "log_level: invalid level '%s'", val);
            return -1;
        }
        return 0;
    }
    if (!strcmp(key, "stats_file"))
        return set_str(c->stats_file, sizeof c->stats_file, val, key, err, errsz);
    if (!strcmp(key, "stats_interval"))
        return set_uint(&c->stats_interval, val, 86400, key, err, errsz);
    if (!strcmp(key, "ctl_socket"))
        return set_str(c->ctl_socket, sizeof c->ctl_socket, val, key, err, errsz);
    snprintf(err, errsz, "unknown key '%s'", key);
    return -1;
}

enum section { SEC_NONE, SEC_DAEMON, SEC_DEFAULTS, SEC_TAPECPU, SEC_PORT };

/* Strip a ';' or '#' comment that starts a line or follows whitespace. */
static void strip_comment(char *s)
{
    for (char *p = s; *p; p++) {
        if ((*p == ';' || *p == '#') && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

int config_load(const char *path, struct config *c, char *err, size_t errsz)
{
    char line[2048], kerr[256];
    enum section sec = SEC_NONE;
    struct port_cfg *cur = NULL;
    bool defaults_used = false;
    unsigned lineno = 0;
    FILE *f;

    config_defaults(c);
    f = fopen(path, "r");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }

    /*
     * Ports inherit [defaults]. To allow [defaults] anywhere before the
     * first port, each port copies the defaults when its section starts.
     */
    while (fgets(line, sizeof line, f)) {
        char *s, *eq, *key, *val;

        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(err, errsz, "%s:%u: line too long", path, lineno);
            goto fail;
        }
        strip_comment(line);
        s = str_trim(line);
        if (!*s)
            continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end || end[1]) {
                snprintf(err, errsz, "%s:%u: malformed section header", path, lineno);
                goto fail;
            }
            *end = '\0';
            s = str_trim(s + 1);
            cur = NULL;
            if (!strcmp(s, "daemon")) {
                sec = SEC_DAEMON;
            } else if (!strcmp(s, "defaults")) {
                if (defaults_used) {
                    snprintf(err, errsz, "%s:%u: [defaults] must come before any [port.*]", path, lineno);
                    goto fail;
                }
                sec = SEC_DEFAULTS;
            } else if (!strcmp(s, "tape.cpu")) {
                sec = SEC_TAPECPU;
            } else if (!strncmp(s, "port.", 5) && s[5]) {
                struct port_cfg *np;
                if (strlen(s + 5) >= CFG_NAME_MAX) {
                    snprintf(err, errsz, "%s:%u: port name too long", path, lineno);
                    goto fail;
                }
                for (unsigned i = 0; i < c->nports; i++) {
                    if (!strcmp(c->ports[i].name, s + 5)) {
                        snprintf(err, errsz, "%s:%u: duplicate port '%s'", path, lineno, s + 5);
                        goto fail;
                    }
                }
                np = realloc(c->ports, (c->nports + 1) * sizeof *np);
                if (!np) {
                    snprintf(err, errsz, "out of memory");
                    goto fail;
                }
                c->ports = np;
                cur = &c->ports[c->nports++];
                *cur = c->defaults;
                str_copy(cur->name, s + 5, sizeof cur->name);
                defaults_used = true;
                sec = SEC_PORT;
            } else {
                snprintf(err, errsz, "%s:%u: unknown section [%s]", path, lineno, s);
                goto fail;
            }
            continue;
        }

        eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, errsz, "%s:%u: expected key = value", path, lineno);
            goto fail;
        }
        *eq = '\0';
        key = str_trim(s);
        val = str_trim(eq + 1);
        if (!*key) {
            snprintf(err, errsz, "%s:%u: empty key", path, lineno);
            goto fail;
        }

        kerr[0] = '\0';
        switch (sec) {
        case SEC_NONE:
            snprintf(err, errsz, "%s:%u: key outside of a section", path, lineno);
            goto fail;
        case SEC_DAEMON:
            if (daemon_kv(c, key, val, kerr, sizeof kerr) < 0)
                goto kfail;
            break;
        case SEC_DEFAULTS:
            if (!strcmp(key, "type") || !strcmp(key, "device") || !strcmp(key, "host") ||
                !strcmp(key, "port") || !strcmp(key, "bind")) {
                snprintf(kerr, sizeof kerr, "'%s' is not allowed in [defaults]", key);
                goto kfail;
            }
            if (config_port_kv(&c->defaults, key, val, kerr, sizeof kerr) < 0)
                goto kfail;
            break;
        case SEC_TAPECPU:
            if (c->nalias == CFG_ALIAS_MAX) {
                snprintf(kerr, sizeof kerr, "too many aliases");
                goto kfail;
            }
            if (!valid_tag(key) || !valid_tag(val)) {
                snprintf(kerr, sizeof kerr, "invalid processor tag");
                goto kfail;
            }
            str_copy(c->alias[c->nalias].tag, key, CFG_TAG_MAX);
            str_copy(c->alias[c->nalias].dir, val, CFG_TAG_MAX);
            c->nalias++;
            break;
        case SEC_PORT:
            if (config_port_kv(cur, key, val, kerr, sizeof kerr) < 0)
                goto kfail;
            break;
        }
        continue;
kfail:
        snprintf(err, errsz, "%s:%u: %s", path, lineno, kerr);
        goto fail;
    }
    fclose(f);

    for (unsigned i = 0; i < c->nports; i++) {
        if (config_port_validate(&c->ports[i], kerr, sizeof kerr) < 0) {
            snprintf(err, errsz, "%s: %s", path, kerr);
            config_free(c);
            return -1;
        }
    }
    return 0;

fail:
    fclose(f);
    config_free(c);
    return -1;
}

void config_free(struct config *c)
{
    free(c->ports);
    c->ports = NULL;
    c->nports = 0;
}
